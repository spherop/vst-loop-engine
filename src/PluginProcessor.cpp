#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>

FuzzDelayProcessor::FuzzDelayProcessor()
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

FuzzDelayProcessor::~FuzzDelayProcessor()
{
}

juce::AudioProcessorValueTreeState::ParameterLayout FuzzDelayProcessor::createParameterLayout()
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

    return { params.begin(), params.end() };
}

const juce::String FuzzDelayProcessor::getName() const
{
    return JucePlugin_Name;
}

bool FuzzDelayProcessor::acceptsMidi() const
{
    return false;
}

bool FuzzDelayProcessor::producesMidi() const
{
    return false;
}

bool FuzzDelayProcessor::isMidiEffect() const
{
    return false;
}

double FuzzDelayProcessor::getTailLengthSeconds() const
{
    return 2.0;
}

int FuzzDelayProcessor::getNumPrograms()
{
    return 1;
}

int FuzzDelayProcessor::getCurrentProgram()
{
    return 0;
}

void FuzzDelayProcessor::setCurrentProgram(int index)
{
    juce::ignoreUnused(index);
}

const juce::String FuzzDelayProcessor::getProgramName(int index)
{
    juce::ignoreUnused(index);
    return {};
}

void FuzzDelayProcessor::changeProgramName(int index, const juce::String& newName)
{
    juce::ignoreUnused(index, newName);
}

void FuzzDelayProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    // Prepare delay lines
    delayLineL.prepare(sampleRate, 2000); // Max 2 second delay
    delayLineR.prepare(sampleRate, 2000);

    // Prepare test tone generator
    testToneGenerator.prepare(sampleRate, samplesPerBlock);
}

void FuzzDelayProcessor::releaseResources()
{
    delayLineL.clear();
    delayLineR.clear();
}

bool FuzzDelayProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
        && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;

    return true;
}

void FuzzDelayProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused(midiMessages);

    juce::ScopedNoDenormals noDenormals;
    const auto totalNumInputChannels = getTotalNumInputChannels();
    const auto totalNumOutputChannels = getTotalNumOutputChannels();
    const auto numSamples = buffer.getNumSamples();

    // Track host tempo
    if (auto* playHead = getPlayHead())
    {
        if (auto posInfo = playHead->getPosition())
        {
            if (posInfo->getBpm())
                lastHostBpm.store(static_cast<float>(*posInfo->getBpm()));
        }
    }

    // Clear any output channels that don't have input data
    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear(i, 0, numSamples);

    // Add test tone to input if playing
    testToneGenerator.processBlock(buffer);

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

    // Process audio
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

void FuzzDelayProcessor::triggerTestSound(int soundType)
{
    switch (soundType)
    {
        case 0:
            testToneGenerator.trigger(TestToneGenerator::SoundType::Click);
            break;
        case 1:
            testToneGenerator.trigger(TestToneGenerator::SoundType::DrumLoop);
            break;
        case 2:
            testToneGenerator.trigger(TestToneGenerator::SoundType::SynthChord);
            break;
        case 3:
            testToneGenerator.trigger(TestToneGenerator::SoundType::GuitarChord);
            break;
        default:
            break;
    }
}

void FuzzDelayProcessor::stopTestSound()
{
    testToneGenerator.stop();
}

void FuzzDelayProcessor::setLoopEnabled(bool enabled)
{
    testToneGenerator.setLoopEnabled(enabled);
}

bool FuzzDelayProcessor::getLoopEnabled() const
{
    return testToneGenerator.getLoopEnabled();
}

void FuzzDelayProcessor::setTempoSync(bool enabled)
{
    tempoSyncEnabled.store(enabled);
}

bool FuzzDelayProcessor::getTempoSyncEnabled() const
{
    return tempoSyncEnabled.load();
}

void FuzzDelayProcessor::setTempoNote(int noteIndex)
{
    tempoNoteValue.store(std::clamp(noteIndex, 0, 5));
}

int FuzzDelayProcessor::getTempoNoteValue() const
{
    return tempoNoteValue.load();
}

float FuzzDelayProcessor::getHostBpm() const
{
    return lastHostBpm.load();
}

float FuzzDelayProcessor::calculateSyncedDelayTime() const
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

bool FuzzDelayProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* FuzzDelayProcessor::createEditor()
{
    return new FuzzDelayEditor(*this);
}

void FuzzDelayProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

void FuzzDelayProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));

    if (xmlState != nullptr && xmlState->hasTagName(apvts.state.getType()))
        apvts.replaceState(juce::ValueTree::fromXml(*xmlState));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new FuzzDelayProcessor();
}
