#include <juce_audio_utils/juce_audio_utils.h>

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

int main(int argc, char** argv)
{
    if (argc != 2)
        return 1;
    const juce::ScopedJuceInitialiser_GUI gui;
    std::unique_ptr<juce::AudioProcessor> processor(createPluginFilter());
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor->createEditorIfNeeded());
    if (editor == nullptr)
        return 2;
    auto image = editor->createComponentSnapshot(editor->getLocalBounds(), true, 1.5f);
    for (int i = 1; i <= 4; ++i)
    {
        processor->setCurrentProgram(i);
        if (processor->getCurrentProgram() != i)
            return 5;
    }
    processor->setCurrentProgram(0);
    if (processor->getCurrentProgram() != 0)
        return 6;
    const juce::File file(argv[1]);
    if (!file.deleteFile())
        return 3;
    juce::FileOutputStream output(file);
    return output.openedOk() && juce::PNGImageFormat().writeImageToStream(image, output) ? 0 : 4;
}
