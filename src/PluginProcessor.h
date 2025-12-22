#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "DelayLine.h"
#include "TestToneGenerator.h"

class FuzzDelayProcessor : public juce::AudioProcessor
{
public:
    FuzzDelayProcessor();
    ~FuzzDelayProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getAPVTS() { return apvts; }

    // Test tone control
    void triggerTestSound(int soundType);
    void stopTestSound();

private:
    juce::AudioProcessorValueTreeState apvts;
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // DSP
    DelayLine delayLineL;
    DelayLine delayLineR;

    // Test sounds
    TestToneGenerator testToneGenerator;

    // Parameter pointers for efficient access
    std::atomic<float>* delayTimeParam = nullptr;
    std::atomic<float>* feedbackParam = nullptr;
    std::atomic<float>* mixParam = nullptr;
    std::atomic<float>* toneParam = nullptr;

    // BBD character parameters
    std::atomic<float>* ageParam = nullptr;
    std::atomic<float>* modRateParam = nullptr;
    std::atomic<float>* modDepthParam = nullptr;
    std::atomic<float>* warmthParam = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FuzzDelayProcessor)
};
