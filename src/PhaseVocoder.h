#pragma once

// Use Apple Accelerate for fast FFTs on macOS
#if defined(__APPLE__)
#define SIGNALSMITH_USE_ACCELERATE
#endif

#include "signalsmith-stretch.h"
#include <vector>
#include <cmath>

/**
 * BlockPitchShifter - Efficient block-based pitch shifter using Signalsmith Stretch
 *
 * Optimized for processing audio in blocks rather than sample-by-sample.
 * This dramatically reduces CPU overhead when multiple instances are running.
 */
class BlockPitchShifter
{
public:
    BlockPitchShifter() = default;

    void prepare(double newSampleRate, int maxBlockSize)
    {
        sampleRate = newSampleRate;
        prepared = true;

        // Configure for mono - use cheaper preset for better performance
        // presetCheaper trades some quality for lower CPU usage
        stretch.presetCheaper(1, static_cast<float>(sampleRate));

        // Get latency info
        inputLatencySamples = stretch.inputLatency();
        outputLatencySamples = stretch.outputLatency();
        totalLatency = inputLatencySamples + outputLatencySamples;

        // Pre-allocate processing buffers sized for expected block sizes
        int bufferSize = std::max(maxBlockSize * 2, 1024);
        inputBuffer.resize(bufferSize, 0.0f);
        outputBuffer.resize(bufferSize, 0.0f);

        // Latency compensation buffer - stores delayed output
        latencyBuffer.resize(totalLatency + bufferSize, 0.0f);
        latencyWritePos = 0;
        latencyReadPos = 0;

        reset();
    }

    void reset()
    {
        if (!prepared)
            return;

        stretch.reset();
        pitchRatio = 1.0f;
        targetPitchRatio = 1.0f;
        std::fill(latencyBuffer.begin(), latencyBuffer.end(), 0.0f);
        latencyWritePos = totalLatency;  // Pre-fill latency
        latencyReadPos = 0;
    }

    void setPitchRatio(float ratio)
    {
        if (!prepared)
            return;

        targetPitchRatio = std::clamp(ratio, 0.25f, 4.0f);
    }

    // Process a block of audio - much more efficient than sample-by-sample
    void processBlock(const float* input, float* output, int numSamples)
    {
        if (!prepared || numSamples <= 0)
        {
            // Pass through if not prepared
            if (input != output)
                std::copy(input, input + numSamples, output);
            return;
        }

        // Smooth pitch ratio changes per-block (not per-sample, saves CPU)
        const float smoothingCoeff = 0.9f;
        pitchRatio = pitchRatio * smoothingCoeff + targetPitchRatio * (1.0f - smoothingCoeff);
        stretch.setTransposeFactor(pitchRatio);

        // Copy input to processing buffer
        std::copy(input, input + numSamples, inputBuffer.data());

        // Process through Signalsmith - single call for entire block
        float* inPtr = inputBuffer.data();
        float* outPtr = outputBuffer.data();
        stretch.process(&inPtr, numSamples, &outPtr, numSamples);

        // Write processed output to latency buffer
        for (int i = 0; i < numSamples; ++i)
        {
            latencyBuffer[latencyWritePos] = outputBuffer[i];
            latencyWritePos = (latencyWritePos + 1) % static_cast<int>(latencyBuffer.size());
        }

        // Read from latency buffer (delayed by totalLatency)
        for (int i = 0; i < numSamples; ++i)
        {
            output[i] = latencyBuffer[latencyReadPos];
            latencyBuffer[latencyReadPos] = 0.0f;  // Clear after reading
            latencyReadPos = (latencyReadPos + 1) % static_cast<int>(latencyBuffer.size());
        }
    }

    int getLatencySamples() const
    {
        if (!prepared)
            return 0;
        return totalLatency;
    }

    float getCurrentPitchRatio() const { return pitchRatio; }

private:
    double sampleRate = 44100.0;
    float pitchRatio = 1.0f;
    float targetPitchRatio = 1.0f;
    bool prepared = false;

    signalsmith::stretch::SignalsmithStretch<float> stretch;

    int inputLatencySamples = 0;
    int outputLatencySamples = 0;
    int totalLatency = 0;

    // Processing buffers (no ring buffer overhead)
    std::vector<float> inputBuffer;
    std::vector<float> outputBuffer;

    // Simple latency compensation buffer
    std::vector<float> latencyBuffer;
    int latencyWritePos = 0;
    int latencyReadPos = 0;
};

/**
 * SignalsmithPitchShifter - Sample-by-sample wrapper (legacy compatibility)
 *
 * Kept for backward compatibility but uses the more efficient block processor internally.
 */
class SignalsmithPitchShifter
{
public:
    SignalsmithPitchShifter() = default;

