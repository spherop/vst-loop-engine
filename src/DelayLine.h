#pragma once

#include <juce_dsp/juce_dsp.h>
#include <vector>

class DelayLine
{
public:
    DelayLine() = default;

    void prepare(double sampleRate, int maxDelayMs = 2000)
    {
        currentSampleRate = sampleRate;
        maxDelaySamples = static_cast<int>((maxDelayMs / 1000.0) * sampleRate) + 1;

        buffer.resize(maxDelaySamples);
        std::fill(buffer.begin(), buffer.end(), 0.0f);

        writeIndex = 0;

        // Prepare the lowpass filter for analog warmth
        prepareFilter(sampleRate);

        // Prepare smoothed values
        delayTimeSamples.reset(sampleRate, 0.05); // 50ms smoothing
        feedbackGain.reset(sampleRate, 0.02);     // 20ms smoothing
        filterCutoff.reset(sampleRate, 0.02);     // 20ms smoothing
    }

    void setDelayTime(float delayMs)
    {
        const float delaySamples = (delayMs / 1000.0f) * static_cast<float>(currentSampleRate);
        delayTimeSamples.setTargetValue(std::clamp(delaySamples, 1.0f, static_cast<float>(maxDelaySamples - 1)));
    }

    void setFeedback(float feedbackPercent)
    {
        // Cap at 95% for stability
        feedbackGain.setTargetValue(std::clamp(feedbackPercent / 100.0f, 0.0f, 0.95f));
    }

    void setTone(float cutoffHz)
    {
        filterCutoff.setTargetValue(std::clamp(cutoffHz, 200.0f, 12000.0f));
    }

    float processSample(float inputSample)
    {
        // Get smoothed values
        const float currentDelaySamples = delayTimeSamples.getNextValue();
        const float currentFeedback = feedbackGain.getNextValue();
        const float currentCutoff = filterCutoff.getNextValue();

        // Update filter if cutoff changed significantly
        updateFilterCutoff(currentCutoff);

        // Read from delay line with linear interpolation
        const float delayedSample = readWithInterpolation(currentDelaySamples);

        // Apply lowpass filter to delayed signal (analog warmth)
        const float filteredSample = processToneFilter(delayedSample);

        // Calculate what goes back into the delay line
        const float feedbackSample = inputSample + (filteredSample * currentFeedback);

        // Soft clip the feedback to prevent runaway
        const float clippedFeedback = softClip(feedbackSample);

        // Write to delay line
        buffer[writeIndex] = clippedFeedback;

        // Advance write position
        writeIndex = (writeIndex + 1) % maxDelaySamples;

        return filteredSample;
    }

    void clear()
    {
        std::fill(buffer.begin(), buffer.end(), 0.0f);
        filterState = 0.0f;
    }

private:
    std::vector<float> buffer;
    int writeIndex = 0;
    int maxDelaySamples = 0;
    double currentSampleRate = 44100.0;

    // Smoothed parameters
    juce::SmoothedValue<float> delayTimeSamples;
    juce::SmoothedValue<float> feedbackGain;
    juce::SmoothedValue<float> filterCutoff;

    // Simple one-pole lowpass filter state
    float filterState = 0.0f;
    float filterCoeff = 0.5f;
    float lastCutoff = 4000.0f;

    float readWithInterpolation(float delaySamples) const
    {
        // Calculate read position
        const float readPos = static_cast<float>(writeIndex) - delaySamples;
        const float wrappedPos = readPos < 0 ? readPos + maxDelaySamples : readPos;

        // Linear interpolation between samples
        const int index0 = static_cast<int>(wrappedPos) % maxDelaySamples;
        const int index1 = (index0 + 1) % maxDelaySamples;
        const float frac = wrappedPos - std::floor(wrappedPos);

        return buffer[index0] * (1.0f - frac) + buffer[index1] * frac;
    }

    void prepareFilter(double sampleRate)
    {
        lastCutoff = 4000.0f;
        calculateFilterCoeff(lastCutoff, sampleRate);
    }

    void updateFilterCutoff(float cutoffHz)
    {
        if (std::abs(cutoffHz - lastCutoff) > 1.0f)
        {
            lastCutoff = cutoffHz;
            calculateFilterCoeff(cutoffHz, currentSampleRate);
        }
    }

    void calculateFilterCoeff(float cutoffHz, double sampleRate)
    {
        // One-pole lowpass coefficient
        const float omega = 2.0f * juce::MathConstants<float>::pi * cutoffHz / static_cast<float>(sampleRate);
        filterCoeff = omega / (omega + 1.0f);
    }

    float processToneFilter(float input)
    {
        filterState += filterCoeff * (input - filterState);
        return filterState;
    }

    static float softClip(float x)
    {
        // Soft saturation to prevent harsh clipping
        if (x > 1.0f)
            return 1.0f - std::exp(-(x - 1.0f));
        else if (x < -1.0f)
            return -1.0f + std::exp(-(-x - 1.0f));
        return x;
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DelayLine)
};
