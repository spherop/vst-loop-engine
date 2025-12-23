#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <vector>
#include <atomic>

class LoopBuffer
{
public:
    // 60 seconds max loop time at 48kHz
    static constexpr int MAX_LOOP_SECONDS = 60;
    static constexpr int CROSSFADE_SAMPLES = 256;

    enum class State
    {
        Idle = 0,
        Recording = 1,
        Playing = 2,
        Overdubbing = 3
    };

    LoopBuffer() = default;

    void prepare(double sampleRate, int samplesPerBlock)
    {
        currentSampleRate = sampleRate;
        maxLoopSamples = static_cast<int>(MAX_LOOP_SECONDS * sampleRate);

        // Pre-allocate buffers
        bufferL.resize(maxLoopSamples, 0.0f);
        bufferR.resize(maxLoopSamples, 0.0f);

        // Reset state
        clear();

        // Prepare smoothed values
        playbackRateSmoothed.reset(sampleRate, 0.05);  // 50ms smoothing
        playbackRateSmoothed.setCurrentAndTargetValue(1.0f);
    }

    void clear()
    {
        std::fill(bufferL.begin(), bufferL.end(), 0.0f);
        std::fill(bufferR.begin(), bufferR.end(), 0.0f);

        writeHead = 0;
        playHead = 0.0f;
        loopLength = 0;
        loopStart = 0;
        loopEnd = 0;
        state.store(State::Idle);
        isReversed.store(false);
        playbackRateSmoothed.setCurrentAndTargetValue(1.0f);
    }

    // Transport controls
    void startRecording()
    {
        if (state.load() == State::Idle)
        {
            clear();
            state.store(State::Recording);
        }
    }

    void stopRecording()
    {
        if (state.load() == State::Recording)
        {
            // Finalize loop length
            loopLength = writeHead;
            loopEnd = loopLength;
            playHead = 0.0f;
            state.store(State::Playing);

            // Apply crossfade at loop boundary for seamless looping
            applyCrossfade();
        }
    }

    void startOverdub()
    {
        if (state.load() == State::Playing || state.load() == State::Idle)
        {
            if (loopLength > 0)
            {
                state.store(State::Overdubbing);
            }
        }
    }

    void stopOverdub()
    {
        if (state.load() == State::Overdubbing)
        {
            state.store(State::Playing);
            applyCrossfade();
        }
    }

    void play()
    {
        if (loopLength > 0)
        {
            playHead = static_cast<float>(loopStart);
            state.store(State::Playing);
        }
    }

    void stop()
    {
        state.store(State::Idle);
    }

    // Parameter setters
    void setLoopStart(float normalizedPos)
    {
        if (loopLength > 0)
        {
            int newStart = static_cast<int>(normalizedPos * loopLength);
            loopStart = std::clamp(newStart, 0, loopEnd - 1);
        }
    }

    void setLoopEnd(float normalizedPos)
    {
        if (loopLength > 0)
        {
            int newEnd = static_cast<int>(normalizedPos * loopLength);
            loopEnd = std::clamp(newEnd, loopStart + 1, loopLength);
        }
    }

    void setPlaybackRate(float rate)
    {
        // Rate range: 0.25 to 4.0
        playbackRateSmoothed.setTargetValue(std::clamp(rate, 0.25f, 4.0f));
    }

    void setReverse(bool reversed)
    {
        isReversed.store(reversed);
    }

    // Process audio
    void processBlock(juce::AudioBuffer<float>& buffer)
    {
        const int numSamples = buffer.getNumSamples();
        float* leftChannel = buffer.getWritePointer(0);
        float* rightChannel = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;

        State currentState = state.load();

        for (int i = 0; i < numSamples; ++i)
        {
            const float inputL = leftChannel[i];
            const float inputR = rightChannel ? rightChannel[i] : inputL;

            float outputL = 0.0f;
            float outputR = 0.0f;

            switch (currentState)
            {
                case State::Recording:
                    processRecording(inputL, inputR, outputL, outputR);
                    break;

                case State::Playing:
                    processPlaying(outputL, outputR);
                    break;

                case State::Overdubbing:
                    processOverdubbing(inputL, inputR, outputL, outputR);
                    break;

                case State::Idle:
                default:
                    // Pass through input
                    outputL = inputL;
                    outputR = inputR;
                    break;
            }

            leftChannel[i] = outputL;
            if (rightChannel)
                rightChannel[i] = outputR;
        }
    }

    // State getters
    State getState() const { return state.load(); }

    float getPlayheadPosition() const
    {
        if (loopLength <= 0)
            return 0.0f;

        int effectiveLength = loopEnd - loopStart;
        if (effectiveLength <= 0)
            return 0.0f;

        float relativePos = (playHead - loopStart) / static_cast<float>(effectiveLength);
        return std::clamp(relativePos, 0.0f, 1.0f);
    }

    float getLoopLengthSeconds() const
    {
        if (loopLength <= 0 || currentSampleRate <= 0)
            return 0.0f;
        return static_cast<float>(loopLength) / static_cast<float>(currentSampleRate);
    }

    int getLoopLengthSamples() const { return loopLength; }

    bool hasContent() const { return loopLength > 0; }

