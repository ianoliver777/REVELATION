#include <juce_audio_utils/juce_audio_utils.h>
#include <iostream>
#if JUCE_MAC
 #include <AudioToolbox/AudioToolbox.h>
#endif

using namespace juce;

void require(bool condition, const String& reason)
{
    if (!condition)
    {
        std::cerr << reason << '\n';
        std::exit(1);
    }
}

int main(int argc, char** argv)
{
    require(argc >= 2, "Usage: HostTest /absolute/path/to/REVELATION.vst3-or-component [preview.png]");
    const ScopedJuceInitialiser_GUI gui;
    const File bundle(argv[1]);
    auto identifier = bundle.getFullPathName();
    AudioPluginFormatManager manager;
#if JUCE_MAC
    CFBundleRef auBundle = nullptr;
    if (bundle.hasFileExtension("component"))
    {
        const auto path = identifier.toCFString();
        const auto url = CFURLCreateWithFileSystemPath(nullptr, path, kCFURLPOSIXPathStyle, true);
        CFRelease(path);
        auBundle = CFBundleCreate(nullptr, url);
        CFRelease(url);
        require(auBundle != nullptr && CFBundleLoadExecutable(auBundle), "Could not load AU bundle");
        const auto factory = reinterpret_cast<AudioComponentFactoryFunction>(
            CFBundleGetFunctionPointerForName(auBundle, CFSTR("REVELATIONAUFactory")));
        require(factory != nullptr, "Missing AU factory function");
        AudioComponentDescription desc{kAudioUnitType_Effect, 'Rv03', 'T7Au', kAudioComponentFlag_SandboxSafe, 0};
        require(AudioComponentRegister(&desc, CFSTR("TRIPLE SEVEN AUDIO: REVELATION"), 65536, factory)
                != nullptr, "Could not register AU in the test process");
        identifier = "AudioUnit:Effects/aufx,Rv03,T7Au";
        manager.addFormat(std::make_unique<AudioUnitPluginFormat>());
    }
    else
#endif
        manager.addFormat(std::make_unique<VST3PluginFormat>());
    OwnedArray<PluginDescription> descriptions;
    manager.getFormat(0)->findAllTypesForFile(descriptions, identifier);
    require(descriptions.size() == 1, "Expected one effect in " + identifier);
    String error;
    auto effect = manager.createPluginInstance(*descriptions[0], 48000.0, 512, error);
    require(effect != nullptr, "Could not instantiate effect: " + error);
    require(effect->getTotalNumInputChannels() == 2 && effect->getTotalNumOutputChannels() == 2,
            "Expected stereo I/O");
    effect->prepareToPlay(48000.0, 512);

    std::array<AudioProcessorParameter*, 3> knobs{};
    AudioProcessorParameter* mix = nullptr;
    for (auto* parameter : effect->getParameters())
    {
        const auto name = parameter->getName(64);
        if (name == "WEIGHT") knobs[0] = parameter;
        if (name == "SHEEN") knobs[1] = parameter;
        if (name == "LOUD") knobs[2] = parameter;
        if (name == "MIX") mix = parameter;
    }
    require(mix != nullptr && mix->getValue() == 1.0f, "Expected MIX to default to 100%");
    for (auto* parameter : knobs)
        require(parameter != nullptr && parameter->getValue() == 0.0f, "Expected three zeroed macros");

    juce::AudioBuffer<float> dry(2, 512), audio(2, 512);
    MidiBuffer midi;
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < 512; ++i)
            dry.setSample(c, i, 0.4f * std::sin(0.14f * i + 0.1f * c));
    const int latency = effect->getLatencySamples();
    require(latency > 0 && latency < 512, "Expected a short reported lookahead latency");
    audio.makeCopyOf(dry);
    effect->processBlock(audio, midi);
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < 512; ++i)
            require(audio.getSample(c, i) == (i >= latency ? dry.getSample(c, i - latency) : 0.0f),
                    "Init audio must be the input delayed by the reported latency");

    if (argc >= 3)
    {
        std::unique_ptr<AudioProcessorEditor> editor(effect->createEditorIfNeeded());
        require(editor != nullptr, "Editor could not be created");
        const auto preview = editor->createComponentSnapshot(editor->getLocalBounds(), true, 1.5f);
        const File file(argv[2]);
        require(file.deleteFile(), "Could not clear preview file");
        FileOutputStream stream(file);
        require(stream.openedOk() && PNGImageFormat().writeImageToStream(preview, stream), "Preview write failed");
    }

    for (auto* parameter : knobs)
        parameter->setValueNotifyingHost(0.65f);
    for (int block = 0; block < 30; ++block)
    {
        audio.makeCopyOf(dry);
        effect->processBlock(audio, midi);
    }
    double difference = 0.0;
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < 512; ++i)
        {
            require(std::isfinite(audio.getSample(c, i)), "Nonfinite processed audio");
            difference += std::abs(audio.getSample(c, i) - dry.getSample(c, i));
        }
    require(difference > 1.0, "Loaded binary did not affect audio");

    MemoryBlock state;
    effect->getStateInformation(state);
    require(state.getSize() > 0, "State blob is empty");
    for (auto* parameter : knobs)
        parameter->setValueNotifyingHost(0.0f);
    mix->setValueNotifyingHost(0.25f);
    effect->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    for (auto* parameter : knobs)
        require(std::abs(parameter->getValue() - 0.65f) < 0.002f, "Macro failed to recall");
    require(mix->getValue() == 1.0f, "MIX failed to recall");
    effect->releaseResources();
    effect.reset();
#if JUCE_MAC
    if (auBundle != nullptr)
        CFRelease(auBundle);
#endif
    std::cout << descriptions[0]->pluginFormatName
              << " loaded and passed stereo rendering, defaults, automation and project state recall\n";
}
