#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>

LoopEngineProcessor::LoopEngineProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "Parameters", createParameterLayout())
{
    // Cache parameter pointers for efficient access in processBlock
    delayTimeParam = apvts.getRawParameterValue("delayTime");
    feedbackParam = apvts.getRawParameterValue("feedback");
    mixParam = apvts.getRawParameterValue("mix");
    toneParam = apvts.getRawParameterValue("tone");

    // BBD character parameters
    ageParam = apvts.getRawParameterValue("age");
    modRateParam = apvts.getRawParameterValue("modRate");
    modDepthParam = apvts.getRawParameterValue("modDepth");
    warmthParam = apvts.getRawParameterValue("warmth");
}

LoopEngineProcessor::~LoopEngineProcessor()
{
}

juce::AudioProcessorValueTreeState::ParameterLayout LoopEngineProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // Delay parameters
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"delayTime", 1},
        "Delay Time",
        juce::NormalisableRange<float>(1.0f, 2000.0f, 0.1f, 0.5f),
        300.0f,
        juce::AudioParameterFloatAttributes().withLabel("ms")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"feedback", 1},
        "Feedback",
        juce::NormalisableRange<float>(0.0f, 95.0f, 0.1f),
        40.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"mix", 1},
        "Mix",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        50.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"tone", 1},
        "Tone",
        juce::NormalisableRange<float>(200.0f, 12000.0f, 1.0f, 0.3f),
        4000.0f,
        juce::AudioParameterFloatAttributes().withLabel("Hz")));

    // BBD Character parameters
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"age", 1},
        "Age",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        25.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"modRate", 1},
        "Mod Rate",
        juce::NormalisableRange<float>(0.1f, 5.0f, 0.01f, 0.5f),
        0.5f,
        juce::AudioParameterFloatAttributes().withLabel("Hz")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"modDepth", 1},
        "Mod Depth",
        juce::NormalisableRange<float>(0.0f, 20.0f, 0.1f),
        3.0f,
        juce::AudioParameterFloatAttributes().withLabel("ms")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"warmth", 1},
        "Warmth",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        30.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    // Loop parameters
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"loopStart", 1},
        "Loop Start",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f),
        0.0f));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"loopEnd", 1},
        "Loop End",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f),
        1.0f));

    // Loop Speed: 0.25x to 4.0x, with 1.0x at center (normalized 0.5)
    // For skew: we need 1.0 = 0.25 + (4.0-0.25) * pow(0.5, 1/skew)
    // Solving: (1.0-0.25)/(4.0-0.25) = pow(0.5, 1/skew)
    // 0.2 = pow(0.5, 1/skew) => log(0.2)/log(0.5) = 1/skew => skew = log(0.5)/log(0.2) ≈ 0.431
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"loopSpeed", 1},
        "Loop Speed",
        juce::NormalisableRange<float>(0.25f, 4.0f, 0.01f, 0.431f),
        1.0f,
        juce::AudioParameterFloatAttributes().withLabel("x")));

    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"loopReverse", 1},
        "Loop Reverse",
        false));

    return { params.begin(), params.end() };
}

const juce::String LoopEngineProcessor::getName() const
{
    return JucePlugin_Name;
}

bool LoopEngineProcessor::acceptsMidi() const
{
    return false;
}

bool LoopEngineProcessor::producesMidi() const
{
    return false;
}

bool LoopEngineProcessor::isMidiEffect() const
{
    return false;
}

double LoopEngineProcessor::getTailLengthSeconds() const
{
    return 2.0;
}

int LoopEngineProcessor::getNumPrograms()
{
    return 1;
}

int LoopEngineProcessor::getCurrentProgram()
{
    return 0;
}

void LoopEngineProcessor::setCurrentProgram(int index)
{
    juce::ignoreUnused(index);
}

const juce::String LoopEngineProcessor::getProgramName(int index)
{
    juce::ignoreUnused(index);
    return {};
}

void LoopEngineProcessor::changeProgramName(int index, const juce::String& newName)
{
    juce::ignoreUnused(index, newName);
}

void LoopEngineProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    // Prepare delay lines
    delayLineL.prepare(sampleRate, 2000); // Max 2 second delay
    delayLineR.prepare(sampleRate, 2000);

    // Prepare loop engine
    loopEngine.prepare(sampleRate, samplesPerBlock);

    // Prepare sample loader (primary) and synth generator (fallback)
    testSoundLoader.prepare(sampleRate, samplesPerBlock);
    testToneGenerator.prepare(sampleRate, samplesPerBlock);
}

void LoopEngineProcessor::releaseResources()
{
    delayLineL.clear();
    delayLineR.clear();
}

bool LoopEngineProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
        && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;

    return true;
}

void LoopEngineProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused(midiMessages);

    juce::ScopedNoDenormals noDenormals;
    const auto totalNumInputChannels = getTotalNumInputChannels();
    const auto totalNumOutputChannels = getTotalNumOutputChannels();
    const auto numSamples = buffer.getNumSamples();

    // Track host tempo and transport state
    if (auto* playHead = getPlayHead())
    {
        if (auto posInfo = playHead->getPosition())
        {
            if (posInfo->getBpm())
            {
                float bpm = static_cast<float>(*posInfo->getBpm());
                lastHostBpm.store(bpm);
                loopEngine.setHostBpm(bpm);
            }

            // Track host playing state for transport sync
            bool hostPlaying = posInfo->getIsPlaying();
            bool wasPlaying = lastHostPlaying.exchange(hostPlaying);

            // If host transport sync is enabled, control looper based on host state
            if (hostTransportSyncEnabled.load())
            {
                if (hostPlaying && !wasPlaying)
                {
                    // Host started playing - start looper playback if we have content
                    if (loopEngine.hasContent() && loopEngine.getState() == LoopBuffer::State::Idle)
                    {
                        loopEngine.play();
                    }
                }
                else if (!hostPlaying && wasPlaying)
                {
                    // Host stopped - stop looper
                    if (loopEngine.getState() == LoopBuffer::State::Playing ||
                        loopEngine.getState() == LoopBuffer::State::Overdubbing)
                    {
                        loopEngine.stop();
                    }
                }
            }
        }
    }

    // Clear any output channels that don't have input data
    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear(i, 0, numSamples);

    // Add test sound to input if playing
    // Use sample loader if samples are available, otherwise fall back to synth
    if (testSoundLoader.getNumSamples() > 0)
        testSoundLoader.processBlock(buffer);
    else
        testToneGenerator.processBlock(buffer);

    // Update loop engine parameters
    if (auto* loopStartParam = apvts.getRawParameterValue("loopStart"))
        loopEngine.setLoopStart(loopStartParam->load());
    if (auto* loopEndParam = apvts.getRawParameterValue("loopEnd"))
        loopEngine.setLoopEnd(loopEndParam->load());
    if (auto* loopSpeedParam = apvts.getRawParameterValue("loopSpeed"))
        loopEngine.setSpeed(loopSpeedParam->load());
    if (auto* loopReverseParam = apvts.getRawParameterValue("loopReverse"))
        loopEngine.setReverse(loopReverseParam->load() > 0.5f);

    // Process through loop engine
    loopEngine.processBlock(buffer);

    // Get delay time - either from parameter or tempo sync
    const float delayTime = tempoSyncEnabled.load()
        ? calculateSyncedDelayTime()
        : delayTimeParam->load();
    const float feedback = feedbackParam->load();
    const float mix = mixParam->load() / 100.0f; // Convert to 0-1
    const float tone = toneParam->load();

    // Get BBD character parameters
    const float age = ageParam->load();
    const float modRate = modRateParam->load();
    const float modDepth = modDepthParam->load();
    const float warmth = warmthParam->load();

    // Update delay line parameters
    delayLineL.setDelayTime(delayTime);
    delayLineL.setFeedback(feedback);
    delayLineL.setTone(tone);
    delayLineL.setAge(age);
    delayLineL.setModRate(modRate);
    delayLineL.setModDepth(modDepth);
    delayLineL.setWarmth(warmth);

    delayLineR.setDelayTime(delayTime);
    delayLineR.setFeedback(feedback);
    delayLineR.setTone(tone);
    delayLineR.setAge(age);
    delayLineR.setModRate(modRate);
    delayLineR.setModDepth(modDepth);
    delayLineR.setWarmth(warmth);

    // Process audio through delay (if enabled)
    if (delayEnabled.load())
    {
        if (totalNumInputChannels >= 1)
        {
            auto* channelL = buffer.getWritePointer(0);

            for (int sample = 0; sample < numSamples; ++sample)
            {
                const float dry = channelL[sample];
                const float wet = delayLineL.processSample(dry);
                channelL[sample] = dry * (1.0f - mix) + wet * mix;
            }
        }

        if (totalNumInputChannels >= 2)
        {
            auto* channelR = buffer.getWritePointer(1);

            for (int sample = 0; sample < numSamples; ++sample)
            {
                const float dry = channelR[sample];
                const float wet = delayLineR.processSample(dry);
                channelR[sample] = dry * (1.0f - mix) + wet * mix;
            }
        }
    }
}

void LoopEngineProcessor::triggerTestSound(int soundIndex)
{
    // If we have samples loaded, use the sample loader
    if (testSoundLoader.getNumSamples() > 0)
    {
        testSoundLoader.trigger(soundIndex);
    }
    else
    {
        // Fall back to synthesized sounds
        switch (soundIndex)
        {
            case 0:  testToneGenerator.trigger(TestToneGenerator::SoundType::Click); break;
            case 1:  testToneGenerator.trigger(TestToneGenerator::SoundType::DrumLoop); break;
            case 2:  testToneGenerator.trigger(TestToneGenerator::SoundType::SynthPad); break;
            case 3:  testToneGenerator.trigger(TestToneGenerator::SoundType::ElectricGuitar); break;
            case 4:  testToneGenerator.trigger(TestToneGenerator::SoundType::BassGroove); break;
            case 5:  testToneGenerator.trigger(TestToneGenerator::SoundType::PianoChord); break;
            case 6:  testToneGenerator.trigger(TestToneGenerator::SoundType::VocalPhrase); break;
            case 7:  testToneGenerator.trigger(TestToneGenerator::SoundType::Percussion); break;
            case 8:  testToneGenerator.trigger(TestToneGenerator::SoundType::AmbientTexture); break;
            case 9:  testToneGenerator.trigger(TestToneGenerator::SoundType::NoiseBurst); break;
            default: break;
        }
    }
}

