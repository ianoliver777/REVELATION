#include <juce_audio_utils/juce_audio_utils.h>
#include <BinaryData.h>
#include "Engine.h"

namespace
{
using namespace juce;
constexpr std::array ids{"weight", "sheen", "loud"};
constexpr std::array names{"WEIGHT", "SHEEN", "LOUD"};
constexpr std::array presetNames{"INIT PRESET", "STREAMING READY", "CLUB WEIGHT", "AIRY POLISH", "LOUD & PROUD", "CUSTOM"};
constexpr std::array<std::array<float, 3>, 5> presets{{
    {0, 0, 0}, {30, 35, 55}, {68, 25, 72}, {15, 72, 35}, {45, 45, 92}
}};
const auto accentWeight = Colour(0xffffa940);
const auto accentSheen = Colour(0xff9fe3ff);
const auto accentLoud = Colour(0xffff6b5e);

class Processor final : public AudioProcessor
{
public:
    Processor() : AudioProcessor(BusesProperties().withInput("Input", AudioChannelSet::stereo(), true)
                                   .withOutput("Output", AudioChannelSet::stereo(), true)),
                  state(*this, nullptr, "REVELATION", layout())
    {
        for (size_t i = 0; i < ids.size(); ++i)
            values[i] = state.getRawParameterValue(ids[i]);
        bypass = state.getRawParameterValue("bypass");
        mix = state.getRawParameterValue("mix");
        match = state.getRawParameterValue("match");
        engine.prepare(44100.0, controls());
        setLatencySamples(engine.latency());
    }

