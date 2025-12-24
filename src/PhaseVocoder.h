#pragma once

#include <juce_dsp/juce_dsp.h>
#include <vector>
#include <cmath>

/**
 * SimplePitchShifter - Granular pitch shifter with crossfading grains
 *
 * Uses two overlapping grains with Hann window crossfading.
 * Simple and reliable approach that works for moderate pitch shifts.
 */
class SimplePitchShifter
{
public:
    static constexpr int BUFFER_SIZE = 8192;  // ~170ms at 48kHz
    static constexpr int GRAIN_SIZE = 2048;   // ~42ms grains

    SimplePitchShifter()
    {
        buffer.resize(BUFFER_SIZE, 0.0f);

        // Pre-calculate Hann window for crossfading
        for (int i = 0; i < GRAIN_SIZE; ++i)
        {
            float t = static_cast<float>(i) / static_cast<float>(GRAIN_SIZE);
            window[i] = 0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * t));
        }
    }

    void prepare(double sampleRate)
    {
        this->sampleRate = sampleRate;
        reset();
    }

    void reset()
    {
        std::fill(buffer.begin(), buffer.end(), 0.0f);
        writePos = 0;
        readPos1 = 0.0f;
        readPos2 = static_cast<float>(GRAIN_SIZE / 2);  // Offset by half grain
        grainPhase = 0.0f;
    }

    void setPitchRatio(float ratio)
    {
        pitchRatio = std::clamp(ratio, 0.5f, 2.0f);
    }

    float processSample(float input)
    {
        // Write input to circular buffer
        buffer[writePos] = input;
        writePos = (writePos + 1) % BUFFER_SIZE;

        // If pitch ratio is 1.0, just return delayed input (no processing needed)
        if (std::abs(pitchRatio - 1.0f) < 0.001f)
        {
            int readIdx = (writePos - GRAIN_SIZE + BUFFER_SIZE) % BUFFER_SIZE;
            return buffer[readIdx];
        }

        // Calculate grain crossfade weights
        float fade1 = window[static_cast<int>(grainPhase * GRAIN_SIZE) % GRAIN_SIZE];
        float fade2 = window[static_cast<int>((grainPhase + 0.5f) * GRAIN_SIZE) % GRAIN_SIZE];

        // Normalize so they sum to 1
        float fadeSum = fade1 + fade2;
        if (fadeSum > 0.0f)
        {
            fade1 /= fadeSum;
            fade2 /= fadeSum;
        }

        // Read from both grain positions with interpolation
        float sample1 = readInterpolated(readPos1);
        float sample2 = readInterpolated(readPos2);

        // Mix the two grains
        float output = sample1 * fade1 + sample2 * fade2;

        // Advance read positions at pitch-shifted rate
        float readIncrement = pitchRatio;
        readPos1 += readIncrement;
        readPos2 += readIncrement;

        // Wrap read positions within buffer
        while (readPos1 >= BUFFER_SIZE) readPos1 -= BUFFER_SIZE;
        while (readPos2 >= BUFFER_SIZE) readPos2 -= BUFFER_SIZE;
        while (readPos1 < 0) readPos1 += BUFFER_SIZE;
        while (readPos2 < 0) readPos2 += BUFFER_SIZE;

        // Advance grain phase
        grainPhase += 1.0f / static_cast<float>(GRAIN_SIZE);

        // When grain phase wraps, reset the older grain to current write position
        if (grainPhase >= 1.0f)
        {
            grainPhase -= 1.0f;
            // Reset grain 1 to track the write position (with some delay)
            readPos1 = static_cast<float>((writePos - GRAIN_SIZE + BUFFER_SIZE) % BUFFER_SIZE);
        }
        else if (grainPhase >= 0.5f && grainPhase - (1.0f / static_cast<float>(GRAIN_SIZE)) < 0.5f)
        {
            // Just crossed 0.5 - reset grain 2
            readPos2 = static_cast<float>((writePos - GRAIN_SIZE + BUFFER_SIZE) % BUFFER_SIZE);
        }

        return output;
    }

    int getLatencySamples() const
    {
        return GRAIN_SIZE;
    }

private:
    double sampleRate = 44100.0;
    float pitchRatio = 1.0f;

    std::vector<float> buffer;
    int writePos = 0;
    float readPos1 = 0.0f;
    float readPos2 = 0.0f;
    float grainPhase = 0.0f;

    std::array<float, GRAIN_SIZE> window{};

    float readInterpolated(float pos) const
    {
        int idx0 = static_cast<int>(pos) % BUFFER_SIZE;
        int idx1 = (idx0 + 1) % BUFFER_SIZE;
        float frac = pos - std::floor(pos);
        return buffer[idx0] * (1.0f - frac) + buffer[idx1] * frac;
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SimplePitchShifter)
};

/**
 * StereoPhaseVocoder - Stereo wrapper (keeping name for compatibility)
 */
class StereoPhaseVocoder
{
public:
    StereoPhaseVocoder() = default;

    void prepare(double sampleRate)
    {
        shifterL.prepare(sampleRate);
        shifterR.prepare(sampleRate);
    }

    void reset()
    {
        shifterL.reset();
        shifterR.reset();
    }

    void setPitchRatio(float ratio)
    {
        shifterL.setPitchRatio(ratio);
        shifterR.setPitchRatio(ratio);
    }

    void processSample(float inputL, float inputR, float& outputL, float& outputR)
    {
        outputL = shifterL.processSample(inputL);
        outputR = shifterR.processSample(inputR);
    }

    int getLatencySamples() const
    {
        return shifterL.getLatencySamples();
    }

private:
    SimplePitchShifter shifterL;
    SimplePitchShifter shifterR;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StereoPhaseVocoder)
};