    void prepare(double newSampleRate)
    {
        sampleRate = newSampleRate;
        prepared = true;

        // Use cheaper preset for sample-by-sample (higher CPU load anyway)
        stretch.presetCheaper(1, static_cast<float>(sampleRate));

        inputLatencySamples = stretch.inputLatency();
        outputLatencySamples = stretch.outputLatency();
        totalLatency = inputLatencySamples + outputLatencySamples;

        // Smaller block size for lower latency in sample-by-sample mode
        inputBuffer.resize(1);
        outputBuffer.resize(1);
        inputBuffer[0].resize(PROCESS_BLOCK_SIZE, 0.0f);
        outputBuffer[0].resize(PROCESS_BLOCK_SIZE, 0.0f);

        ringBufferIn.resize(RING_BUFFER_SIZE, 0.0f);
        ringBufferOut.resize(RING_BUFFER_SIZE, 0.0f);

        reset();
    }

    void reset()
    {
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

        targetPitchRatio = std::clamp(ratio, 0.25f, 4.0f);
        // Smoother per-sample updates
        const float smoothingCoeff = 0.999f;
        pitchRatio = pitchRatio * smoothingCoeff + targetPitchRatio * (1.0f - smoothingCoeff);
        stretch.setTransposeFactor(pitchRatio);
    }

    float processSample(float input)
    {
        if (!prepared)
            return input;

        ringBufferIn[ringWritePos] = input;
        ringWritePos = (ringWritePos + 1) % RING_BUFFER_SIZE;
        samplesInBuffer++;

        if (samplesInBuffer >= PROCESS_BLOCK_SIZE)
        {
            processBlock();
        }

        float output = ringBufferOut[ringReadPos];
        ringBufferOut[ringReadPos] = 0.0f;
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
    static constexpr int PROCESS_BLOCK_SIZE = 128;  // Smaller for lower latency
    static constexpr int RING_BUFFER_SIZE = 4096;

    double sampleRate = 44100.0;
    float pitchRatio = 1.0f;
    float targetPitchRatio = 1.0f;
    bool prepared = false;

    signalsmith::stretch::SignalsmithStretch<float> stretch;

    int inputLatencySamples = 0;
    int outputLatencySamples = 0;
    int totalLatency = 0;

    std::vector<std::vector<float>> inputBuffer;
    std::vector<std::vector<float>> outputBuffer;

    std::vector<float> ringBufferIn;
    std::vector<float> ringBufferOut;
    int ringWritePos = 0;
    int ringReadPos = 0;
    int samplesInBuffer = 0;

    void processBlock()
    {
        int startPos = (ringWritePos - samplesInBuffer + RING_BUFFER_SIZE) % RING_BUFFER_SIZE;
        for (int i = 0; i < samplesInBuffer; ++i)
        {
            inputBuffer[0][i] = ringBufferIn[(startPos + i) % RING_BUFFER_SIZE];
        }

        float* inPtrs[1] = { inputBuffer[0].data() };
        float* outPtrs[1] = { outputBuffer[0].data() };

        stretch.process(inPtrs, samplesInBuffer, outPtrs, samplesInBuffer);

        int outWritePos = (ringReadPos + totalLatency) % RING_BUFFER_SIZE;
        for (int i = 0; i < samplesInBuffer; ++i)
        {
            int idx = (outWritePos + i) % RING_BUFFER_SIZE;
            ringBufferOut[idx] += outputBuffer[0][i];
        }

        samplesInBuffer = 0;
    }
};

/**
 * StereoBlockPitchShifter - Efficient stereo block-based pitch shifting
 *
 * Processes both channels in blocks for maximum efficiency.
 * Use this for layer playback where entire blocks are available.
 */
class StereoBlockPitchShifter
{
public:
    StereoBlockPitchShifter() = default;

    void prepare(double sampleRate, int maxBlockSize)
    {
        shifterL.prepare(sampleRate, maxBlockSize);
        shifterR.prepare(sampleRate, maxBlockSize);
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

    // Process stereo block - much more efficient than sample-by-sample
    void processBlock(const float* inputL, const float* inputR,
                      float* outputL, float* outputR, int numSamples)
    {
        shifterL.processBlock(inputL, outputL, numSamples);
        shifterR.processBlock(inputR, outputR, numSamples);
    }

    int getLatencySamples() const
    {
        return shifterL.getLatencySamples();
    }

    float getCurrentPitchRatio() const
    {
        return shifterL.getCurrentPitchRatio();
    }

private:
    BlockPitchShifter shifterL;
    BlockPitchShifter shifterR;
};

/**
 * StereoPhaseVocoder - Stereo wrapper for sample-by-sample pitch shifting (legacy)
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
