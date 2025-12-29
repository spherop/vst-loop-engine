#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <cmath>

/**
 * SubBassProcessor - Octave-down sub-bass generator
 *
 * Algorithm: Zero-crossing detection + polarity flip for octave-down effect
 * - Detects zero crossings in the input signal
 * - Flips polarity every other crossing = octave down
 * - Envelope follower for natural dynamics
 * - Low-pass filtered for clean sub bass
 */
class SubBassProcessor
{
public:
    SubBassProcessor() = default;

    void prepare(double sampleRate, int /*samplesPerBlock*/)
    {
        currentSampleRate = sampleRate;

        // Initialize bypass gain smoother for click-free toggling
        bypassGain.reset(sampleRate, 0.050);
        bypassGain.setCurrentAndTargetValue(0.0f);

        // Parameter smoothers
        frequencySmooth.reset(sampleRate, 0.020);
        frequencySmooth.setCurrentAndTargetValue(60.0f);  // Default 60Hz

        amountSmooth.reset(sampleRate, 0.020);
        amountSmooth.setCurrentAndTargetValue(0.5f);

        // Reset state
        prevSampleL = 0.0f;
        prevSampleR = 0.0f;
        subPolarityL = false;
        subPolarityR = false;
        subOscL = 0.0f;
        subOscR = 0.0f;
        envFollowerL = 0.0f;
        envFollowerR = 0.0f;
        subLpStateL = 0.0f;
        subLpStateR = 0.0f;
    }

    void processBlock(juce::AudioBuffer<float>& buffer)
    {
        const int numChannels = buffer.getNumChannels();
        const int numSamples = buffer.getNumSamples();

        if (numChannels < 1) return;

        float* leftData = buffer.getWritePointer(0);
        float* rightData = numChannels > 1 ? buffer.getWritePointer(1) : nullptr;

        for (int i = 0; i < numSamples; ++i)
        {
            const float gain = bypassGain.getNextValue();

            // Skip processing if bypassed
            if (gain < 0.001f)
            {
                // Consume smoothed values to keep them updated
                frequencySmooth.getNextValue();
                amountSmooth.getNextValue();
                continue;
            }

            const float freq = frequencySmooth.getNextValue();
            const float amount = amountSmooth.getNextValue();

            // Low-pass filter coefficient for sub output
            const float lpCoeff = std::clamp(
                2.0f * juce::MathConstants<float>::pi * freq / static_cast<float>(currentSampleRate),
                0.001f, 0.5f
            );

            // Envelope follower attack/release - fast attack for responsive sub
            const float envAttack = 0.3f;   // Much faster attack
            const float envRelease = 0.999f; // Slightly faster release

            // Process left channel
            float inputL = leftData[i];

            // Zero-crossing detection
            if ((prevSampleL <= 0.0f && inputL > 0.0f) ||
                (prevSampleL >= 0.0f && inputL < 0.0f))
            {
                subPolarityL = !subPolarityL;
            }
            prevSampleL = inputL;

            // Octave-down oscillator (square wave at half frequency)
            subOscL = subPolarityL ? 1.0f : -1.0f;

            // Envelope follower for dynamics
            float inputLevelL = std::abs(inputL);
            if (inputLevelL > envFollowerL)
                envFollowerL = envFollowerL * (1.0f - envAttack) + inputLevelL * envAttack;
            else
                envFollowerL = envFollowerL * envRelease;

            // Apply envelope to sub oscillator
            float subL = subOscL * envFollowerL;

            // Low-pass filter the sub for clean bass
            subLpStateL = subLpStateL * (1.0f - lpCoeff) + subL * lpCoeff;

            // Mix sub into output
            leftData[i] = inputL + subLpStateL * amount * gain;

            // Process right channel
            if (rightData)
            {
                float inputR = rightData[i];

                if ((prevSampleR <= 0.0f && inputR > 0.0f) ||
                    (prevSampleR >= 0.0f && inputR < 0.0f))
                {
                    subPolarityR = !subPolarityR;
                }
                prevSampleR = inputR;

                subOscR = subPolarityR ? 1.0f : -1.0f;

                float inputLevelR = std::abs(inputR);
                if (inputLevelR > envFollowerR)
                    envFollowerR = envFollowerR * (1.0f - envAttack) + inputLevelR * envAttack;
                else
                    envFollowerR = envFollowerR * envRelease;

                float subR = subOscR * envFollowerR;
                subLpStateR = subLpStateR * (1.0f - lpCoeff) + subR * lpCoeff;

                rightData[i] = inputR + subLpStateR * amount * gain;
            }
        }
    }

    // Parameter setters (normalized 0-1)
    void setFrequency(float normalized)
    {
        // Map 0-1 to 30-80Hz
        float hz = 30.0f + normalized * 50.0f;
        frequencySmooth.setTargetValue(hz);
    }

    void setAmount(float normalized)
    {
        // Map 0-1 to 0-400% for STRONG sub bass effect
        amountSmooth.setTargetValue(normalized * 4.0f);
    }

    void setEnabled(bool on)
    {
        bypassGain.setTargetValue(on ? 1.0f : 0.0f);
        enabled.store(on);
    }

    bool getEnabled() const { return enabled.load(); }

private:
    double currentSampleRate = 44100.0;

    // Bypass gain for smooth enable/disable
    juce::SmoothedValue<float> bypassGain;
    std::atomic<bool> enabled { false };

    // Parameter smoothers
    juce::SmoothedValue<float> frequencySmooth;
    juce::SmoothedValue<float> amountSmooth;

    // Zero-crossing state
    float prevSampleL = 0.0f;
    float prevSampleR = 0.0f;
    bool subPolarityL = false;
    bool subPolarityR = false;

    // Octave-down oscillator state
    float subOscL = 0.0f;
    float subOscR = 0.0f;

    // Envelope followers
    float envFollowerL = 0.0f;
    float envFollowerR = 0.0f;

    // LP filter state for clean sub
    float subLpStateL = 0.0f;
    float subLpStateR = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SubBassProcessor)
};
