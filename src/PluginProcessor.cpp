#include "PluginProcessor.h"
#include "PluginEditor.h"

FuzzDelayProcessor::FuzzDelayProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "Parameters", createParameterLayout())
{
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
    juce::ignoreUnused(sampleRate, samplesPerBlock);
    // Will be implemented in Phase 2 for delay processing
}

void FuzzDelayProcessor::releaseResources()
{
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
    auto totalNumInputChannels = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    // Clear any output channels that don't have input data
    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear(i, 0, buffer.getNumSamples());

    // Phase 1: Pass audio through unchanged
    // DSP processing will be added in Phase 2
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
