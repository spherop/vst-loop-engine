#include "PluginProcessor.h"
#include "PluginEditor.h"

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

    // Clear any output channels that don't have input data
    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear(i, 0, numSamples);

    // Add test tone to input if playing
    testToneGenerator.processBlock(buffer);

    // Get current parameter values
    const float delayTime = delayTimeParam->load();
    const float feedback = feedbackParam->load();
    const float mix = mixParam->load() / 100.0f; // Convert to 0-1
    const float tone = toneParam->load();

    // Update delay line parameters
    delayLineL.setDelayTime(delayTime);
    delayLineL.setFeedback(feedback);
    delayLineL.setTone(tone);

    delayLineR.setDelayTime(delayTime);
    delayLineR.setFeedback(feedback);
    delayLineR.setTone(tone);

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