    static AudioProcessorValueTreeState::ParameterLayout layout()
    {
        AudioProcessorValueTreeState::ParameterLayout result;
        for (size_t i = 0; i < ids.size(); ++i)
            result.add(std::make_unique<AudioParameterFloat>(
                ParameterID{ids[i], 1}, names[i], NormalisableRange<float>{0, 100, 0.1f}, 0.0f,
                AudioParameterFloatAttributes().withLabel("%")));
        result.add(std::make_unique<AudioParameterBool>(ParameterID{"bypass", 1}, "Bypass", false));
        result.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"mix", 1}, "MIX", NormalisableRange<float>{0, 100, 0.1f}, 100.0f,
            AudioParameterFloatAttributes().withLabel("%")));
        result.add(std::make_unique<AudioParameterBool>(ParameterID{"match", 1}, "MATCH", false));
        return result;
    }

    const String getName() const override { return "REVELATION"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    bool hasEditor() const override { return true; }
    AudioProcessorEditor* createEditor() override;
    AudioProcessorParameter* getBypassParameter() const override { return state.getParameter("bypass"); }

    bool isBusesLayoutSupported(const BusesLayout& b) const override
    {
        const auto out = b.getMainOutputChannelSet();
        return out == b.getMainInputChannelSet()
            && (out == AudioChannelSet::mono() || out == AudioChannelSet::stereo());
    }

    void prepareToPlay(double rate, int) override
    {
        engine.prepare(rate, controls());
        setLatencySamples(engine.latency());
    }
    void releaseResources() override {}
    void reset() override { engine.reset(controls()); }

    void processBlock(AudioBuffer<float>& buffer, MidiBuffer&) override { render(buffer, false); }
    void processBlockBypassed(AudioBuffer<float>& buffer, MidiBuffer&) override { render(buffer, true); }

    void render(AudioBuffer<float>& buffer, bool hostBypass)
    {
        ScopedNoDenormals noDenormals;
        const auto count = buffer.getNumSamples();
        if (count == 0 || buffer.getNumChannels() == 0)
            return;
        const bool stereo = buffer.getNumChannels() > 1;
        const auto peak = [&] {
            return jmax(buffer.getMagnitude(0, 0, count), stereo ? buffer.getMagnitude(1, 0, count) : 0.0f);
        };
        const auto publish = [](std::atomic<float>& meter, float value) {
            auto previous = meter.load(std::memory_order_relaxed);
            while (previous < value && !meter.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {}
        };
        publish(inputPeak, peak());
        auto target = controls();
        if (hostBypass)
            target.bypass = 1.0f;
        engine.process(buffer.getWritePointer(0), stereo ? buffer.getWritePointer(1) : nullptr, count, target);
        publish(outputPeak, peak());
        publish(reduction, engine.gainReductionDb());
    }

    int getNumPrograms() override { return static_cast<int>(presetNames.size()); }
    int getCurrentProgram() override
    {
        for (size_t i = 0; i < presets.size(); ++i)
        {
            if (std::abs(values[0]->load() - presets[i][0]) < 0.01f && std::abs(values[1]->load() - presets[i][1]) < 0.01f
                && std::abs(values[2]->load() - presets[i][2]) < 0.01f)
                return static_cast<int>(i);
        }
        return static_cast<int>(presets.size());
    }
    const String getProgramName(int index) override
    {
        return isPositiveAndBelow(index, getNumPrograms()) ? presetNames[static_cast<size_t>(index)] : "";
    }
    void changeProgramName(int, const String&) override {}
    void setCurrentProgram(int index) override
    {
        if (!isPositiveAndBelow(index, static_cast<int>(presets.size())))
            return;
        for (size_t i = 0; i < ids.size(); ++i)
        {
            auto* parameter = state.getParameter(ids[i]);
            parameter->beginChangeGesture();
            parameter->setValueNotifyingHost(parameter->convertTo0to1(presets[static_cast<size_t>(index)][i]));
            parameter->endChangeGesture();
        }
    }

    void getStateInformation(MemoryBlock& bytes) override
    {
        if (const auto xml = state.copyState().createXml())
            copyXmlToBinary(*xml, bytes);
    }
    void setStateInformation(const void* data, int size) override { restore(data, size); }
    bool restore(const void* data, int size)
    {
        if (const auto xml = getXmlFromBinary(data, size))
        {
            const auto tree = ValueTree::fromXml(*xml);
            if (!tree.hasType(state.state.getType()))
                return false;
            const std::array required{"weight", "sheen", "loud", "bypass", "mix", "match"};
            for (auto id : required)
            {
                const auto node = tree.getChildWithProperty("id", id);
                const auto value = node.getProperty("value").toString();
                const auto first = value.getCharPointer();
                auto end = first;
                const auto number = CharacterFunctions::readDoubleValue(end);
                const bool toggle = String(id) == "bypass" || String(id) == "match";
                if (!node.hasType("PARAM") || !value.containsAnyOf("0123456789")
                    || end == first || !end.isEmpty() || !std::isfinite(number)
                    || number < 0.0 || number > (toggle ? 1.0 : 100.0))
                    return false;
            }
            state.replaceState(tree);
            return true;
        }
        return false;
    }

    AudioProcessorValueTreeState state;
    std::atomic<float> inputPeak{}, outputPeak{}, reduction{};

private:
    revelation::Controls controls() const
    {
        return {values[0]->load() * 0.01f, values[1]->load() * 0.01f, values[2]->load() * 0.01f,
                bypass->load(), mix->load() * 0.01f, match->load()};
    }
    revelation::Engine engine;
    std::array<std::atomic<float>*, 3> values{};
    std::atomic<float>* bypass{};
    std::atomic<float>* mix{};
    std::atomic<float>* match{};
};

Image loadLogo()
{
    auto original = ImageFileFormat::loadFrom(BinaryData::mark_png, BinaryData::mark_pngSize);
    if (!original.isValid())
        return {};
    original = original.rescaled(320, roundToInt(320.0f * original.getHeight() / original.getWidth()));
    Image logo(Image::ARGB, original.getWidth(), original.getHeight(), true);
    for (int y = 0; y < logo.getHeight(); ++y)
        for (int x = 0; x < logo.getWidth(); ++x)
        {
            const auto mask = original.getPixelAt(x, y).getBrightness();
            const auto metal = Colour(0xfffff6e4).interpolatedWith(Colour(0xff7a6a52), 0.15f + 0.55f * y / logo.getHeight());
            logo.setPixelAt(x, y, metal.withAlpha(jlimit(0.0f, 0.86f, (mask - 0.10f) * 0.96f)));
        }
    return logo;
}

class KnobLook final : public LookAndFeel_V4
{
public:
    KnobLook() : logo(loadLogo()) {}

    void drawButtonText(Graphics& g, TextButton& button, bool, bool) override
    {
        const auto bounds = button.getLocalBounds().toFloat();
        const auto c = bounds.getCentre();
        const auto side = bounds.getHeight();
        const auto label = button.getButtonText();
        g.setColour(Colour(0xfffff3e0));
        if (label == "<" || label == ">")
        {
            const auto direction = label == "<" ? 1.0f : -1.0f;
            Path chevron;
            chevron.startNewSubPath(c.x + direction * side * 0.08f, c.y - side * 0.17f);
            chevron.lineTo(c.x - direction * side * 0.10f, c.y);
            chevron.lineTo(c.x + direction * side * 0.08f, c.y + side * 0.17f);
            g.strokePath(chevron, PathStrokeType(side * 0.085f, PathStrokeType::curved, PathStrokeType::rounded));
        }
        else if (label == "SAVE")
        {
            const auto box = Rectangle<float>(side * 0.52f, side * 0.52f).withCentre(c);
            g.drawRoundedRectangle(box, 1.5f, side * 0.065f);
            g.drawLine(box.getX() + side * 0.09f, c.y, box.getRight() - side * 0.09f, c.y, side * 0.06f);
            g.fillRect(c.x - side * 0.11f, c.y + side * 0.08f, side * 0.22f, side * 0.12f);
        }
        else if (label == "...")
        {
            for (int dot = -1; dot <= 1; ++dot)
                g.fillEllipse(Rectangle<float>(side * 0.115f, side * 0.115f)
                                  .withCentre({c.x + dot * side * 0.19f, c.y}));
        }
        else
        {
            if (button.getToggleState())
                g.setColour(Colour(0xff15100a));
            g.setFont(FontOptions(side * (label == "MATCH" ? 0.36f : 0.40f)).withKerningFactor(label == "MATCH" ? 0.12f : 0.0f));
            g.drawFittedText(label, button.getLocalBounds().reduced(2), Justification::centred, 1, 0.5f);
        }
    }

    void drawButtonBackground(Graphics& g, Button& button, const Colour&, bool over, bool down) override
    {
        auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
        const auto radius = bounds.getHeight() * 0.5f;
        if (button.getButtonText() == "MATCH")
        {
            const auto on = button.getToggleState();
            g.setColour(on ? Colour(0xffffcf7a) : Colour(0xff15120f).withAlpha(over ? 1.0f : 0.9f));
            g.fillRoundedRectangle(bounds, radius);
            g.setColour(on ? Colour(0xfffff1d0) : Colour(0x88f4e4c8));
            g.drawRoundedRectangle(bounds, radius, 1.2f);
            return;
        }
        g.setColour(Colour(0xff120f0c).brighter(down ? 0.25f : (over ? 0.12f : 0.0f)));
        g.fillRoundedRectangle(bounds, jmin(radius, 12.0f));
        g.setColour(Colour(0x66f4e4c8));
        g.drawRoundedRectangle(bounds, jmin(radius, 12.0f), 1.2f);
    }

    int getSliderThumbRadius(Slider& slider) override
    {
        return slider.isHorizontal() ? jmax(6, roundToInt(slider.getHeight() * 0.26f))
                                     : LookAndFeel_V4::getSliderThumbRadius(slider);
    }

    void drawLinearSlider(Graphics& g, int x, int y, int w, int h, float position, float, float,
                          Slider::SliderStyle style, Slider& slider) override
    {
        if (style != Slider::LinearHorizontal)
            return LookAndFeel_V4::drawLinearSlider(g, x, y, w, h, position, 0, 0, style, slider);
        const auto bounds = Rectangle<float>(static_cast<float>(x), static_cast<float>(y),
                                             static_cast<float>(w), static_cast<float>(h));
        const auto cy = bounds.getCentreY();
        const auto thickness = jmax(3.0f, h * 0.13f);
        const auto track = Rectangle<float>(bounds.getX(), cy - thickness * 0.5f, bounds.getWidth(), thickness);
        g.setColour(Colour(0xff221c14));
        g.fillRoundedRectangle(track, thickness * 0.5f);
        g.setColour(Colour(0x40f4e4c8));
        g.drawRoundedRectangle(track, thickness * 0.5f, 0.8f);
        const auto filled = track.withRight(jmax(track.getX(), position));
        if (filled.getWidth() > 0.5f)
        {
            ColourGradient fill(accentWeight, track.getX(), cy, Colour(0xfffff1d0), track.getRight(), cy, false);
            auto halo = fill;
            halo.multiplyOpacity(0.28f);
            g.setGradientFill(halo);
            g.fillRoundedRectangle(filled.expanded(0, thickness), thickness);
            g.setGradientFill(fill);
            g.fillRoundedRectangle(filled, thickness * 0.5f);
        }
        const auto radius = static_cast<float>(getSliderThumbRadius(slider));
        const auto thumb = Rectangle<float>(radius * 2, radius * 2).withCentre({position, cy});
        g.setColour(accentWeight.withAlpha(slider.hasKeyboardFocus(true) || slider.isMouseOverOrDragging() ? 0.45f : 0.25f));
        g.fillEllipse(thumb.expanded(radius * 0.45f));
        g.setGradientFill(ColourGradient(Colour(0xffa89a84), thumb.getX(), thumb.getY(),
                                         Colour(0xff17130e), thumb.getRight(), thumb.getBottom(), false));
        g.fillEllipse(thumb);
        g.setColour(Colour(0xffffcf7a));
        g.drawEllipse(thumb.reduced(0.6f), 1.4f);
        g.setColour(Colours::white);
        g.fillEllipse(Rectangle<float>(radius * 0.55f, radius * 0.55f).withCentre(thumb.getCentre()));
    }

    void drawRotarySlider(Graphics& g, int x, int y, int w, int h, float position,
                          float start, float end, Slider& slider) override
    {
        const auto scale = static_cast<float>(jmin(w, h)) / 390.0f;
        Graphics::ScopedSaveState save(g);
        g.addTransform(AffineTransform::scale(scale).translated(x + (w - 390 * scale) * 0.5f,
                                                                 y + (h - 390 * scale) * 0.5f));
        const auto accent = slider.findColour(Slider::rotarySliderFillColourId);
        const Point<float> c{195, 195};
        const auto disc = [c](float r) { return Rectangle<float>(2 * r, 2 * r).withCentre(c); };
        const auto fill = [&](float r, Colour top, Colour bottom) {
            g.setGradientFill(ColourGradient(top, 131, 195 - r, bottom, 253, 195 + r, false));
            g.fillEllipse(disc(r));
        };
        fill(190, Colour(0xff1c1812), Colour(0xff09080a));
        g.setColour(accent.withAlpha(0.26f));
        g.drawEllipse(disc(184), 2);
        fill(173, Colour(0xff6a6152), Colour(0xff0b0a0b));
        fill(162, Colour(0xff050406), Colour(0xff17140f));
        ColourGradient steel(Colour(0xffcdc2ad), 147, 50, Colour(0xff13110e), 259, 335, false);
        steel.addColour(0.17, Colour(0xff57503f));
        steel.addColour(0.45, Colour(0xff17140f));
        steel.addColour(0.72, Colour(0xff3b3428));
        g.setGradientFill(steel);
        g.fillEllipse(disc(147));
        g.setColour(Colour(0xff8c8270));
        g.drawEllipse(disc(147), 1.4f);
        for (int r = 77; r < 145; r += 2)
        {
            g.setColour(Colours::white.withAlpha(0.018f + (r % 7) * 0.003f));
            g.drawEllipse(disc(static_cast<float>(r)), 0.7f);
        }
        g.setColour(Colours::black.withAlpha(0.21f));
        g.fillEllipse(disc(87));
        g.setOpacity(1.0f);
        if (logo.isValid())
            g.drawImageWithin(logo, 95, 95, 200, 200, RectanglePlacement::centred);
        const auto angle = start + position * (end - start);
        if (position > 0.0f)
        {
            Path track;
            track.addCentredArc(c.x, c.y, 176, 176, 0, start, angle, true);
            g.setColour(accent.withAlpha(0.23f));
            g.strokePath(track, PathStrokeType(17, PathStrokeType::curved, PathStrokeType::rounded));
            g.setColour(accent);
            g.strokePath(track, PathStrokeType(5, PathStrokeType::curved, PathStrokeType::rounded));
        }
        const auto inner = c.getPointOnCircumference(130.0f, angle);
        const auto outer = c.getPointOnCircumference(168.0f, angle);
        g.setColour(accent.withAlpha(0.45f));
        g.drawLine({inner, outer}, 15);
        g.setColour(accent.interpolatedWith(Colours::white, 0.78f));
        g.drawLine({inner, outer}, 6);
    }
private:
    Image logo;
};

class Editor final : public AudioProcessorEditor, private Timer
{
public:
    explicit Editor(Processor& p) : AudioProcessorEditor(p), processor(p),
        artwork(ImageFileFormat::loadFrom(BinaryData::panel_png, BinaryData::panel_pngSize))
    {
        const std::array colors{accentWeight, accentSheen, accentLoud};
        const std::array tips{"WEIGHT: low-end tilt and sub harmonics", "SHEEN: top-end air and polish",
                              "LOUD: limiter drive into a -1 dBFS ceiling"};
        for (size_t i = 0; i < knobs.size(); ++i)
        {
            auto& knob = knobs[i];
            knob.setSliderStyle(Slider::RotaryHorizontalVerticalDrag);
            knob.setTextBoxStyle(Slider::NoTextBox, false, 0, 0);
            knob.setRotaryParameters(MathConstants<float>::pi * 1.25f, MathConstants<float>::pi * 2.75f, true);
            knob.setDoubleClickReturnValue(true, 0.0);
            knob.setMouseDragSensitivity(210);
            knob.setColour(Slider::rotarySliderFillColourId, colors[i]);
            knob.setTitle(names[i]);
            knob.setTooltip(String(tips[i]) + "   /   Double-click to reset");
            knob.setLookAndFeel(&look);
            knob.onValueChange = [this] { repaint(); };
            addAndMakeVisible(knob);
            attachments[i] = std::make_unique<AudioProcessorValueTreeState::SliderAttachment>(
                processor.state, ids[i], knob);
        }

        mixSlider.setSliderStyle(Slider::LinearHorizontal);
        mixSlider.setTextBoxStyle(Slider::NoTextBox, false, 0, 0);
        mixSlider.setDoubleClickReturnValue(true, 100.0);
        mixSlider.setWantsKeyboardFocus(true);
        mixSlider.setTitle("MIX");
        mixSlider.setTooltip("MIX (dry / wet)   /   Double-click for 100%");
        mixSlider.setLookAndFeel(&look);
        mixSlider.onValueChange = [this] { repaint(); };
        addAndMakeVisible(mixSlider);
        mixAttachment = std::make_unique<AudioProcessorValueTreeState::SliderAttachment>(
            processor.state, "mix", mixSlider);

        matchButton.setClickingTogglesState(true);
        matchButton.setLookAndFeel(&look);
        matchButton.setTitle("MATCH");
        matchButton.setTooltip("MATCH: turns the result down to your input's loudness, so you judge tone, not volume");
        addAndMakeVisible(matchButton);
        matchAttachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment>(
            processor.state, "match", matchButton);

        const std::array<TextButton*, 5> buttons{&prev, &preset, &next, &save, &menu};
        const std::array titles{"Previous factory preset", "Select a factory preset", "Next factory preset",
                                "Save preset file", "Options and bypass"};
        for (size_t i = 0; i < buttons.size(); ++i)
        {
            auto& button = *buttons[i];
            button.setLookAndFeel(&look);
            button.setTitle(titles[i]);
            button.setTooltip(titles[i]);
            addAndMakeVisible(button);
        }
        prev.onClick = [this] { step(-1); };
        next.onClick = [this] { step(1); };
        preset.onClick = [this] { showPresets(); };
        save.onClick = [this] { presetFile(true); };
        menu.onClick = [this] { showOptions(); };
        setResizable(true, true);
        getConstrainer()->setFixedAspectRatio(1672.0 / 941.0);
        setResizeLimits(760, 428, 1672, 941);
        setSize(1003, 565);
        updateDisplay();
        startTimerHz(30);
    }

    ~Editor() override
    {
        stopTimer();
        for (auto& knob : knobs)
            knob.setLookAndFeel(nullptr);
        mixSlider.setLookAndFeel(nullptr);
        matchButton.setLookAndFeel(nullptr);
        for (auto* button : {&prev, &preset, &next, &save, &menu})
            button->setLookAndFeel(nullptr);
    }

    void resized() override
    {
        for (size_t i = 0; i < knobs.size(); ++i)
            knobs[i].setBounds(region(centres[i] - 195, 263, 390, 390));
        prev.setBounds(region(1271, 98, 42, 45));
        preset.setBounds(region(1313, 98, 159, 45));
        next.setBounds(region(1472, 98, 42, 45));
        save.setBounds(region(1532, 98, 45, 45));
        menu.setBounds(region(1588, 98, 45, 45));
        mixSlider.setBounds(region(1330, 156, 160, 44));
        matchButton.setBounds(region(1548, 162, 78, 32));
    }

    void paint(Graphics& g) override
    {
        g.fillAll(Colour(0xff07060a));
        g.setImageResamplingQuality(Graphics::highResamplingQuality);
        if (artwork.isValid())
            g.drawImage(artwork, getLocalBounds().toFloat());
        g.addTransform(AffineTransform::scale(getWidth() / 1672.0f, getHeight() / 941.0f));

        const Rectangle<float> mixPill(1271, 156, 362, 44);
        g.setColour(Colour(0xd00c0a08));
        g.fillRoundedRectangle(mixPill, 22);
        g.setColour(Colour(0x66f4e4c8));
        g.drawRoundedRectangle(mixPill, 22, 1.2f);
        g.setColour(Colour(0xfffff3e0));
        g.setFont(FontOptions(16.0f).withKerningFactor(0.18f));
        g.drawText("MIX", Rectangle<float>(1288, 156, 44, 44), Justification::centredLeft);
        g.setFont(FontOptions(15.0f).withStyle("Bold"));
        g.drawText(String(roundToInt(mixSlider.getValue())) + "%", Rectangle<float>(1490, 156, 52, 44),
                   Justification::centred);

        meter(g, 53, in);
        meter(g, 1544, out);
        g.setFont(FontOptions(23.0f).withStyle("Bold"));
        for (size_t i = 0; i < knobs.size(); ++i)
        {
            const Rectangle<float> badge(static_cast<float>(centres[i] - 42), 708, 84, 32);
            g.setColour(Colour(0xec0f0c09));
            g.fillRoundedRectangle(badge, 7);
            g.setColour(Colour(0xffcdb894));
            g.drawRoundedRectangle(badge, 7, 1);
            g.setColour(Colour(0xfffff6e8));
            g.drawText(String(roundToInt(knobs[i].getValue())) + "%", badge, Justification::centred);
        }
        if (knobs[2].getValue() > 0.0)
        {
            g.setFont(FontOptions(15.0f).withKerningFactor(0.12f));
            g.setColour(gainReduction > 0.05f ? accentLoud.interpolatedWith(Colours::white, 0.35f) : Colour(0xa0fff3e0));
            g.drawText("GR  -" + String(gainReduction, 1) + " dB",
                       Rectangle<float>(static_cast<float>(centres[2] - 90), 744, 180, 22), Justification::centred);
        }
        if (processor.getBypassParameter()->getValue() >= 0.5f)
        {
            g.setColour(Colour(0xff15110c));
            g.fillRoundedRectangle(761, 228, 150, 34, 6);
            g.setColour(Colour(0xffffd9a0));
            g.setFont(FontOptions(23.0f).withStyle("Bold"));
            g.drawText("BYPASSED", 761, 228, 150, 34, Justification::centred);
        }
    }

private:
    Rectangle<int> region(int x, int y, int w, int h) const
    {
        return Rectangle<float>(x, y, w, h).transformedBy(
            AffineTransform::scale(getWidth() / 1672.0f, getHeight() / 941.0f)).toNearestInt();
    }

    static void meter(Graphics& g, float x, float peak)
    {
        constexpr std::array db{-72, -60, -50, -42, -36, -33, -30, -27, -24, -22, -21, -20,
                                -18, -17, -15, -14, -12, -11, -10, -8, -7, -6, -4, -3, -1, 0};
        g.setColour(Colour(0xff100d0a));
        g.fillRoundedRectangle(x - 3, 280, 34, 355, 4);
        const auto level = Decibels::gainToDecibels(peak, -100.0f);
        for (size_t i = 0; i < db.size(); ++i)
        {
            const auto lit = Colour(0xfffff3dc).interpolatedWith(Colour(0xffffa940), i / 25.0f);
            g.setColour(level >= db[i] ? (i == db.size() - 1 ? Colour(0xffff6b5e) : lit) : Colour(0xff2a2219));
            g.fillRoundedRectangle(x, 624.0f - 13.55f * i, 28, 8.5f, 1.5f);
        }
    }

    void timerCallback() override
    {
        in = jmax(in * 0.87f, processor.inputPeak.exchange(0.0f));
        out = jmax(out * 0.87f, processor.outputPeak.exchange(0.0f));
        gainReduction = jmax(gainReduction * 0.9f, processor.reduction.exchange(0.0f));
        updateDisplay();
        repaint();
    }

    void updateDisplay()
    {
        preset.setButtonText(processor.getProgramName(processor.getCurrentProgram()));
    }

    void step(int delta)
    {
        const auto n = static_cast<int>(presets.size());
        const auto current = processor.getCurrentProgram();
        processor.setCurrentProgram(current == n ? (delta < 0 ? n - 1 : 0) : (current + delta + n) % n);
    }

    void showPresets()
    {
        PopupMenu choices;
        for (size_t i = 0; i < presets.size(); ++i)
            choices.addItem(static_cast<int>(i) + 1, presetNames[i], true,
                            processor.getCurrentProgram() == static_cast<int>(i));
        const Component::SafePointer<Editor> self(this);
        choices.showMenuAsync(PopupMenu::Options().withTargetComponent(&preset), [self](int selected) {
            if (self != nullptr && selected > 0)
                self->processor.setCurrentProgram(selected - 1);
        });
    }

    void showOptions()
    {
        PopupMenu choices;
        auto* parameter = processor.getBypassParameter();
        choices.addItem(1, "Bypass", true, parameter->getValue() >= 0.5f);
        choices.addItem(2, "Open preset file...");
        choices.addItem(3, "Reset to Init");
        const Component::SafePointer<Editor> self(this);
        choices.showMenuAsync(PopupMenu::Options().withTargetComponent(&menu), [self](int selected) {
            if (self == nullptr)
                return;
            if (selected == 1)
            {
                auto* bypassParameter = self->processor.getBypassParameter();
                bypassParameter->beginChangeGesture();
                bypassParameter->setValueNotifyingHost(bypassParameter->getValue() < 0.5f ? 1.0f : 0.0f);
                bypassParameter->endChangeGesture();
            }
            if (selected == 2)
                self->presetFile(false);
            if (selected == 3)
                self->processor.setCurrentProgram(0);
        });
    }

    void presetFile(bool writing)
    {
        if (chooser != nullptr)
            return;
        chooser = std::make_unique<FileChooser>(writing ? "Save REVELATION preset" : "Open REVELATION preset",
            File::getSpecialLocation(File::userDocumentsDirectory).getChildFile(writing ? "Revelation.t7apreset" : ""),
            "*.t7apreset");
        const auto flags = FileBrowserComponent::canSelectFiles
            | (writing ? FileBrowserComponent::saveMode | FileBrowserComponent::warnAboutOverwriting
                       : FileBrowserComponent::openMode);
        const Component::SafePointer<Editor> self(this);
        chooser->launchAsync(flags, [self, writing](const FileChooser& dialog) {
            if (self == nullptr)
                return;
            const auto result = dialog.getResult();
            if (result != File{})
            {
                MemoryBlock bytes;
                bool success;
                if (writing)
                {
                    self->processor.getStateInformation(bytes);
                    success = result.withFileExtension("t7apreset").replaceWithData(bytes.getData(), bytes.getSize());
                }
                else
                    success = result.getSize() <= 65536 && result.loadFileAsData(bytes)
                        && self->processor.restore(bytes.getData(), static_cast<int>(bytes.getSize()));
                if (!success)
                    AlertWindow::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "REVELATION",
                        writing ? "The preset could not be saved." : "This file could not be loaded as a REVELATION preset.");
            }
            self->chooser.reset();
        });
    }

    Processor& processor;
    const Image artwork;
    KnobLook look;
    TooltipWindow tooltips{this, 700};
    static constexpr std::array centres{397, 840, 1282};
    std::array<Slider, 3> knobs;
    std::array<std::unique_ptr<AudioProcessorValueTreeState::SliderAttachment>, 3> attachments;
    Slider mixSlider;
    std::unique_ptr<AudioProcessorValueTreeState::SliderAttachment> mixAttachment;
    TextButton matchButton{"MATCH"};
    std::unique_ptr<AudioProcessorValueTreeState::ButtonAttachment> matchAttachment;
    TextButton prev{"<"}, preset{"INIT PRESET"}, next{">"}, save{"SAVE"}, menu{"..."};
    std::unique_ptr<FileChooser> chooser;
    float in{}, out{}, gainReduction{};
};

AudioProcessorEditor* Processor::createEditor() { return new Editor(*this); }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new Processor(); }
