#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "DelayLine.h"
#include "LoopEngine.h"
#include "DegradeProcessor.h"
#include "TestToneGenerator.h"
#include "TestSoundLoader.h"

class LoopEngineProcessor : public juce::AudioProcessor
{
public:
    LoopEngineProcessor();
    ~LoopEngineProcessor() override;

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

    // Test sound control
    void triggerTestSound(int soundIndex);
    void stopTestSound();
    void setLoopEnabled(bool enabled);
    bool getLoopEnabled() const;

    // Sample loader access
    int getNumTestSounds() const;
    juce::String getTestSoundName(int index) const;
    juce::StringArray getAllTestSoundNames() const;
    juce::String getSampleFolderPath() const;
    void reloadSamples();
    bool usingSamplesFromDisk() const;
    void setSampleFolder(const juce::String& path);
    TestSoundLoader& getTestSoundLoader() { return testSoundLoader; }

    // Tempo sync control
    void setTempoSync(bool enabled);
    bool getTempoSyncEnabled() const;
    void setTempoNote(int noteIndex);
    int getTempoNoteValue() const;
    float getHostBpm() const;
    float calculateSyncedDelayTime() const;

    // Delay bypass
    void setDelayEnabled(bool enabled);
    bool getDelayEnabled() const;

    // Degrade master bypass
    void setDegradeEnabled(bool enabled);
    bool getDegradeEnabled() const;

    // Degrade processor access
    DegradeProcessor& getDegradeProcessor() { return degradeProcessor; }
    void setDegradeScrambleSubdiv(int subdiv);

    // Degrade section bypass
    void setDegradeFilterEnabled(bool enabled);
    void setDegradeLofiEnabled(bool enabled);
    void setDegradeScramblerEnabled(bool enabled);
    bool getDegradeFilterEnabled() const;
    bool getDegradeLofiEnabled() const;
    bool getDegradeScramblerEnabled() const;

    // Host transport sync
    void setHostTransportSync(bool enabled);
    bool getHostTransportSync() const;
    bool isHostPlaying() const;

    // Loop engine access
    LoopEngine& getLoopEngine() { return loopEngine; }
    const LoopEngine& getLoopEngine() const { return loopEngine; }

private:
    juce::AudioProcessorValueTreeState apvts;
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // DSP
    DelayLine delayLineL;
    DelayLine delayLineR;
    LoopEngine loopEngine;
    DegradeProcessor degradeProcessor;

    // Test sounds - sample loader (primary) and synth generator (fallback)
    TestSoundLoader testSoundLoader;
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

    // Degrade parameters
    std::atomic<float>* degradeHPParam = nullptr;
    std::atomic<float>* degradeHPQParam = nullptr;
    std::atomic<float>* degradeLPParam = nullptr;
    std::atomic<float>* degradeLPQParam = nullptr;
    std::atomic<float>* degradeBitParam = nullptr;
    std::atomic<float>* degradeSRParam = nullptr;
    std::atomic<float>* degradeWobbleParam = nullptr;
    std::atomic<float>* degradeScrambleAmtParam = nullptr;
    std::atomic<float>* degradeMixParam = nullptr;

    // Tempo sync state
    std::atomic<bool> tempoSyncEnabled { false };
    std::atomic<int> tempoNoteValue { 1 };  // 0=1/4, 1=1/8, 2=1/8T, 3=1/16, 4=1/16T, 5=1/32
    std::atomic<float> lastHostBpm { 120.0f };

    // Delay bypass
    std::atomic<bool> delayEnabled { true };

    // Host transport sync
    std::atomic<bool> hostTransportSyncEnabled { false };
    std::atomic<bool> lastHostPlaying { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LoopEngineProcessor)
};
