#pragma once

// Use Apple Accelerate for fast FFTs on macOS
#if defined(__APPLE__)
#define SIGNALSMITH_USE_ACCELERATE
#endif

#include "signalsmith-stretch.h"
#include <vector>
#include <cmath>

/**
 * SignalsmithPitchShifter - High-quality pitch shifter using Signalsmith Stretch
 *
 * This wraps the Signalsmith Stretch library for professional-quality pitch shifting
 * that maintains audio quality across a wide range of pitch shifts.
 */
class SignalsmithPitchShifter
{
public:
    SignalsmithPitchShifter() = default;

    void prepare(double newSampleRate)
    {
        sampleRate = newSampleRate;
        prepared = true;

        // Configure for mono (we'll use two instances for stereo)
        // Use the default preset which balances quality and latency
        stretch.presetDefault(1, static_cast<float>(sampleRate));

        // Get latency info
        inputLatencySamples = stretch.inputLatency();
        outputLatencySamples = stretch.outputLatency();
        totalLatency = inputLatencySamples + outputLatencySamples;

        // Allocate buffers
        inputBuffer.resize(1);  // 1 channel
        outputBuffer.resize(1);
        inputBuffer[0].resize(MAX_BLOCK_SIZE, 0.0f);
        outputBuffer[0].resize(MAX_BLOCK_SIZE, 0.0f);

        // Ring buffer for sample-by-sample processing
        ringBufferIn.resize(RING_BUFFER_SIZE, 0.0f);
        ringBufferOut.resize(RING_BUFFER_SIZE, 0.0f);

        reset();
    }

    void reset()
    {
        // Only reset if we've been prepared
        if (!prepared)
            return;

        stretch.reset();
        ringWritePos = 0;
        ringReadPos = 0;
        samplesInBuffer = 0;
        pitchRatio = 1.0f;
        targetPitchRatio = 1.0f;
        std::fill(ringBufferIn.begin(), ringBufferIn.end(), 0.0f);
        std::fill(ringBufferOut.begin(), ringBufferOut.end(), 0.0f);
    }

    void setPitchRatio(float ratio)
    {
        if (!prepared)
            return;

        float newRatio = std::clamp(ratio, 0.25f, 4.0f);

        // Smooth pitch ratio changes to prevent clicking
        // Use a simple exponential smoothing with a fast response
        const float smoothingCoeff = 0.995f;  // Fast but smooth
        targetPitchRatio = newRatio;
        pitchRatio = pitchRatio * smoothingCoeff + targetPitchRatio * (1.0f - smoothingCoeff);

        stretch.setTransposeFactor(pitchRatio);
    }

    float processSample(float input)
    {
        // Safety check - return input unchanged if not prepared
        if (!prepared)
            return input;

        // Write input to ring buffer
        ringBufferIn[ringWritePos] = input;
        ringWritePos = (ringWritePos + 1) % RING_BUFFER_SIZE;
        samplesInBuffer++;

        // Process in blocks when we have enough samples
        if (samplesInBuffer >= PROCESS_BLOCK_SIZE)
        {
            processBlock();
        }

        // Read from output ring buffer
        float output = ringBufferOut[ringReadPos];
        ringBufferOut[ringReadPos] = 0.0f;  // Clear after reading
        ringReadPos = (ringReadPos + 1) % RING_BUFFER_SIZE;

        return output;
    }

    int getLatencySamples() const
    {
        if (!prepared)
            return 0;
        return totalLatency + PROCESS_BLOCK_SIZE;
    }

private:
    static constexpr int MAX_BLOCK_SIZE = 2048;
    static constexpr int PROCESS_BLOCK_SIZE = 256;  // Process in small blocks for low latency
    static constexpr int RING_BUFFER_SIZE = 8192;   // Must be > total latency + block size

    double sampleRate = 44100.0;
    float pitchRatio = 1.0f;
    float targetPitchRatio = 1.0f;  // Target for smoothing
    bool prepared = false;  // Track if prepare() has been called

    signalsmith::stretch::SignalsmithStretch<float> stretch;

    int inputLatencySamples = 0;
    int outputLatencySamples = 0;
    int totalLatency = 0;

    // Buffers for Signalsmith
    std::vector<std::vector<float>> inputBuffer;
    std::vector<std::vector<float>> outputBuffer;

    // Ring buffers for sample-by-sample processing
    std::vector<float> ringBufferIn;
    std::vector<float> ringBufferOut;
    int ringWritePos = 0;
    int ringReadPos = 0;
    int samplesInBuffer = 0;

    void processBlock()
    {
        // Copy samples from input ring buffer to processing buffer
        int startPos = (ringWritePos - samplesInBuffer + RING_BUFFER_SIZE) % RING_BUFFER_SIZE;
        for (int i = 0; i < samplesInBuffer; ++i)
        {
            inputBuffer[0][i] = ringBufferIn[(startPos + i) % RING_BUFFER_SIZE];
        }

        // Create pointer arrays for Signalsmith
        float* inPtrs[1] = { inputBuffer[0].data() };
        float* outPtrs[1] = { outputBuffer[0].data() };

        // For pitch shifting without time stretching, input and output sizes are the same
        int inputSamples = samplesInBuffer;
        int outputSamples = samplesInBuffer;

        // Process through Signalsmith
        stretch.process(inPtrs, inputSamples, outPtrs, outputSamples);

        // Copy output to output ring buffer using overlap-add
        // Note: output comes with latency, so we offset the write position
        int outWritePos = (ringReadPos + totalLatency) % RING_BUFFER_SIZE;
        for (int i = 0; i < outputSamples; ++i)
        {
            int idx = (outWritePos + i) % RING_BUFFER_SIZE;
            ringBufferOut[idx] += outputBuffer[0][i];
        }

        samplesInBuffer = 0;  // Reset counter
    }
};

/**
 * StereoPhaseVocoder - Stereo wrapper for pitch shifting
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
    SignalsmithPitchShifter shifterL;
    SignalsmithPitchShifter shifterR;
};
