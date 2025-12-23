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
        juce::String msg = "LoopBuffer::prepare() sampleRate=" + juce::String(sampleRate);
        DBG(msg);

        // Write to log file for DAW debugging - Documents folder
        {
            juce::File logFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                .getChildFile("LoopEngine_debug.log");
            logFile.appendText(juce::Time::getCurrentTime().toString(true, true) + " - " + msg + "\n");
        }

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
    void startRecording(int targetLengthSamples = 0)
    {
        if (state.load() == State::Idle)
        {
            clear();
            targetLoopLength = targetLengthSamples;  // 0 = free/unlimited
            state.store(State::Recording);
            DBG("LoopBuffer::startRecording() targetLength=" + juce::String(targetLengthSamples) +
                " samples (" + juce::String(targetLengthSamples / currentSampleRate, 2) + "s)");
        }
    }

    int getTargetLoopLength() const { return targetLoopLength; }

    // stopRecording with option to continue into overdub mode
    void stopRecording(bool continueToOverdub = false)
    {
        if (state.load() == State::Recording)
        {
            // Finalize loop length
            loopLength = writeHead;
            loopEnd = loopLength;
            playHead = 0.0f;

            // If continueToOverdub is true, go into overdub mode instead of just playing
            if (continueToOverdub)
            {
                state.store(State::Overdubbing);
            }
            else
            {
                state.store(State::Playing);
            }

            juce::String msg = "LoopBuffer::stopRecording() - loopLength=" + juce::String(loopLength) +
                " samples (" + juce::String(loopLength / currentSampleRate, 2) + "s)" +
                " at sampleRate=" + juce::String(currentSampleRate) +
                " continueToOverdub=" + juce::String(continueToOverdub ? "true" : "false");
            DBG(msg);

            // Write to log file for DAW debugging - Documents folder
            {
                juce::File logFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                    .getChildFile("LoopEngine_debug.log");
                logFile.appendText(juce::Time::getCurrentTime().toString(true, true) + " - " + msg + "\n");

                // Also log the current playback rate
                juce::String rateMsg = "Current playbackRate target=" + juce::String(playbackRateSmoothed.getTargetValue(), 3);
                logFile.appendText(juce::Time::getCurrentTime().toString(true, true) + " - " + rateMsg + "\n");
            }

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
            // Start from appropriate end based on direction
            if (isReversed.load())
            {
                // For reverse playback, start from the end
                const int effectiveEnd = loopEnd > 0 ? loopEnd : loopLength;
                playHead = static_cast<float>(effectiveEnd - 1);
            }
            else
            {
                // For forward playback, start from the beginning
                playHead = static_cast<float>(loopStart);
            }
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
        float clampedRate = std::clamp(rate, 0.25f, 4.0f);
        playbackRateSmoothed.setTargetValue(clampedRate);
        // Only log when rate actually changes significantly
        static float lastLoggedRate = 1.0f;
        if (std::abs(clampedRate - lastLoggedRate) > 0.01f)
        {
            DBG("LoopBuffer::setPlaybackRate(" + juce::String(rate, 3) + ") -> clamped to " + juce::String(clampedRate, 3));
            lastLoggedRate = clampedRate;
        }
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
                    processPlaying(inputL, inputR, outputL, outputR);
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
    bool getIsReversed() const { return isReversed.load(); }

    // Get waveform data for UI visualization (downsampled)
    // Works during recording (uses writeHead) and playback (uses loopLength)
    std::vector<float> getWaveformData(int numPoints) const
    {
        std::vector<float> waveform(numPoints, 0.0f);

        // Determine the length to visualize
        int visualLength = loopLength;
        if (state.load() == State::Recording)
        {
            // During recording, show what we've recorded so far
            // Use target length if set, otherwise use current writeHead position
            if (targetLoopLength > 0)
            {
                visualLength = targetLoopLength;  // Show full target length
            }
            else
            {
                visualLength = writeHead;  // Show recorded portion
            }
        }

        if (visualLength <= 0)
            return waveform;

        const int samplesPerPoint = visualLength / numPoints;
        if (samplesPerPoint <= 0)
            return waveform;

        // For recording with target length, only show samples up to writeHead
        const int maxSampleToShow = (state.load() == State::Recording) ? writeHead : visualLength;

        for (int i = 0; i < numPoints; ++i)
        {
            float maxVal = 0.0f;
            const int startSample = i * samplesPerPoint;
            const int endSample = std::min(startSample + samplesPerPoint, maxSampleToShow);

            // Only process if we have samples in this range
            if (startSample < maxSampleToShow)
            {
                for (int j = startSample; j < endSample; ++j)
                {
                    float val = std::abs(bufferL[j]) + std::abs(bufferR[j]);
                    maxVal = std::max(maxVal, val * 0.5f);
                }
            }
            waveform[i] = std::min(maxVal, 1.0f);
        }

        return waveform;
    }

    // Get recording progress (0-1) for UI
    float getRecordingProgress() const
    {
        if (state.load() != State::Recording || targetLoopLength <= 0)
            return 0.0f;
        return static_cast<float>(writeHead) / static_cast<float>(targetLoopLength);
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

    // Preset loop length (0 = free/unlimited)
    int targetLoopLength = 0;

    void processRecording(float inputL, float inputR, float& outputL, float& outputR)
    {
        // Check if we've reached the target length (if set)
        const int effectiveMaxLength = (targetLoopLength > 0) ? targetLoopLength : maxLoopSamples;

        if (writeHead < effectiveMaxLength)
        {
            bufferL[writeHead] = inputL;
            bufferR[writeHead] = inputR;
            ++writeHead;
        }
        else
        {
            // Target or max loop length reached - auto-stop recording
            // Blooper-style: just switch to play mode, user taps REC again to overdub
            stopRecording(false);
        }

        // Output the input (monitoring)
        outputL = inputL;
        outputR = inputR;
    }

    void processPlaying(float inputL, float inputR, float& outputL, float& outputR)
    {
        if (loopLength <= 0)
        {
            // Pass through input if no loop content
            outputL = inputL;
            outputR = inputR;
            return;
        }

        // Read loop playback with interpolation
        float loopL = readWithInterpolation(bufferL, playHead);
        float loopR = readWithInterpolation(bufferR, playHead);

        // Mix loop playback with input passthrough for seamless monitoring
        // This allows hearing the instrument while the loop plays
        outputL = loopL + inputL;
        outputR = loopR + inputR;

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
        const float effectiveLength = static_cast<float>(effectiveEnd - effectiveStart);

        if (effectiveLength <= 0)
            return;

        if (reversed)
        {
            playHead -= rate;
            // Wrap around for proper looping at any speed
            while (playHead < effectiveStart)
            {
                playHead += effectiveLength;
            }
        }
        else
        {
            playHead += rate;
            // Wrap around for proper looping at any speed
            while (playHead >= effectiveEnd)
            {
                playHead -= effectiveLength;
            }
        }
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