    // Get waveform data for UI visualization (downsampled)
    std::vector<float> getWaveformData(int numPoints) const
    {
        std::vector<float> waveform(numPoints, 0.0f);
        if (loopLength <= 0)
            return waveform;

        const int samplesPerPoint = loopLength / numPoints;
        if (samplesPerPoint <= 0)
            return waveform;

        for (int i = 0; i < numPoints; ++i)
        {
            float maxVal = 0.0f;
            const int startSample = i * samplesPerPoint;
            const int endSample = std::min(startSample + samplesPerPoint, loopLength);

            for (int j = startSample; j < endSample; ++j)
            {
                float val = std::abs(bufferL[j]) + std::abs(bufferR[j]);
                maxVal = std::max(maxVal, val * 0.5f);
            }
            waveform[i] = std::min(maxVal, 1.0f);
        }

        return waveform;
    }

private:
    std::vector<float> bufferL;
    std::vector<float> bufferR;

    int maxLoopSamples = 0;
    double currentSampleRate = 44100.0;

    // Position tracking
    int writeHead = 0;
    float playHead = 0.0f;
    int loopLength = 0;
    int loopStart = 0;
    int loopEnd = 0;

    // Playback control
    juce::SmoothedValue<float> playbackRateSmoothed;
    std::atomic<bool> isReversed { false };
    std::atomic<State> state { State::Idle };

    void processRecording(float inputL, float inputR, float& outputL, float& outputR)
    {
        if (writeHead < maxLoopSamples)
        {
            bufferL[writeHead] = inputL;
            bufferR[writeHead] = inputR;
            ++writeHead;
        }
        else
        {
            // Max loop length reached, auto-stop recording
            stopRecording();
        }

        // Output the input (monitoring)
        outputL = inputL;
        outputR = inputR;
    }

    void processPlaying(float& outputL, float& outputR)
    {
        if (loopLength <= 0)
        {
            outputL = outputR = 0.0f;
            return;
        }

        // Read with interpolation
        outputL = readWithInterpolation(bufferL, playHead);
        outputR = readWithInterpolation(bufferR, playHead);

        // Advance playhead
        advancePlayhead();
    }

    void processOverdubbing(float inputL, float inputR, float& outputL, float& outputR)
    {
        if (loopLength <= 0)
        {
            outputL = inputL;
            outputR = inputR;
            return;
        }

        // Read existing content
        outputL = readWithInterpolation(bufferL, playHead);
        outputR = readWithInterpolation(bufferR, playHead);

        // Add input to buffer (overdub)
        int writePos = static_cast<int>(playHead) % loopLength;
        bufferL[writePos] += inputL;
        bufferR[writePos] += inputR;

        // Soft clip to prevent runaway
        bufferL[writePos] = softClip(bufferL[writePos]);
        bufferR[writePos] = softClip(bufferR[writePos]);

        // Mix input with output for monitoring
        outputL += inputL;
        outputR += inputR;

        // Advance playhead
        advancePlayhead();
    }

    void advancePlayhead()
    {
        const float rate = playbackRateSmoothed.getNextValue();
        const bool reversed = isReversed.load();
        const int effectiveStart = loopStart;
        const int effectiveEnd = loopEnd > 0 ? loopEnd : loopLength;
        const int effectiveLength = effectiveEnd - effectiveStart;

        if (effectiveLength <= 0)
            return;

        if (reversed)
        {
            playHead -= rate;
            if (playHead < effectiveStart)
            {
                playHead = effectiveEnd - 1 + (playHead - effectiveStart);
            }
        }
        else
        {
            playHead += rate;
            if (playHead >= effectiveEnd)
            {
                playHead = effectiveStart + (playHead - effectiveEnd);
            }
        }

        // Clamp to valid range
        playHead = std::clamp(playHead, static_cast<float>(effectiveStart), static_cast<float>(effectiveEnd - 1));
    }

    float readWithInterpolation(const std::vector<float>& buffer, float position) const
    {
        if (loopLength <= 0)
            return 0.0f;

        const int idx0 = static_cast<int>(position) % loopLength;
        const int idx1 = (idx0 + 1) % loopLength;
        const float frac = position - std::floor(position);

        return buffer[idx0] * (1.0f - frac) + buffer[idx1] * frac;
    }

    void applyCrossfade()
    {
        if (loopLength < CROSSFADE_SAMPLES * 2)
            return;

        // Apply crossfade at the loop boundary
        for (int i = 0; i < CROSSFADE_SAMPLES; ++i)
        {
            const float fadeOut = static_cast<float>(CROSSFADE_SAMPLES - i) / CROSSFADE_SAMPLES;
            const float fadeIn = static_cast<float>(i) / CROSSFADE_SAMPLES;

            const int endIdx = loopLength - CROSSFADE_SAMPLES + i;
            const int startIdx = i;

            // Blend end into start for seamless loop
            bufferL[startIdx] = bufferL[startIdx] * fadeIn + bufferL[endIdx] * fadeOut;
            bufferR[startIdx] = bufferR[startIdx] * fadeIn + bufferR[endIdx] * fadeOut;
        }
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LoopBuffer)
};
