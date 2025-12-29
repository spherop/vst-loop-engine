#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <array>
#include <cmath>

/**
 * ReverbProcessor - High-quality reverb using JUCE's built-in Reverb
 *
 * Uses JUCE's Freeverb-based reverb engine which provides smooth,
 * natural-sounding reverberation without the metallic artifacts of
 * short comb filter implementations.
 *
 * Algorithm modes adjust the reverb parameters to simulate:
 * - SPRING: Shorter decay, brighter, more intimate
 * - PLATE: Medium decay, smooth, professional studio sound
 * - HALL: Long decay, spacious, concert hall ambience
 */
class ReverbProcessor
{
public:
    enum class Algorithm { Spring = 0, Plate, Hall };

    ReverbProcessor() = default;

    void prepare(double sampleRate, int samplesPerBlock)
    {
        currentSampleRate = sampleRate;

        // Prepare JUCE reverb
        juce::dsp::ProcessSpec spec;
        spec.sampleRate = sampleRate;
        spec.maximumBlockSize = static_cast<juce::uint32>(samplesPerBlock);
        spec.numChannels = 2;
        reverb.prepare(spec);

        // Bypass gain smoother
        bypassGain.reset(sampleRate, 0.050);
        bypassGain.setCurrentAndTargetValue(0.0f);

        // Parameter smoothers
        sizeSmooth.reset(sampleRate, 0.050);
        decaySmooth.reset(sampleRate, 0.050);
        dampingSmooth.reset(sampleRate, 0.020);
        mixSmooth.reset(sampleRate, 0.020);
        widthSmooth.reset(sampleRate, 0.020);

        // Default values
        sizeSmooth.setCurrentAndTargetValue(0.5f);
        decaySmooth.setCurrentAndTargetValue(0.5f);
        dampingSmooth.setCurrentAndTargetValue(0.5f);
        mixSmooth.setCurrentAndTargetValue(0.3f);
        widthSmooth.setCurrentAndTargetValue(1.0f);

        updateReverbParams();
    }

    void processBlock(juce::AudioBuffer<float>& buffer)
    {
        const int numChannels = buffer.getNumChannels();
        const int numSamples = buffer.getNumSamples();

        if (numChannels < 1) return;

        // Get current bypass gain
        float currentGain = bypassGain.getCurrentValue();
        float targetGain = bypassGain.getTargetValue();

        // Skip if bypassed
        if (currentGain < 0.001f && targetGain < 0.001f)
        {
            // Consume smoothed values to keep them updated
            for (int i = 0; i < numSamples; ++i)
            {
                bypassGain.getNextValue();
                sizeSmooth.getNextValue();
                decaySmooth.getNextValue();
                dampingSmooth.getNextValue();
                mixSmooth.getNextValue();
                widthSmooth.getNextValue();
            }
            return;
        }

        // Update reverb parameters from smoothed values
        updateReverbParams();

        // Store dry signal
        juce::AudioBuffer<float> dryBuffer(numChannels, numSamples);
        for (int ch = 0; ch < numChannels; ++ch)
            dryBuffer.copyFrom(ch, 0, buffer, ch, 0, numSamples);

        // Process through reverb (this replaces buffer with wet signal)
        juce::dsp::AudioBlock<float> block(buffer);
        juce::dsp::ProcessContextReplacing<float> context(block);
        reverb.process(context);

        // Mix dry/wet with send-style mixing (dry stays full, wet added on top)
        for (int i = 0; i < numSamples; ++i)
        {
            float gain = bypassGain.getNextValue();
            float mix = mixSmooth.getNextValue();
            float wetAmount = mix * gain;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                float dry = dryBuffer.getSample(ch, i);
                float wet = buffer.getSample(ch, i);
                // Send-style: dry + wet * mix (preserves original signal)
                buffer.setSample(ch, i, dry + wet * wetAmount);
            }
        }
    }

    // Parameter setters (normalized 0-1)
    void setSize(float norm) { sizeSmooth.setTargetValue(norm); }
    void setDecay(float norm) { decaySmooth.setTargetValue(norm); }
    void setDamping(float norm) { dampingSmooth.setTargetValue(norm); }
    void setMix(float norm) { mixSmooth.setTargetValue(norm); }
    void setWidth(float norm) { widthSmooth.setTargetValue(norm); }

    // These are handled by the algorithm preset now
    void setPreDelay(float /*norm*/) { /* Not used with JUCE reverb */ }
    void setModRate(float /*norm*/) { /* Not used with JUCE reverb */ }
    void setModDepth(float /*norm*/) { /* Not used with JUCE reverb */ }

    void setAlgorithm(int algo)
    {
        currentAlgorithm = static_cast<Algorithm>(std::clamp(algo, 0, 2));
        updateReverbParams();
    }

    int getAlgorithm() const { return static_cast<int>(currentAlgorithm); }

    void setEnabled(bool on)
    {
        bypassGain.setTargetValue(on ? 1.0f : 0.0f);
        enabled.store(on);
    }

    bool getEnabled() const { return enabled.load(); }

private:
    void updateReverbParams()
    {
        juce::Reverb::Parameters params;

        float size = sizeSmooth.getCurrentValue();
        float decay = decaySmooth.getCurrentValue();
        float damping = dampingSmooth.getCurrentValue();
        float width = widthSmooth.getCurrentValue();

        // Base parameters that all algorithms share
        params.width = width;
        params.freezeMode = 0.0f;

        switch (currentAlgorithm)
        {
            case Algorithm::Spring:
                // Spring: shorter, brighter, more intimate
                params.roomSize = 0.3f + size * 0.3f;     // 0.3-0.6 (smaller room)
                params.damping = 0.2f + damping * 0.4f;   // 0.2-0.6 (brighter)
                params.wetLevel = 0.6f + decay * 0.3f;    // Shorter tail
                params.dryLevel = 0.0f;                    // We handle dry/wet ourselves
                break;

            case Algorithm::Plate:
                // Plate: medium, smooth, professional
                params.roomSize = 0.5f + size * 0.35f;    // 0.5-0.85 (medium room)
                params.damping = 0.3f + damping * 0.5f;   // 0.3-0.8 (balanced)
                params.wetLevel = 0.7f + decay * 0.25f;   // Medium tail
                params.dryLevel = 0.0f;
                break;

            case Algorithm::Hall:
                // Hall: spacious, long decay, dark
                params.roomSize = 0.7f + size * 0.29f;    // 0.7-0.99 (large room)
                params.damping = 0.4f + damping * 0.55f;  // 0.4-0.95 (darker)
                params.wetLevel = 0.8f + decay * 0.19f;   // Long tail
                params.dryLevel = 0.0f;
                break;
        }

        reverb.setParameters(params);
    }

    double currentSampleRate = 44100.0;
    Algorithm currentAlgorithm = Algorithm::Plate;

    // JUCE's high-quality reverb
    juce::dsp::Reverb reverb;

    // Bypass
    juce::SmoothedValue<float> bypassGain;
    std::atomic<bool> enabled { false };

    // Parameter smoothers
    juce::SmoothedValue<float> sizeSmooth;
    juce::SmoothedValue<float> decaySmooth;
    juce::SmoothedValue<float> dampingSmooth;
    juce::SmoothedValue<float> mixSmooth;
    juce::SmoothedValue<float> widthSmooth;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReverbProcessor)
};
