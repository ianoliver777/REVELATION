#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace revelation
{
// All controls are 0..1. mix defaults to fully wet.
struct Controls { float weight{}, sheen{}, loud{}, bypass{}, mix{1.0f}, match{}; };

class Engine
{
public:
    static constexpr float ceilingDb = -1.0f;

    void prepare(double rate, Controls initial)
    {
        sampleRate = static_cast<float>(rate);
        smoothing = pole(14.0f);
        const auto nyquistSafe = [this](float hz) { return std::min(hz, 0.42f * sampleRate); };

        for (auto& c : channels)
        {
            c.weightLow.shelf(100.0f, sampleRate, 4.5f, false);
            c.weightHigh.shelf(nyquistSafe(7000.0f), sampleRate, -1.2f, true);
            c.sheenShelf.shelf(nyquistSafe(10000.0f), sampleRate, 4.5f, true);
            c.sheenBand.highpass(nyquistSafe(5000.0f), sampleRate);
            c.sheenHarm.highpass(nyquistSafe(7000.0f), sampleRate);
        }
        subSplit.lowpass(90.0f, sampleRate);
        subHarm.highpass(140.0f, sampleRate);
        sheenFollow = pole(50.0f);

        lookahead = std::max<std::size_t>(2, static_cast<std::size_t>(std::lround(0.0015f * sampleRate)) + 1);
        for (auto& line : delayLines)
            line.assign(lookahead, 0.0f);
        holdValues.assign(lookahead, 1.0f);
        holdIndex.assign(lookahead, 0);
        boxValues.assign(lookahead, 1.0f);
        release = pole(1.0f / 0.12f);           // ~120 ms recovery
        loudnessPole = pole(1.0f / 0.4f);       // ~400 ms loudness window
        matchPole = pole(1.0f / 0.08f);
        reset(initial);
    }

    // Samples of delay added by the limiter lookahead (identical on every path).
    int latency() const { return static_cast<int>(lookahead) - 1; }
    float gainReductionDb() const { return lastReductionDb; }

    void reset(Controls initial)
    {
        current = initial;
        for (auto& c : channels)
            c.clear();
        subSplit.clear();
        subHarm.clear();
        for (auto& line : delayLines)
            std::fill(line.begin(), line.end(), 0.0f);
        writeIndex = 0;
        std::fill(holdValues.begin(), holdValues.end(), 1.0f);
        std::fill(holdIndex.begin(), holdIndex.end(), 0);
        holdHead = holdTail = 0;
        holdCount = 0;
        clock = 0;
        released = 1.0;
        std::fill(boxValues.begin(), boxValues.end(), 1.0f);
        boxSum = static_cast<double>(lookahead);
        boxIndex = 0;
        dryPower = wetPower = 0.0f;
        matchGain = 1.0f;
        lastReductionDb = 0.0f;
    }

    void process(float* left, float* right, int samples, Controls target)
    {
        const bool stereo = right != nullptr;
        const auto ceiling = std::pow(10.0f, ceilingDb / 20.0f);
        float maxReduction = 0.0f;
        for (int i = 0; i < samples; ++i)
        {
            smooth(current.weight, target.weight);
            smooth(current.sheen, target.sheen);
            smooth(current.loud, target.loud);
            smooth(current.bypass, target.bypass);
            smooth(current.mix, target.mix);
            smooth(current.match, target.match);

            const float inL = left[i];
            const float inR = stereo ? right[i] : inL;
            float l = inL, r = inR;

            // WEIGHT: low shelf lift, slight top tilt down, and sub harmonics on the mid channel.
            if (current.weight > 0.0f)
            {
                const auto w = current.weight;
                l += w * (channels[0].weightHigh.run(channels[0].weightLow.run(inL)) - inL);
                r += w * (channels[1].weightHigh.run(channels[1].weightLow.run(inR)) - inR);
                const auto mid = 0.5f * (inL + inR);
                const auto sub = subSplit.run(mid);
                const auto drive = 1.0f + 5.0f * w;
                const auto driven = drive * sub;
                const auto harmonics = subHarm.run(driven / std::sqrt(1.0f + driven * driven) / drive - sub);
                const auto add = -1.6f * w * harmonics;
                l += add;
                r += add;
            }
            else
                keepFiltersWarm(inL, inR);

            // SHEEN: air shelf plus level-normalised second-harmonic sparkle.
            if (current.sheen > 0.0f)
            {
                const auto s = current.sheen;
                l += s * (channels[0].sheenShelf.run(l) - l + sparkle(channels[0], l));
                if (stereo)
                    r += s * (channels[1].sheenShelf.run(r) - r + sparkle(channels[1], r));
                else
                    r = l;
            }

            // LOUD: drive into a lookahead peak limiter with a -1 dBFS ceiling.
            const auto drive = dbToGain(10.0f * current.loud);
            l *= drive;
            r *= drive;
            if (current.loud > 0.0f)
            {
                const auto blend = 0.45f * current.loud;
                l += blend * (softClip(l, ceiling) - l);
                r += blend * (softClip(r, ceiling) - r);
            }
            const auto peak = std::max(std::abs(l), std::abs(r));
            const auto required = peak > ceiling ? ceiling / peak : 1.0f;
            const auto held = pushHold(required);
            released = held < released ? held : released + release * (held - released);
            const auto limiterGain = box(static_cast<float>(released));

            // Delay wet and dry by the lookahead so every path stays aligned.
            float wetL, wetR, dryL, dryR;
            readWrite(l, r, inL, inR, wetL, wetR, dryL, dryR);
            const auto engage = std::min(1.0f, current.loud * 20.0f);
            const auto gain = 1.0f + engage * (limiterGain - 1.0f);
            wetL *= gain;
            wetR *= gain;
            if (engage > 0.0f)
            {
                wetL = std::clamp(wetL, -ceiling, ceiling);
                wetR = std::clamp(wetR, -ceiling, ceiling);
                maxReduction = std::max(maxReduction, -20.0f * std::log10(std::max(gain, 1.0e-6f)));
            }

            const bool neutral = current.weight == 0.0f && current.sheen == 0.0f && current.loud == 0.0f;
            float outL = neutral ? dryL : dryL + current.mix * (wetL - dryL);
            float outR = neutral ? dryR : dryR + current.mix * (wetR - dryR);

            // MATCH: turn the processed signal down to the input's loudness for honest A/B.
            follow(dryPower, 0.5f * (dryL * dryL + dryR * dryR), loudnessPole);
            follow(wetPower, 0.5f * (outL * outL + outR * outR), loudnessPole);
            const auto wanted = wetPower > 1.0e-9f ? std::clamp(std::sqrt(dryPower / wetPower), 0.0625f, 1.0f) : 1.0f;
            follow(matchGain, wanted, matchPole);
            if (current.match > 0.0f && !neutral)
            {
                const auto g = 1.0f + current.match * (matchGain - 1.0f);
                outL *= g;
                outR *= g;
            }

            if (current.bypass == 1.0f)
            {
                outL = dryL;
                outR = dryR;
            }
            else if (current.bypass > 0.0f)
            {
                outL += current.bypass * (dryL - outL);
                outR += current.bypass * (dryR - outR);
            }
            left[i] = outL;
            if (stereo)
                right[i] = outR;
        }
        lastReductionDb = maxReduction;
    }

private:
    // Fixed-coefficient 2nd-order Butterworth section (RBJ); blended in parallel so controls stay cheap.
    struct Biquad
    {
        float b0{}, b1{}, b2{}, a1{}, a2{}, z1{}, z2{};
        void design(float hz, float rate, bool high)
        {
            const auto w = 6.283185307f * hz / rate;
            const auto cosw = std::cos(w);
            const auto alpha = std::sin(w) / (2.0f * 0.70710678f);
            const auto a0 = 1.0f + alpha;
            if (high)
            {
                b0 = (1.0f + cosw) * 0.5f / a0;
                b1 = -(1.0f + cosw) / a0;
            }
            else
            {
                b0 = (1.0f - cosw) * 0.5f / a0;
                b1 = (1.0f - cosw) / a0;
            }
            b2 = b0;
            a1 = -2.0f * cosw / a0;
            a2 = (1.0f - alpha) / a0;
        }
        // RBJ shelving EQ, slope 1. Designed once at full gain; the knob blends towards it.
        void shelf(float hz, float rate, float gainDb, bool high)
        {
            const auto a = std::pow(10.0f, gainDb / 40.0f);
            const auto w = 6.283185307f * hz / rate;
            const auto cosw = std::cos(w);
            const auto alpha = std::sin(w) / 2.0f * std::sqrt(2.0f);
            const auto root = 2.0f * std::sqrt(a) * alpha;
            const auto sign = high ? -1.0f : 1.0f;
            const auto a0 = (a + 1.0f) + sign * (a - 1.0f) * cosw + root;
            b0 = a * ((a + 1.0f) - sign * (a - 1.0f) * cosw + root) / a0;
            b1 = sign * 2.0f * a * ((a - 1.0f) - sign * (a + 1.0f) * cosw) / a0;
            b2 = a * ((a + 1.0f) - sign * (a - 1.0f) * cosw - root) / a0;
            a1 = -sign * 2.0f * ((a - 1.0f) + sign * (a + 1.0f) * cosw) / a0;
            a2 = ((a + 1.0f) + sign * (a - 1.0f) * cosw - root) / a0;
        }
        void lowpass(float hz, float rate) { design(hz, rate, false); }
        void highpass(float hz, float rate) { design(hz, rate, true); }
        void clear() { z1 = z2 = 0.0f; }
        float run(float x)
        {
            const auto y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    struct Channel
    {
        Biquad weightLow, weightHigh, sheenShelf, sheenBand, sheenHarm;
        float sheenEnvelope{};
        void clear()
        {
            for (auto* f : {&weightLow, &weightHigh, &sheenShelf, &sheenBand, &sheenHarm})
                f->clear();
            sheenEnvelope = 0.0f;
        }
    };

    float pole(float hz) const { return 1.0f - std::exp(-6.283185307f * hz / sampleRate); }
    static float dbToGain(float db) { return std::pow(10.0f, db * 0.05f); }
    static void follow(float& state, float target, float coefficient) { state += coefficient * (target - state); }

    void smooth(float& state, float target) const
    {
        follow(state, target, smoothing);
        if (std::abs(state - target) < 0.00001f)
            state = target;
    }

    static float softClip(float x, float ceiling)
    {
        const auto u = x / ceiling;
        return ceiling * u / std::sqrt(1.0f + u * u);
    }

    // Keeps WEIGHT's filters running while the knob is at zero so turning it up never clicks.
    void keepFiltersWarm(float inL, float inR)
    {
        channels[0].weightHigh.run(channels[0].weightLow.run(inL));
        channels[1].weightHigh.run(channels[1].weightLow.run(inR));
        subSplit.run(0.5f * (inL + inR));
        subHarm.run(0.0f);
    }

    float sparkle(Channel& c, float x)
    {
        const auto band = c.sheenBand.run(x);
        follow(c.sheenEnvelope, std::abs(band), sheenFollow);
        return 0.5f * c.sheenHarm.run(band * band / (c.sheenEnvelope + 0.02f));
    }

    // Sliding minimum over the lookahead window (monotonic queue).
    float pushHold(float value)
    {
        const auto n = holdValues.size();
        // Drop values that left the window first, so the queue never holds more than n entries.
        while (holdCount > 0 && holdIndex[holdHead] + n <= clock)
        {
            holdHead = (holdHead + 1) % n;
            --holdCount;
        }
        while (holdCount > 0)
        {
            const auto last = (holdTail + n - 1) % n;
            if (holdValues[last] < value)
                break;
            holdTail = last;
            --holdCount;
        }
        holdValues[holdTail] = value;
        holdIndex[holdTail] = clock;
        holdTail = (holdTail + 1) % n;
        ++holdCount;
        ++clock;
        return holdValues[holdHead];
    }

    // Moving average over the lookahead window: a smooth attack that lands before each peak.
    float box(float value)
    {
        boxSum += static_cast<double>(value) - boxValues[boxIndex];
        boxValues[boxIndex] = value;
        if (++boxIndex == boxValues.size())
        {
            boxIndex = 0;
            boxSum = 0.0;
            for (auto v : boxValues)
                boxSum += v;
        }
        return static_cast<float>(boxSum / static_cast<double>(boxValues.size()));
    }

    void readWrite(float l, float r, float dl, float dr, float& wetL, float& wetR, float& dryL, float& dryR)
    {
        // Each line holds `lookahead` samples, so reading before writing gives lookahead - 1 samples of delay.
        const auto n = lookahead;
        const auto oldest = (writeIndex + 1) % n;
        delayLines[0][writeIndex] = l;
        delayLines[1][writeIndex] = r;
        delayLines[2][writeIndex] = dl;
        delayLines[3][writeIndex] = dr;
        wetL = delayLines[0][oldest];
        wetR = delayLines[1][oldest];
        dryL = delayLines[2][oldest];
        dryR = delayLines[3][oldest];
        writeIndex = oldest;
    }

    Controls current;
    std::array<Channel, 2> channels;
    Biquad subSplit, subHarm;
    std::array<std::vector<float>, 4> delayLines;
    std::vector<float> holdValues, boxValues;
    std::vector<std::size_t> holdIndex;
    std::size_t lookahead{2}, writeIndex{}, holdHead{}, holdTail{}, holdCount{}, clock{}, boxIndex{};
    double released{1.0}, boxSum{};
    float sampleRate{48000.0f}, smoothing{}, sheenFollow{}, release{}, loudnessPole{}, matchPole{};
    float dryPower{}, wetPower{}, matchGain{1.0f}, lastReductionDb{};
};
}
