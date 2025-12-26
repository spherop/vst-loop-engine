#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include "PhaseVocoder.h"
#include <vector>
#include <atomic>
#include <array>

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

        pitchRatioSmoothed.reset(sampleRate, 0.1);  // 100ms smoothing for pitch
        pitchRatioSmoothed.setCurrentAndTargetValue(1.0f);

        fadeSmoothed.reset(sampleRate, 0.1);  // 100ms smoothing for fade
        fadeSmoothed.setCurrentAndTargetValue(1.0f);  // Default to 100% (no fade)

        // Prepare phase vocoder for high-quality pitch shifting during playback
        phaseVocoder.prepare(sampleRate);

        // Initialize granular pitch shifter grains (for monitoring/lower latency)
        initGrains();
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
        isMuted.store(false);
        fadeActive.store(false);
        playbackRateSmoothed.setCurrentAndTargetValue(1.0f);
        pitchRatioSmoothed.setCurrentAndTargetValue(1.0f);
        fadeSmoothed.setCurrentAndTargetValue(1.0f);
        currentFadeMultiplier = 1.0f;
        lastPlayheadPosition = 0.0f;

        // Reset pitch shifters
        phaseVocoder.reset();
        initGrains();
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
                // Reset fade multiplier when starting overdub so new content is at full volume
                currentFadeMultiplier = 1.0f;
                state.store(State::Overdubbing);
            }
        }
    }

    // Start overdubbing on a fresh layer - sets up the buffer for the master loop length
    // The new layer starts empty but synced with other layers
    void startOverdubOnNewLayer(int masterLoopLengthSamples)
    {
        // Clear this layer's buffers
        std::fill(bufferL.begin(), bufferL.end(), 0.0f);
        std::fill(bufferR.begin(), bufferR.end(), 0.0f);

        // Set up loop parameters to match master loop
        loopLength = masterLoopLengthSamples;
        loopStart = 0;
        loopEnd = loopLength;
        playHead = 0.0f;  // Will be synced to master playhead
        writeHead = 0;
        currentFadeMultiplier = 1.0f;
        lastPlayheadPosition = 0.0f;

        // Mark as having content immediately so it gets processed
        // (the loopLength > 0 makes hasContent() true)

        state.store(State::Overdubbing);
        DBG("LoopBuffer::startOverdubOnNewLayer() loopLength=" + juce::String(loopLength) +
            " samples (" + juce::String(loopLength / currentSampleRate, 2) + "s)");
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
            // Reset fade multiplier when starting playback
            currentFadeMultiplier = 1.0f;
            lastPlayheadPosition = 0.0f;

            // Start from appropriate end based on direction
            if (isReversed.load())
            {
                // For reverse playback, start from the end
                const int effectiveEnd = loopEnd > 0 ? loopEnd : loopLength;
                playHead = static_cast<float>(effectiveEnd - 1);
                lastPlayheadPosition = 1.0f;  // Start at end for reverse
            }
            else
            {
                // For forward playback, start from the beginning
                playHead = static_cast<float>(loopStart);
                lastPlayheadPosition = 0.0f;
            }
            state.store(State::Playing);
        }
    }

    void stop()
    {
        state.store(State::Idle);
        // Reset pitch shifter to prevent latent audio from continuing
        phaseVocoder.reset();
        // Reset playhead so any subsequent reads return silence until play() is called
        playHead = 0.0f;
        lastPlayheadPosition = 0.0f;
        // Reset granular pitch shifter state
        initGrains();
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

    // Pitch shift in semitones (-12 to +12)
    void setPitchShift(float semitones)
    {
        float clampedSemitones = std::clamp(semitones, -12.0f, 12.0f);
        // Convert semitones to pitch ratio: ratio = 2^(semitones/12)
        float targetRatio = std::pow(2.0f, clampedSemitones / 12.0f);
        pitchRatioSmoothed.setTargetValue(targetRatio);

        // Debug logging
        static int logCounter = 0;
        if (++logCounter % 10000 == 0) {
            DBG("setPitchShift: semitones=" + juce::String(semitones, 2) +
                " -> ratio=" + juce::String(targetRatio, 4));
        }
    }

    float getPitchShift() const
    {
        // Convert ratio back to semitones for UI
        float ratio = pitchRatioSmoothed.getTargetValue();
        return 12.0f * std::log2(ratio);
    }

    // Fade/decay amount: 0.0 = fade after one loop, 1.0 = no fade (infinite)
    void setFade(float fadeAmount)
    {
        float clampedFade = std::clamp(fadeAmount, 0.0f, 1.0f);
        fadeSmoothed.setTargetValue(clampedFade);
    }

    float getFade() const
    {
        return fadeSmoothed.getTargetValue();
    }

    // Set whether fade should be active (applies during playback when any layer is recording/overdubbing)
    void setFadeActive(bool active)
    {
        fadeActive.store(active);
    }

    bool getFadeActive() const
    {
        return fadeActive.load();
    }

    float getCurrentFadeMultiplier() const
    {
        return currentFadeMultiplier;
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

    // Get raw playhead position for syncing
    float getRawPlayhead() const { return playHead; }

    // Set playhead position (for syncing with other layers)
    void setPlayhead(float position) { playHead = position; }

    bool hasContent() const { return loopLength > 0; }
    bool getIsReversed() const { return isReversed.load(); }

    // Mute control
    void setMuted(bool muted) { isMuted.store(muted); }
    bool getMuted() const { return isMuted.load(); }

    // Get waveform data for UI visualization (downsampled)
    // Works during recording (uses writeHead) and playback (uses loopLength)
    // ALWAYS applies current fade multiplier so waveform visually reflects faded audio
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

        // ALWAYS apply fade multiplier to waveform display so it visually reflects faded audio
        // This ensures the waveform shrinks/grows with the audio level even after overdubbing stops
        const float fadeMultiplier = currentFadeMultiplier;

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
            // Apply fade multiplier to visual representation
            waveform[i] = std::min(maxVal * fadeMultiplier, 1.0f);
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
    juce::SmoothedValue<float> pitchRatioSmoothed;
    juce::SmoothedValue<float> fadeSmoothed;  // 0.0 = full fade, 1.0 = no fade
    std::atomic<bool> isReversed { false };
    std::atomic<bool> isMuted { false };
    std::atomic<bool> fadeActive { false };  // True when fade should apply during playback (any layer is recording)
    std::atomic<State> state { State::Idle };

    // Preset loop length (0 = free/unlimited)
    int targetLoopLength = 0;

    // Granular pitch shifter state
    // Uses two overlapping grains for smooth pitch shifting
    static constexpr int GRAIN_SIZE = 2048;  // Grain size in samples
    float grainPhase = 0.0f;       // Phase within current grain cycle (0-1)
    float pitchReadPos1 = 0.0f;    // Continuous read position for grain 1
    float pitchReadPos2 = 0.0f;    // Continuous read position for grain 2
    bool wasPitchShifting = false; // Track when pitch shifting state changes

    // Fade/decay tracking
    float currentFadeMultiplier = 1.0f;  // Current fade level (starts at 1.0, decays each loop)
    float lastPlayheadPosition = 0.0f;   // For detecting loop boundary crossings

    // FFT Phase Vocoder for high-quality pitch shifting during playback
    StereoPhaseVocoder phaseVocoder;

    void initGrains()
    {
        // Reset granular pitch shifter state
        grainPhase = 0.0f;
        pitchReadPos1 = 0.0f;
        pitchReadPos2 = static_cast<float>(GRAIN_SIZE) / 2.0f;  // Offset by half grain
        wasPitchShifting = false;
    }

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

        // Get current pitch ratio and fade
        const float pitchRatio = pitchRatioSmoothed.getNextValue();
        const float fadeTarget = fadeSmoothed.getNextValue();

        // Track position for UI and detect loop boundary
        const float currentPos = getPlayheadPosition();
        const bool loopWrapped = detectLoopWrap(lastPlayheadPosition, currentPos);
        lastPlayheadPosition = currentPos;

        // Apply fade decay when loop wraps - ALWAYS applies to ALL playing layers
        // This is Blooper-style behavior: each layer fades independently based on fade knob
        // fadeActive is no longer needed for this - fade ALWAYS works on playing layers
        if (loopWrapped)
        {
            // Convert fade knob (0-1) to a per-loop decay multiplier
            // At 100% (1.0): no decay (infinite loops)
            // At 85% (0.85): gentle decay (~8-10 audible loops)
            // At 50% (0.5): moderate decay (~4-5 audible loops)
            // At 0% (0.0): fast decay (~2 audible loops)
            //
            // Formula: quadratic curve with moderate decay factor
            float invFade = 1.0f - fadeTarget;
            float decayStrength = invFade * invFade;  // Quadratic curve
            float decayMultiplier = 1.0f - (decayStrength * 0.25f);  // Max 25% reduction per loop at 0%

            // Calculate the "target" multiplier for the current fade setting
            // This is what the multiplier should be if we started fresh at this fade level
            // Higher fade = higher target (closer to 1.0)
            float targetMultiplier = fadeTarget;  // Simple: fade knob directly sets target level

            // If current multiplier is below target (user turned knob UP), recover towards target
            // If current multiplier is above target (user turned knob DOWN), decay towards target
            if (currentFadeMultiplier < targetMultiplier)
            {
                // Recovery: gradually restore volume when knob is turned up
                float recoveryRate = 0.15f;  // Recover ~15% of the gap per loop
                currentFadeMultiplier += (targetMultiplier - currentFadeMultiplier) * recoveryRate;
            }
            else
            {
                // Decay: apply normal fade decay
                currentFadeMultiplier *= decayMultiplier;
            }

            // Clamp to minimum to prevent complete silence
            currentFadeMultiplier = std::max(currentFadeMultiplier, 0.001f);
        }

        // ALWAYS apply fade multiplier - once audio has faded, it stays faded
        // The fadeActive flag only controls whether decay CONTINUES, not whether it's audible
        float fadeToApply = currentFadeMultiplier;

        // Calculate loop boundary crossfade to prevent pops
        const int effectiveStart = loopStart;
        const int effectiveEnd = loopEnd > 0 ? loopEnd : loopLength;
        const int effectiveLength = effectiveEnd - effectiveStart;

        float crossfadeGain = 1.0f;
        if (effectiveLength > CROSSFADE_SAMPLES * 2)
        {
            const float posInLoop = playHead - effectiveStart;
            const bool reversed = isReversed.load();

            if (!reversed)
            {
                // Near end of loop: fade out
                const float distToEnd = effectiveEnd - playHead;
                if (distToEnd < CROSSFADE_SAMPLES && distToEnd > 0)
                {
                    crossfadeGain = distToEnd / static_cast<float>(CROSSFADE_SAMPLES);
                }
                // Near start of loop: fade in
                else if (posInLoop < CROSSFADE_SAMPLES && posInLoop >= 0)
                {
                    crossfadeGain = posInLoop / static_cast<float>(CROSSFADE_SAMPLES);
                }
            }
            else
            {
                // Reversed: near start = fade out, near end = fade in
                if (posInLoop < CROSSFADE_SAMPLES && posInLoop >= 0)
                {
                    crossfadeGain = posInLoop / static_cast<float>(CROSSFADE_SAMPLES);
                }
                const float distToEnd = effectiveEnd - playHead;
                if (distToEnd < CROSSFADE_SAMPLES && distToEnd > 0)
                {
                    crossfadeGain = distToEnd / static_cast<float>(CROSSFADE_SAMPLES);
                }
            }
        }

        // Read from loop buffer at current playhead (with fade and crossfade applied)
        float rawL = readWithInterpolation(bufferL, playHead) * fadeToApply * crossfadeGain;
        float rawR = readWithInterpolation(bufferR, playHead) * fadeToApply * crossfadeGain;

        // Read wrapped position for crossfade blend (from the other end of the loop)
        if (crossfadeGain < 1.0f && effectiveLength > CROSSFADE_SAMPLES * 2)
        {
            float wrappedPos;
            const float posInLoop = playHead - effectiveStart;
            const bool reversed = isReversed.load();

            if (!reversed)
            {
                // If near end, blend with start; if near start, blend with end
                const float distToEnd = effectiveEnd - playHead;
                if (distToEnd < CROSSFADE_SAMPLES)
                {
                    // Near end: blend with start
                    wrappedPos = effectiveStart + (CROSSFADE_SAMPLES - distToEnd);
                }
                else
                {
                    // Near start: blend with end
                    wrappedPos = effectiveEnd - (CROSSFADE_SAMPLES - posInLoop);
                }
            }
            else
            {
                if (posInLoop < CROSSFADE_SAMPLES)
                {
                    wrappedPos = effectiveEnd - (CROSSFADE_SAMPLES - posInLoop);
                }
                else
                {
                    const float distToEnd = effectiveEnd - playHead;
                    wrappedPos = effectiveStart + (CROSSFADE_SAMPLES - distToEnd);
                }
            }

            float wrapL = readWithInterpolation(bufferL, wrappedPos) * fadeToApply * (1.0f - crossfadeGain);
            float wrapR = readWithInterpolation(bufferR, wrappedPos) * fadeToApply * (1.0f - crossfadeGain);
            rawL += wrapL;
            rawR += wrapR;
        }

        float loopL, loopR;

        // Check if pitch shifting is needed
        const bool isPitchShifting = std::abs(pitchRatio - 1.0f) >= 0.001f;

        if (isPitchShifting)
        {
            // Use FFT phase vocoder for high-quality pitch shifting
            // The vocoder changes pitch WITHOUT affecting playback speed
            phaseVocoder.setPitchRatio(pitchRatio);
            phaseVocoder.processSample(rawL, rawR, loopL, loopR);
        }
        else
        {
            // No pitch shift needed - pass through directly
            loopL = rawL;
            loopR = rawR;
        }

        // Mix loop playback with input passthrough for seamless monitoring
        outputL = loopL + inputL;
        outputR = loopR + inputR;

        // Advance playhead at NORMAL speed (not affected by pitch ratio)
        // This is the key difference from tape-style pitch shifting
        advancePlayhead(false);  // false = don't multiply by pitch ratio
    }

    void processOverdubbing(float inputL, float inputR, float& outputL, float& outputR)
    {
        if (loopLength <= 0)
        {
            outputL = inputL;
            outputR = inputR;
            return;
        }

        // Get fade amount for this sample
        const float fadeTarget = fadeSmoothed.getNextValue();

        // Detect loop boundary crossing for fade decay
        const float currentPos = getPlayheadPosition();
        const bool loopWrapped = detectLoopWrap(lastPlayheadPosition, currentPos);
        lastPlayheadPosition = currentPos;

        if (loopWrapped)
        {
            // Apply same fade decay formula as processPlaying for consistency
            // Quadratic curve with moderate decay factor
            float invFade = 1.0f - fadeTarget;
            float decayStrength = invFade * invFade;  // Quadratic curve
            float decayMultiplier = 1.0f - (decayStrength * 0.25f);  // Max 25% reduction per loop at 0%

            // Target multiplier based on fade knob position
            float targetMultiplier = fadeTarget;

            // Recovery or decay based on current vs target
            if (currentFadeMultiplier < targetMultiplier)
            {
                // Recovery: gradually restore volume when knob is turned up
                float recoveryRate = 0.15f;
                currentFadeMultiplier += (targetMultiplier - currentFadeMultiplier) * recoveryRate;
            }
            else
            {
                // Decay: apply normal fade decay
                currentFadeMultiplier *= decayMultiplier;
            }

            currentFadeMultiplier = std::max(currentFadeMultiplier, 0.001f);
        }

        // Read existing content (with fade applied)
        float existingL = readWithInterpolation(bufferL, playHead) * currentFadeMultiplier;
        float existingR = readWithInterpolation(bufferR, playHead) * currentFadeMultiplier;

        // Write position
        int writePos = static_cast<int>(playHead) % loopLength;

        // Apply fade to existing buffer content, then add new input
        // This way existing content decays while new content is added at full volume
        bufferL[writePos] = bufferL[writePos] * currentFadeMultiplier + inputL;
        bufferR[writePos] = bufferR[writePos] * currentFadeMultiplier + inputR;

        // Soft clip to prevent runaway (gentler curve)
        bufferL[writePos] = softClip(bufferL[writePos]);
        bufferR[writePos] = softClip(bufferR[writePos]);

        // Output: existing content (faded) + input for monitoring
        outputL = existingL + inputL;
        outputR = existingR + inputR;

        // Advance playhead WITHOUT pitch ratio (recording must happen at normal speed)
        advancePlayhead(false);
    }

    // Advance playhead - applyPitch should be false during recording/overdubbing
    void advancePlayhead(bool applyPitch = true)
    {
        const float rate = playbackRateSmoothed.getNextValue();
        const bool reversed = isReversed.load();
        const int effectiveStart = loopStart;
        const int effectiveEnd = loopEnd > 0 ? loopEnd : loopLength;
        const float effectiveLength = static_cast<float>(effectiveEnd - effectiveStart);

        if (effectiveLength <= 0)
            return;

        // Only apply pitch ratio during pure playback, not during recording/overdubbing
        // During recording, we need to write at normal speed
        float effectiveRate = rate;
        if (applyPitch)
        {
            const float pitchRatio = pitchRatioSmoothed.getNextValue();
            effectiveRate *= pitchRatio;
        }
        else
        {
            // Still need to consume the smoothed value to keep it advancing
            pitchRatioSmoothed.getNextValue();
        }

        if (reversed)
        {
            playHead -= effectiveRate;
            // Wrap around for proper looping at any speed
            while (playHead < effectiveStart)
            {
                playHead += effectiveLength;
            }
        }
        else
        {
            playHead += effectiveRate;
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

    // Simple pitch shift: just read at a different rate
    // For now, let's try the simplest possible approach
    // and see if we get ANY pitch change at all
    float readWithPitchShift(const std::vector<float>& buffer, float pitchRatio)
    {
        if (loopLength <= 0)
            return 0.0f;

        const int effectiveStart = loopStart;
        const int effectiveEnd = loopEnd > 0 ? loopEnd : loopLength;
        const float effectiveLength = static_cast<float>(effectiveEnd - effectiveStart);

        if (effectiveLength <= 0)
            return 0.0f;

        // Debug: log pitch ratio periodically
        static int debugCounter = 0;
        if (++debugCounter % 48000 == 0) {
            DBG("readWithPitchShift: pitchRatio=" + juce::String(pitchRatio, 4) +
                " pitchReadPos1=" + juce::String(pitchReadPos1, 1) +
                " playHead=" + juce::String(playHead, 1) +
                " grainPhase=" + juce::String(grainPhase, 3));
        }

        // Wrap helper for positions within loop bounds
        auto wrapPos = [effectiveStart, effectiveLength, effectiveEnd](float pos) {
            while (pos < effectiveStart) pos += effectiveLength;
            while (pos >= effectiveEnd) pos -= effectiveLength;
            return pos;
        };

        // Calculate Hann windows for each tap
        const float phase1 = grainPhase;
        const float phase2 = std::fmod(grainPhase + 0.5f, 1.0f);

        // Hann window: 0.5 * (1 - cos(2*pi*phase))
        const float window1 = 0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * phase1));
        const float window2 = 0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * phase2));

        // Read from both tap positions
        float pos1 = wrapPos(pitchReadPos1);
        float pos2 = wrapPos(pitchReadPos2);

        const float sample1 = readWithInterpolation(buffer, pos1);
        const float sample2 = readWithInterpolation(buffer, pos2);

        // Advance both read positions at pitchRatio speed
        pitchReadPos1 += pitchRatio;
        pitchReadPos2 += pitchRatio;

        // Keep read positions wrapped
        pitchReadPos1 = wrapPos(pitchReadPos1);
        pitchReadPos2 = wrapPos(pitchReadPos2);

        // Advance the phase counter
        float prevPhase = grainPhase;
        grainPhase += 1.0f / static_cast<float>(GRAIN_SIZE);

        // When tap 1's window completes, reset it to playhead
        if (grainPhase >= 1.0f)
        {
            grainPhase -= 1.0f;
            pitchReadPos1 = playHead;
        }

        // When tap 2's window completes
        float prevPhase2 = std::fmod(prevPhase + 0.5f, 1.0f);
        float newPhase2 = std::fmod(grainPhase + 0.5f, 1.0f);
        if (newPhase2 < prevPhase2)
        {
            pitchReadPos2 = playHead;
        }

        return sample1 * window1 + sample2 * window2;
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

    // Detect if the playhead wrapped around (crossed loop boundary)
    bool detectLoopWrap(float prevPos, float currentPos)
    {
        // Forward playback: detect when position jumps from high to low
        if (!isReversed.load())
        {
            // If previous was > 0.9 and current is < 0.1, loop wrapped forward
            return (prevPos > 0.9f && currentPos < 0.1f);
        }
        else
        {
            // Reverse playback: detect when position jumps from low to high
            return (prevPos < 0.1f && currentPos > 0.9f);
        }
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LoopBuffer)
};
