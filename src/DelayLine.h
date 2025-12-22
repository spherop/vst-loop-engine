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
        lfoPhase = 0.0f;
        noiseState = 0.0f;

        // Prepare the lowpass filter for analog warmth
        prepareFilter(sampleRate);

        // Prepare smoothed values - original params
        delayTimeSamples.reset(sampleRate, 0.05); // 50ms smoothing
        feedbackGain.reset(sampleRate, 0.02);     // 20ms smoothing
        filterCutoff.reset(sampleRate, 0.02);     // 20ms smoothing

        // BBD character params
        ageAmount.reset(sampleRate, 0.05);
        modRateHz.reset(sampleRate, 0.1);
        modDepthSamples.reset(sampleRate, 0.05);
        warmthAmount.reset(sampleRate, 0.02);
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

    // BBD Character setters
    void setAge(float agePercent)
    {
        // Age controls noise and saturation amount (0-100%)
        ageAmount.setTargetValue(std::clamp(agePercent / 100.0f, 0.0f, 1.0f));
    }

    void setModRate(float rateHz)
    {
        // LFO rate for delay time modulation (0.1-5 Hz)
        modRateHz.setTargetValue(std::clamp(rateHz, 0.1f, 5.0f));
    }

    void setModDepth(float depthMs)
    {
        // Modulation depth in samples (0-20ms)
        const float depthSamples = (depthMs / 1000.0f) * static_cast<float>(currentSampleRate);
        modDepthSamples.setTargetValue(std::clamp(depthSamples, 0.0f, static_cast<float>(currentSampleRate) * 0.02f));
    }

    void setWarmth(float warmthPercent)
    {
        // Additional saturation amount (0-100%)
        warmthAmount.setTargetValue(std::clamp(warmthPercent / 100.0f, 0.0f, 1.0f));
    }

    float processSample(float inputSample)
    {
        // Get smoothed values - original params
        const float currentDelaySamples = delayTimeSamples.getNextValue();
        const float currentFeedback = feedbackGain.getNextValue();
        const float currentCutoff = filterCutoff.getNextValue();

        // Get smoothed BBD params
        const float currentAge = ageAmount.getNextValue();
        const float currentModRate = modRateHz.getNextValue();
        const float currentModDepth = modDepthSamples.getNextValue();
        const float currentWarmth = warmthAmount.getNextValue();

        // Update filter if cutoff changed significantly
        updateFilterCutoff(currentCutoff);

        // Calculate LFO modulation (triangle wave for smooth wobble)
        const float lfoValue = calculateLFO(currentModRate);

        // Apply modulation to delay time
        const float modulatedDelay = std::clamp(
            currentDelaySamples + (lfoValue * currentModDepth),
            1.0f,
            static_cast<float>(maxDelaySamples - 1)
        );

        // Read from delay line with linear interpolation
        float delayedSample = readWithInterpolation(modulatedDelay);

        // Apply BBD-style saturation (warmth)
        delayedSample = bbdSaturate(delayedSample, currentWarmth);

        // Apply lowpass filter to delayed signal (analog warmth)
        float filteredSample = processToneFilter(delayedSample);

        // Inject colored noise based on age
        filteredSample += bbdNoise(currentAge);

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
        noiseState = 0.0f;
        lfoPhase = 0.0f;
    }

private:
    std::vector<float> buffer;
    int writeIndex = 0;
    int maxDelaySamples = 0;
    double currentSampleRate = 44100.0;

    // Original smoothed parameters
    juce::SmoothedValue<float> delayTimeSamples;
    juce::SmoothedValue<float> feedbackGain;
    juce::SmoothedValue<float> filterCutoff;

    // BBD character smoothed parameters
    juce::SmoothedValue<float> ageAmount;
    juce::SmoothedValue<float> modRateHz;
    juce::SmoothedValue<float> modDepthSamples;
    juce::SmoothedValue<float> warmthAmount;

    // Simple one-pole lowpass filter state
    float filterState = 0.0f;
    float filterCoeff = 0.5f;
    float lastCutoff = 4000.0f;

    // BBD modulation and noise state
    float lfoPhase = 0.0f;
    float noiseState = 0.0f;
    juce::Random noiseGen;

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

    float calculateLFO(float rateHz)
    {
        // Triangle wave LFO for smooth delay time wobble
        lfoPhase += rateHz / static_cast<float>(currentSampleRate);
        if (lfoPhase >= 1.0f)
            lfoPhase -= 1.0f;

        // Convert to triangle wave (-1 to +1)
        return (lfoPhase < 0.5f)
            ? (4.0f * lfoPhase - 1.0f)
            : (3.0f - 4.0f * lfoPhase);
    }

    float bbdSaturate(float x, float amount)
    {
        // BBD-style asymmetric soft saturation
        // More drive = more harmonic content (warmth)
        if (amount < 0.001f)
            return x;

        const float drive = 1.0f + amount * 3.0f;
        const float shaped = std::tanh(x * drive);
        // Normalize to maintain volume
        return shaped / std::tanh(drive);
    }

    float bbdNoise(float age)
    {
        // Filtered noise that increases with age
        // Simulates bucket brigade charge transfer imperfections
        if (age < 0.001f)
            return 0.0f;

        // Generate white noise
        const float noise = noiseGen.nextFloat() * 2.0f - 1.0f;

        // Apply simple lowpass for colored noise (more realistic)
        noiseState = noiseState * 0.9f + noise * 0.1f;

        // Scale by age - more age = more noise
        return noiseState * age * 0.015f;
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
