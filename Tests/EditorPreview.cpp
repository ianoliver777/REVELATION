#include <juce_audio_utils/juce_audio_utils.h>

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

int main(int argc, char** argv)
{
    if (argc != 2)
        return 1;
    const juce::ScopedJuceInitialiser_GUI gui;
    std::unique_ptr<juce::AudioProcessor> processor(createPluginFilter());
    for (int i = 1; i <= 4; ++i)
    {
        processor->setCurrentProgram(i);
        if (processor->getCurrentProgram() != i)
            return 5;
    }
    processor->setCurrentProgram(1);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor->createEditorIfNeeded());
    if (editor == nullptr)
        return 2;
    processor->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> block(2, 512);
    juce::MidiBuffer midi;
    // Eight seconds of a pulsing test signal so the whole waveform panel fills.
    for (int blockIndex = 0; blockIndex < 760; ++blockIndex)
    {
        for (int i = 0; i < block.getNumSamples(); ++i)
        {
            const auto t = static_cast<float>(blockIndex * 512 + i) / 48000.0f;
            const auto phase = juce::MathConstants<float>::twoPi * t;
            const auto beat = std::exp(-6.0f * std::fmod(t, 0.5f)) * (0.55f + 0.45f * std::sin(0.7f * phase));
            block.setSample(0, i, beat * (0.42f * std::sin(117.0f * phase) + 0.12f * std::sin(1311.0f * phase)));
            block.setSample(1, i, beat * (0.38f * std::sin(117.0f * phase + 0.9f) + 0.15f * std::sin(1293.0f * phase)));
        }
        processor->processBlock(block, midi);
    }
    auto image = editor->createComponentSnapshot(editor->getLocalBounds(), true, 1.5f);
    processor->releaseResources();
    processor->setCurrentProgram(0);
    if (processor->getCurrentProgram() != 0)
        return 6;
    const juce::File file(argv[1]);
    if (!file.deleteFile())
        return 3;
    juce::FileOutputStream output(file);
    return output.openedOk() && juce::PNGImageFormat().writeImageToStream(image, output) ? 0 : 4;
}
