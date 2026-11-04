#include "Engine.h"
#include <cstdlib>
#include <iostream>

using revelation::Controls;
using revelation::Engine;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

static float peakOf(const std::vector<float>& a, int from)
{
    float p = 0.0f;
    for (size_t i = static_cast<size_t>(from); i < a.size(); ++i)
        p = std::max(p, std::abs(a[i]));
    return p;
}

static double rmsOf(const std::vector<float>& a, int from)
{
    double sum = 0.0;
    for (size_t i = static_cast<size_t>(from); i < a.size(); ++i)
        sum += static_cast<double>(a[i]) * a[i];
    return std::sqrt(sum / static_cast<double>(a.size() - static_cast<size_t>(from)));
}

int main()
{
    const auto ceiling = std::pow(10.0f, Engine::ceilingDb / 20.0f);
    for (const auto rate : {44100.0, 48000.0, 96000.0, 192000.0})
    {
        const int n = static_cast<int>(rate * 1.5);
        std::vector<float> l(static_cast<size_t>(n)), r(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
        {
            const auto t = static_cast<float>(i / rate);
            l[static_cast<size_t>(i)] = 0.45f * std::sin(6.2831853f * 55.0f * t) + 0.3f * std::sin(6.2831853f * 3100.0f * t)
                + 0.2f * std::sin(6.2831853f * 9000.0f * t);
            r[static_cast<size_t>(i)] = 0.40f * std::sin(6.2831853f * 55.0f * t + 0.3f) + 0.3f * std::sin(6.2831853f * 2300.0f * t)
                - 0.2f * std::sin(6.2831853f * 11000.0f * t);
        }
        const auto dryL = l;
        const auto dryR = r;
        Engine engine;
        engine.prepare(rate, {});
        const int d = engine.latency();
        require(d >= 1 && d <= static_cast<int>(rate * 0.002), "Latency should be about 1.5 ms");

        const auto delayedMatches = [&](const std::vector<float>& outL, const std::vector<float>& outR) {
            for (int i = 0; i < n; ++i)
            {
                const auto expectL = i >= d ? dryL[static_cast<size_t>(i - d)] : 0.0f;
                const auto expectR = i >= d ? dryR[static_cast<size_t>(i - d)] : 0.0f;
                if (outL[static_cast<size_t>(i)] != expectL || outR[static_cast<size_t>(i)] != expectR)
                    return false;
            }
            return true;
        };

        engine.process(l.data(), r.data(), n, {});
        require(delayedMatches(l, r), "Init must pass audio through bit for bit (delayed by the reported latency)");

        for (auto setting : {Controls{1, 1, 1, 1}, Controls{1, 1, 1, 0, 0.0f}})
        {
            l = dryL;
            r = dryR;
            engine.reset(setting);
            engine.process(l.data(), r.data(), n, setting);
            require(delayedMatches(l, r), "Bypass and MIX 0% must pass audio through bit for bit");
        }

        for (auto setting : {Controls{1, 0, 0, 0}, Controls{0, 1, 0, 0}, Controls{0, 0, 1, 0}})
        {
            l = dryL;
            r = dryR;
            engine.reset(setting);
            engine.process(l.data(), r.data(), n, setting);
            require(!delayedMatches(l, r), "Every knob must change the audio");
            for (int i = 0; i < n; ++i)
                require(std::isfinite(l[static_cast<size_t>(i)]) && std::isfinite(r[static_cast<size_t>(i)]), "Output must stay finite");
        }

        // LOUD at full must hold the ceiling while making the track louder.
        // Mix at a realistic level (about -6 dBFS peak), then drive it.
        Controls loud{0.6f, 0.6f, 1, 0};
        auto mixL = dryL;
        auto mixR = dryR;
        for (auto& v : mixL) v *= 0.5f;
        for (auto& v : mixR) v *= 0.5f;
        l = mixL;
        r = mixR;
        engine.reset(loud);
        engine.process(l.data(), r.data(), n, loud);
        require(std::max(peakOf(l, d), peakOf(r, d)) <= ceiling + 1.0e-6f, "Limiter exceeded the -1 dBFS ceiling");
        require(rmsOf(l, n / 3) > 1.6 * rmsOf(mixL, n / 3), "LOUD should raise loudness by 4 dB or more");
        l = dryL;
        r = dryR;
        engine.reset(loud);
        engine.process(l.data(), r.data(), n, loud);
        require(std::max(peakOf(l, d), peakOf(r, d)) <= ceiling + 1.0e-6f, "Limiter exceeded the ceiling on a hot input");
        require(engine.gainReductionDb() > 0.5f, "Gain reduction should be reported");

        // A single isolated spike must be caught by the lookahead, not clipped late.
        std::vector<float> spike(static_cast<size_t>(n), 0.05f);
        spike[static_cast<size_t>(n / 2)] = 1.5f;
        auto spikeR = spike;
        engine.reset({0, 0, 0.2f, 0});
        engine.process(spike.data(), spikeR.data(), n, {0, 0, 0.2f, 0});
        require(peakOf(spike, d) <= ceiling + 1.0e-6f, "Spike passed the ceiling");

        // MATCH turns the processed result down to about the input loudness.
        Controls matched{0.6f, 0.6f, 1, 0, 1, 1};
        l = dryL;
        r = dryR;
        engine.reset(matched);
        engine.process(l.data(), r.data(), n, matched);
        const auto ratio = rmsOf(l, n / 2) / rmsOf(dryL, n / 2);
        require(ratio > 0.85 && ratio < 1.15, "MATCH should land within about 1 dB of the input loudness");

        // Mono bus.
        auto mono = dryL;
        engine.reset({0.8f, 0.8f, 0.8f, 0});
        engine.process(mono.data(), nullptr, n, {0.8f, 0.8f, 0.8f, 0});
        require(peakOf(mono, d) <= ceiling + 1.0e-6f, "Mono limiter exceeded the ceiling");
        require(mono != dryL, "Mono processing must work");
    }
    std::cout << "REVELATION DSP checks passed at 44.1, 48, 96 and 192 kHz\n";
}