void LoopEngineProcessor::stopTestSound()
{
    testSoundLoader.stop();
    testToneGenerator.stop();
}

void LoopEngineProcessor::setLoopEnabled(bool enabled)
{
    testSoundLoader.setLoopEnabled(enabled);
    testToneGenerator.setLoopEnabled(enabled);
}

bool LoopEngineProcessor::getLoopEnabled() const
{
    return testSoundLoader.getLoopEnabled();
}

int LoopEngineProcessor::getNumTestSounds() const
{
    if (testSoundLoader.getNumSamples() > 0)
        return testSoundLoader.getNumSamples();
    return 10; // Fallback synth sounds
}

juce::String LoopEngineProcessor::getTestSoundName(int index) const
{
    if (testSoundLoader.getNumSamples() > 0)
        return testSoundLoader.getSampleName(index);

    // Fallback names for synthesized sounds
    static const char* synthNames[] = {
        "Click", "Drum Loop", "Synth Pad", "Electric Guitar", "Bass Groove",
        "Piano Chord", "Vocal Phrase", "Percussion", "Ambient Texture", "Noise Burst"
    };
    if (index >= 0 && index < 10)
        return synthNames[index];
    return "---";
}

juce::StringArray LoopEngineProcessor::getAllTestSoundNames() const
{
    if (testSoundLoader.getNumSamples() > 0)
        return testSoundLoader.getAllSampleNames();

    // Fallback names for synthesized sounds
    return juce::StringArray{
        "Click", "Drum Loop", "Synth Pad", "Electric Guitar", "Bass Groove",
        "Piano Chord", "Vocal Phrase", "Percussion", "Ambient Texture", "Noise Burst"
    };
}

juce::String LoopEngineProcessor::getSampleFolderPath() const
{
    return testSoundLoader.getSampleFolderPath();
}

void LoopEngineProcessor::reloadSamples()
{
    testSoundLoader.reloadSamples();
}

bool LoopEngineProcessor::usingSamplesFromDisk() const
{
    return testSoundLoader.getNumSamples() > 0;
}

void LoopEngineProcessor::setSampleFolder(const juce::String& path)
{
    juce::File folder(path);
    if (folder.exists() && folder.isDirectory())
        testSoundLoader.setSampleFolder(folder);
}

void LoopEngineProcessor::setTempoSync(bool enabled)
{
    tempoSyncEnabled.store(enabled);
}

bool LoopEngineProcessor::getTempoSyncEnabled() const
{
    return tempoSyncEnabled.load();
}

void LoopEngineProcessor::setTempoNote(int noteIndex)
{
    tempoNoteValue.store(std::clamp(noteIndex, 0, 5));
}

int LoopEngineProcessor::getTempoNoteValue() const
{
    return tempoNoteValue.load();
}

float LoopEngineProcessor::getHostBpm() const
{
    return lastHostBpm.load();
}

float LoopEngineProcessor::calculateSyncedDelayTime() const
{
    // Note value multipliers relative to quarter note
    // 0=1/4, 1=1/8, 2=1/8T, 3=1/16, 4=1/16T, 5=1/32
    static constexpr float noteMultipliers[] = {
        1.0f,      // 1/4
        0.5f,      // 1/8
        0.333333f, // 1/8T (triplet)
        0.25f,     // 1/16
        0.166667f, // 1/16T (triplet)
        0.125f     // 1/32
    };

    const float bpm = lastHostBpm.load();
    if (bpm <= 0.0f)
        return 300.0f; // Default fallback

    const int noteIdx = std::clamp(tempoNoteValue.load(), 0, 5);
    const float multiplier = noteMultipliers[noteIdx];

    // Quarter note duration in ms = 60000 / BPM
    const float quarterNoteMs = 60000.0f / bpm;

    return std::clamp(quarterNoteMs * multiplier, 1.0f, 2000.0f);
}

void LoopEngineProcessor::setDelayEnabled(bool enabled)
{
    delayEnabled.store(enabled);
}

bool LoopEngineProcessor::getDelayEnabled() const
{
    return delayEnabled.load();
}

void LoopEngineProcessor::setHostTransportSync(bool enabled)
{
    hostTransportSyncEnabled.store(enabled);
}

bool LoopEngineProcessor::getHostTransportSync() const
{
    return hostTransportSyncEnabled.load();
}

bool LoopEngineProcessor::isHostPlaying() const
{
    return lastHostPlaying.load();
}

bool LoopEngineProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* LoopEngineProcessor::createEditor()
{
    return new LoopEngineEditor(*this);
}

void LoopEngineProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

void LoopEngineProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));

    if (xmlState != nullptr && xmlState->hasTagName(apvts.state.getType()))
        apvts.replaceState(juce::ValueTree::fromXml(*xmlState));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new LoopEngineProcessor();
}
