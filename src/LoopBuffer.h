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
    static constexpr int CROSSFADE_SAMPLES = 2048;  // Long crossfade for click-free looping (~42ms at 48kHz)

    enum class State
    {
        Idle = 0,
        Recording = 1,
        Playing = 2,
        Overdubbing = 3
    };

    // Layer type for Blooper-style ADD+ functionality
    // Override layers mute all regular layers below them
    enum class LayerType
    {
        Regular = 0,    // Normal layer (DUB, initial recording)
        Override = 1    // ADD+ layer - mutes all regular layers below when playing
    };

    LoopBuffer() = default;

    void prepare(double sampleRate, int samplesPerBlock)
    {
        juce::String msg = "LoopBuffer::prepare() sampleRate=" + juce::String(sampleRate);
        DBG(msg);
        // Note: prepare() logging to file removed to reduce spam (called 8x per layer)

        currentSampleRate = sampleRate;
        currentBlockSize = samplesPerBlock;
        maxLoopSamples = static_cast<int>(MAX_LOOP_SECONDS * sampleRate);

        // Pre-allocate buffers
        bufferL.resize(maxLoopSamples, 0.0f);
        bufferR.resize(maxLoopSamples, 0.0f);

        // Pre-allocate pitch processing buffers for block-based processing
        pitchInputL.resize(samplesPerBlock * 2, 0.0f);
        pitchInputR.resize(samplesPerBlock * 2, 0.0f);
        pitchOutputL.resize(samplesPerBlock * 2, 0.0f);
        pitchOutputR.resize(samplesPerBlock * 2, 0.0f);

        // Reset state
        clear();

        // Prepare smoothed values
        playbackRateSmoothed.reset(sampleRate, 0.02);  // 20ms smoothing (faster to reduce crackle)
        playbackRateSmoothed.setCurrentAndTargetValue(1.0f);

        pitchRatioSmoothed.reset(sampleRate, 0.015);  // 15ms smoothing for pitch (faster response)
        pitchRatioSmoothed.setCurrentAndTargetValue(1.0f);

        fadeSmoothed.reset(sampleRate, 0.1);  // 100ms smoothing for fade
        fadeSmoothed.setCurrentAndTargetValue(1.0f);  // Default to 100% (no fade)

        // Mute gain smoother for click-free muting (15ms fade)
        muteGainSmoothed.reset(sampleRate, 0.015);
        muteGainSmoothed.setCurrentAndTargetValue(isMuted.load() ? 0.0f : 1.0f);

        // Prepare block-based phase vocoder for efficient pitch shifting
        blockPitchShifter.prepare(sampleRate, samplesPerBlock);

        // Keep legacy sample-by-sample vocoder for compatibility
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
        isSoloed.store(false);
        fadeActive.store(false);
        playbackRateSmoothed.setCurrentAndTargetValue(1.0f);
        pitchRatioSmoothed.setCurrentAndTargetValue(1.0f);
        fadeSmoothed.setCurrentAndTargetValue(1.0f);
        muteGainSmoothed.setCurrentAndTargetValue(1.0f);  // Unmuted by default
        currentFadeMultiplier.store(1.0f);
        lastPlayheadPosition = 0.0f;
        skipFirstBlock = false;

        // Reset pitch shifters
        blockPitchShifter.reset();
        phaseVocoder.reset();
        initGrains();

        // Reset layer type to Regular
        layerType = LayerType::Regular;

        // Invalidate waveform cache
        waveformCacheDirty = true;
        cachedWaveform.clear();
        cachedPeakLevel = 0.0f;
        lastCachedWriteHead = 0;

        // Reset anti-aliasing filter state
        antiAliasLpfL = 0.0f;
        antiAliasLpfR = 0.0f;
        prevOutputL = 0.0f;
        prevOutputR = 0.0f;
        prevInputL = 0.0f;
        prevInputR = 0.0f;

        // Reset EQ state (keeps gain settings, just clears filter history)
        resetEQState();
    }

    // Copy content from another LoopBuffer (for layer shuffling)
    void copyFrom(const LoopBuffer& other)
    {
        // Copy audio data
        loopLength = other.loopLength;
        if (loopLength > 0 && loopLength <= maxLoopSamples)
        {
            std::copy(other.bufferL.begin(), other.bufferL.begin() + loopLength, bufferL.begin());
            std::copy(other.bufferR.begin(), other.bufferR.begin() + loopLength, bufferR.begin());
        }

        // Copy state
        writeHead = other.writeHead;
        playHead = other.playHead;
        loopStart = other.loopStart;
        loopEnd = other.loopEnd;
        targetLoopLength = other.targetLoopLength;
        state.store(other.state.load());
        isReversed.store(other.isReversed.load());
        isMuted.store(other.isMuted.load());
        // Sync mute gain smoother with muted state
        muteGainSmoothed.setCurrentAndTargetValue(other.isMuted.load() ? 0.0f : 1.0f);
        fadeActive.store(other.fadeActive.load());
        currentFadeMultiplier.store(other.currentFadeMultiplier.load());
        lastPlayheadPosition = other.lastPlayheadPosition;

        // Reset pitch shifters (they have internal state that shouldn't be copied)
        blockPitchShifter.reset();
        phaseVocoder.reset();
        initGrains();

        // Invalidate waveform cache since content changed
        waveformCacheDirty = true;

        DBG("LoopBuffer::copyFrom() - Copied " + juce::String(loopLength) + " samples");
    }

    // Add this layer's buffer content to an external buffer (for flattening)
    void addToBuffer(juce::AudioBuffer<float>& destBuffer) const
    {
        if (loopLength <= 0)
            return;

        const int numSamples = std::min(loopLength, destBuffer.getNumSamples());
        const int numChannels = destBuffer.getNumChannels();

        // Apply current fade multiplier when adding
        const float fadeMultiplier = currentFadeMultiplier.load();

        if (numChannels >= 1)
        {
            float* destL = destBuffer.getWritePointer(0);
            for (int i = 0; i < numSamples; ++i)
            {
                destL[i] += bufferL[i] * fadeMultiplier;
            }
        }

        if (numChannels >= 2)
        {
            float* destR = destBuffer.getWritePointer(1);
            for (int i = 0; i < numSamples; ++i)
            {
                destR[i] += bufferR[i] * fadeMultiplier;
            }
        }
    }

    // Set this layer's buffer from an external buffer (for flattening)
    void setFromBuffer(const juce::AudioBuffer<float>& srcBuffer, int length)
    {
        loopLength = std::min(length, maxLoopSamples);
        if (loopLength <= 0)
            return;

        const int numChannels = srcBuffer.getNumChannels();

        if (numChannels >= 1)
        {
            const float* srcL = srcBuffer.getReadPointer(0);
            std::copy(srcL, srcL + loopLength, bufferL.begin());
        }

        if (numChannels >= 2)
        {
            const float* srcR = srcBuffer.getReadPointer(1);
            std::copy(srcR, srcR + loopLength, bufferR.begin());
        }
        else if (numChannels >= 1)
        {
            // Mono source - copy to both channels
            const float* srcL = srcBuffer.getReadPointer(0);
            std::copy(srcL, srcL + loopLength, bufferR.begin());
        }

        // Set state to playing
        writeHead = loopLength;
        playHead = 0.0f;
        loopStart = 0;
        loopEnd = loopLength;
        targetLoopLength = loopLength;
        state.store(State::Playing);
        currentFadeMultiplier.store(1.0f);

        DBG("LoopBuffer::setFromBuffer() - Set " + juce::String(loopLength) + " samples");
    }

    // Set buffer content while preserving playhead and state (for seamless flatten)
    // This replaces the audio data without interrupting playback
    void setFromBufferSeamless(const juce::AudioBuffer<float>& srcBuffer, int length,
                                float preservedPlayhead, State preservedState)
    {
        loopLength = std::min(length, maxLoopSamples);
        if (loopLength <= 0)
            return;

        const int numChannels = srcBuffer.getNumChannels();

        if (numChannels >= 1)
        {
            const float* srcL = srcBuffer.getReadPointer(0);
            std::copy(srcL, srcL + loopLength, bufferL.begin());
        }

        if (numChannels >= 2)
        {
            const float* srcR = srcBuffer.getReadPointer(1);
            std::copy(srcR, srcR + loopLength, bufferR.begin());
        }
        else if (numChannels >= 1)
        {
            // Mono source - copy to both channels
            const float* srcL = srcBuffer.getReadPointer(0);
            std::copy(srcL, srcL + loopLength, bufferR.begin());
        }

        // Preserve playback state for seamless transition
        writeHead = loopLength;
        playHead = preservedPlayhead;  // Keep current position
        loopStart = 0;
        loopEnd = loopLength;
        targetLoopLength = loopLength;
        state.store(preservedState);  // Keep current state (Playing/Overdubbing/etc)
        currentFadeMultiplier.store(1.0f);  // Reset fade since layers are merged
        lastPlayheadPosition = playHead / static_cast<float>(loopLength);  // Sync for fade detection

        DBG("LoopBuffer::setFromBufferSeamless() - Set " + juce::String(loopLength) +
            " samples, preserved playhead at " + juce::String(preservedPlayhead));
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
            // Pass true for initial recording (needs fade-in at start)
            applyCrossfade(true);

            // Mark waveform cache as dirty to regenerate with final content
            waveformCacheDirty = true;
        }
    }

    void startOverdub()
    {
        if (state.load() == State::Playing || state.load() == State::Idle)
        {
            if (loopLength > 0)
            {
                // Reset fade multiplier when starting overdub so new content is at full volume
                currentFadeMultiplier.store(1.0f);
                // Initialize overdub fade-in (Blooper-style: fade at recording start)
                overdubFadeInCounter = 0;
                isOverdubFadingOut = false;
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
        currentFadeMultiplier.store(1.0f);
        lastPlayheadPosition = 0.0f;

        // Initialize overdub fade-in (Blooper-style: fade at recording start)
        overdubFadeInCounter = 0;
        isOverdubFadingOut = false;

        // Skip the first block to avoid capturing residual audio from the
        // state transition (e.g., when transitioning from Recording to Playing+Overdub)
        skipFirstBlock = true;

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
            // Start fade-out instead of immediately stopping
            // The fade-out will be processed in processOverdubbing
            isOverdubFadingOut = true;
            overdubFadeOutCounter = OVERDUB_FADE_SAMPLES;
            // Note: state stays Overdubbing until fade-out completes
            DBG("stopOverdub() - starting fade-out");
        }
    }

    // Immediate stop without fade-out (used when transitioning to a new layer)
    void stopOverdubImmediate()
    {
        if (state.load() == State::Overdubbing)
        {
            isOverdubFadingOut = false;
            overdubFadeOutCounter = 0;
            state.store(State::Playing);
            waveformCacheDirty = true;  // Regenerate waveform with new overdub content
            DBG("stopOverdubImmediate() - immediate switch to Playing");
        }
    }

    void play()
    {
        if (loopLength > 0)
        {
            // Reset fade multiplier when starting playback
            currentFadeMultiplier.store(1.0f);
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
        // Reset pitch shifters to prevent latent audio from continuing
        blockPitchShifter.reset();
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
        return currentFadeMultiplier.load();
    }

    // Get the peak level of the raw buffer (without fade applied)
    // Used for waveform visualization to establish a consistent baseline
    float getBufferPeakLevel() const
    {
        if (loopLength <= 0)
            return 0.0f;

        // Use cached peak level if available, update cache if dirty
        updateWaveformCacheIfNeeded();
        return cachedPeakLevel;
    }

    // Process audio
    void processBlock(juce::AudioBuffer<float>& buffer)
    {
        const int numSamples = buffer.getNumSamples();
        float* leftChannel = buffer.getWritePointer(0);
        float* rightChannel = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;

        State currentState = state.load();

        // For Playing state with pitch shifting, use optimized block processing
        if (currentState == State::Playing && loopLength > 0)
        {
            processPlayingBlock(buffer);
            return;
        }

        // Skip first block after starting overdub to avoid residual audio from state transitions
        // (e.g., when transitioning from Recording to Playing+Overdub on a new layer)
        if (currentState == State::Overdubbing && skipFirstBlock)
        {
            skipFirstBlock = false;
            // Output silence for this block - playhead will advance on next block
            buffer.clear();
            return;
        }

        // For other states, use sample-by-sample processing
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
                    // Fallback for edge cases (loopLength <= 0)
                    outputL = inputL;
                    outputR = inputR;
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

    // Block-optimized playing - processes entire buffer at once for efficiency
    void processPlayingBlock(juce::AudioBuffer<float>& buffer)
    {
        const int numSamples = buffer.getNumSamples();
        float* leftChannel = buffer.getWritePointer(0);
        float* rightChannel = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;

        // Ensure pitch buffers are large enough
        if (static_cast<int>(pitchInputL.size()) < numSamples)
        {
            pitchInputL.resize(numSamples, 0.0f);
            pitchInputR.resize(numSamples, 0.0f);
            pitchOutputL.resize(numSamples, 0.0f);
            pitchOutputR.resize(numSamples, 0.0f);
        }

        // Get pitch ratio for this block (read once, not per-sample)
        float pitchRatio = pitchRatioSmoothed.getTargetValue();
        const float pitchDistance = std::abs(pitchRatio - 1.0f);
        const bool isPitchShifting = pitchDistance >= 0.002f;

        // Fade handling (simplified for block processing)
        const float fadeTarget = fadeSmoothed.getTargetValue();
        float fadeToApply = currentFadeMultiplier.load();

        // Calculate effective loop bounds
        const int effectiveStart = loopStart;
        const int effectiveEnd = loopEnd > 0 ? loopEnd : loopLength;
        const int effectiveLength = effectiveEnd - effectiveStart;

        if (effectiveLength <= 0)
        {
            // No valid loop - pass through input
            return;
        }

        // Phase 1: Read raw loop audio for entire block with real-time crossfade at boundaries
        for (int i = 0; i < numSamples; ++i)
        {
            // Advance smoothed values to keep them in sync
            pitchRatioSmoothed.getNextValue();
            fadeSmoothed.getNextValue();

            // Detect loop wrap for fade
            const float currentPos = getPlayheadPosition();
            const bool loopWrapped = detectLoopWrap(lastPlayheadPosition, currentPos);
            lastPlayheadPosition = currentPos;

            // Handle fade decay on loop wrap
            if (fadeTarget >= 0.99f)
            {
                currentFadeMultiplier.store(1.0f);
                fadeToApply = 1.0f;
            }
            else if (loopWrapped)
            {
                float invFade = 1.0f - fadeTarget;
                float decayStrength = invFade * invFade;
                float decayMultiplier = 1.0f - (decayStrength * 0.25f);
                float targetMultiplier = fadeTarget;
                float fadeMult = currentFadeMultiplier.load();

                if (fadeMult < targetMultiplier)
                {
                    float recoveryRate = 0.15f;
                    fadeMult += (targetMultiplier - fadeMult) * recoveryRate;
                }
                else
                {
                    fadeMult *= decayMultiplier;
                }

                currentFadeMultiplier.store(std::max(fadeMult, 0.001f));
                fadeToApply = fadeMult;
            }

            // Read from buffer with real-time crossfade at loop boundary
            float rawL, rawR;
            readWithCrossfade(rawL, rawR, playHead, effectiveStart, effectiveEnd);
            rawL *= fadeToApply;
            rawR *= fadeToApply;

            // Apply anti-aliasing low-pass filter when playing at high speeds
            // This prevents aliasing artifacts that cause crackling
            const float currentRate = playbackRateSmoothed.getCurrentValue();
            if (currentRate > 1.01f)
            {
                // One-pole low-pass filter with cutoff tracking the speed
                // Higher speed = lower cutoff to prevent aliasing
                // Coefficient: 0.5 at 2x speed, 0.25 at 4x speed, etc.
                const float lpfCoeff = std::min(0.9f, 1.0f / currentRate);
                antiAliasLpfL = lpfCoeff * rawL + (1.0f - lpfCoeff) * antiAliasLpfL;
                antiAliasLpfR = lpfCoeff * rawR + (1.0f - lpfCoeff) * antiAliasLpfR;
                rawL = antiAliasLpfL;
                rawR = antiAliasLpfR;
            }

            pitchInputL[i] = rawL;
            pitchInputR[i] = rawR;

            // Advance playhead
            advancePlayhead(false);
        }

        // Phase 2: Apply pitch shifting to entire block at once (THE KEY OPTIMIZATION)
        if (isPitchShifting)
        {
            blockPitchShifter.setPitchRatio(pitchRatio);
            blockPitchShifter.processBlock(pitchInputL.data(), pitchInputR.data(),
                                           pitchOutputL.data(), pitchOutputR.data(), numSamples);

            // Crossfade near unity for smooth transition
            if (pitchDistance < 0.01f)
            {
                float crossfade = pitchDistance / 0.01f;
                for (int i = 0; i < numSamples; ++i)
                {
                    pitchOutputL[i] = pitchInputL[i] * (1.0f - crossfade) + pitchOutputL[i] * crossfade;
                    pitchOutputR[i] = pitchInputR[i] * (1.0f - crossfade) + pitchOutputR[i] * crossfade;
                }
            }

            wasPitchShifting = true;
        }
        else
        {
            // No pitch shift - bypass
            if (wasPitchShifting)
            {
                blockPitchShifter.reset();
                wasPitchShifting = false;
            }
            std::copy(pitchInputL.begin(), pitchInputL.begin() + numSamples, pitchOutputL.begin());
            std::copy(pitchInputR.begin(), pitchInputR.begin() + numSamples, pitchOutputR.begin());
        }

        // Phase 2.5: Apply per-layer 3-band EQ (if active)
        if (isEQActive())
        {
            for (int i = 0; i < numSamples; ++i)
            {
                processEQ(pitchOutputL[i], pitchOutputR[i]);
            }
        }

        // Phase 3: Apply volume and pan, then mix with input and write to output buffer
        const float vol = volume.load();
        const float panVal = pan.load();
        // Pan law: constant power panning
        // panVal: -1.0 = full left, 0.0 = center, 1.0 = full right
        const float panAngle = (panVal + 1.0f) * 0.5f * juce::MathConstants<float>::halfPi;
        const float panL = std::cos(panAngle);
        const float panR = std::sin(panAngle);

        for (int i = 0; i < numSamples; ++i)
        {
            float outL = pitchOutputL[i] * vol * panL;
            float outR = pitchOutputR[i] * vol * panR;

            // DC blocker to remove any DC offset that accumulates during speed changes
            // Uses a simple high-pass filter: y[n] = x[n] - x[n-1] + 0.995 * y[n-1]
            const float dcBlockCoeff = 0.995f;
            float filteredL = outL - prevInputL + dcBlockCoeff * prevOutputL;
            float filteredR = outR - prevInputR + dcBlockCoeff * prevOutputR;
            prevInputL = outL;
            prevInputR = outR;
            prevOutputL = filteredL;
            prevOutputR = filteredR;

            leftChannel[i] = filteredL + leftChannel[i];
            if (rightChannel)
                rightChannel[i] = filteredR + rightChannel[i];
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

    // Get loop boundaries as normalized positions (0-1)
    float getLoopStartNormalized() const
    {
        if (loopLength <= 0) return 0.0f;
        return static_cast<float>(loopStart) / static_cast<float>(loopLength);
    }

    float getLoopEndNormalized() const
    {
        if (loopLength <= 0) return 1.0f;
        int end = (loopEnd > 0) ? loopEnd : loopLength;
        return static_cast<float>(end) / static_cast<float>(loopLength);
    }

    // Get raw playhead position for syncing
    float getRawPlayhead() const { return playHead; }

    // Set playhead position (for syncing with other layers)
    void setPlayhead(float position) { playHead = position; }

    // Peek at playback content without advancing state (for Layer mode bounce)
    // Reads what this layer would output at current playhead position
    void peekPlayback(juce::AudioBuffer<float>& buffer) const
    {
        if (loopLength <= 0 || state.load() != State::Playing)
        {
            buffer.clear();
            return;
        }

        const int numSamples = buffer.getNumSamples();
        float* leftChannel = buffer.getWritePointer(0);
        float* rightChannel = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;

        const int effectiveStart = loopStart;
        const int effectiveEnd = loopEnd > 0 ? loopEnd : loopLength;
        const int effectiveLength = effectiveEnd - effectiveStart;

        if (effectiveLength <= 0)
        {
            buffer.clear();
            return;
        }

        // Get playback rate and calculate sample positions
        const float rate = playbackRateSmoothed.getTargetValue();
        const bool reversed = isReversed.load();
        const float fadeMultiplier = currentFadeMultiplier.load();
        const float vol = volume.load();
        const float panVal = pan.load();
        const float panAngle = (panVal + 1.0f) * 0.5f * juce::MathConstants<float>::halfPi;
        const float panL = std::cos(panAngle);
        const float panR = std::sin(panAngle);

        float peekHead = playHead;

        for (int i = 0; i < numSamples; ++i)
        {
            // Read with crossfade at boundaries
            float rawL, rawR;
            readWithCrossfade(rawL, rawR, peekHead, effectiveStart, effectiveEnd);

            // Apply fade and volume
            float outL = rawL * fadeMultiplier * vol * panL;
            float outR = rawR * fadeMultiplier * vol * panR;

            leftChannel[i] = outL;
            if (rightChannel)
                rightChannel[i] = outR;

            // Advance peek head (simulating playback without modifying actual state)
            if (reversed)
            {
                peekHead -= rate;
                while (peekHead < effectiveStart)
                    peekHead += effectiveLength;
            }
            else
            {
                peekHead += rate;
                while (peekHead >= effectiveEnd)
                    peekHead -= effectiveLength;
            }
        }
    }

    bool hasContent() const { return loopLength > 0; }
    bool getIsReversed() const { return isReversed.load(); }
    bool getReversed() const { return isReversed.load(); }
    float getPlaybackRate() const { return playbackRateSmoothed.getTargetValue(); }

    // Mute control with click-free smoothing
    void setMuted(bool muted)
    {
        isMuted.store(muted);
        muteGainSmoothed.setTargetValue(muted ? 0.0f : 1.0f);
    }
    bool getMuted() const { return isMuted.load(); }

    // Get current smoothed mute gain (call per-sample or per-block)
    float getMuteGain() { return muteGainSmoothed.getNextValue(); }

    // Check if mute is transitioning (for optimization - can skip processing if fully muted and stable)
    bool isMuteTransitioning() const { return muteGainSmoothed.isSmoothing(); }

    // Solo control (soloed layers play exclusively when any layer is soloed)
    void setSoloed(bool soloed) { isSoloed.store(soloed); }
    bool getSoloed() const { return isSoloed.load(); }

    // Layer type control (Regular vs Override/ADD+)
    void setLayerType(LayerType type) { layerType = type; }
    LayerType getLayerType() const { return layerType; }
    bool isOverrideLayer() const { return layerType == LayerType::Override; }

    // Per-layer volume (0.0 to ~1.41 for +3dB boost)
    void setVolume(float vol) { volume.store(std::clamp(vol, 0.0f, 2.0f)); }
    float getVolume() const { return volume.load(); }

    // Per-layer pan (-1.0 = full left, 0.0 = center, 1.0 = full right)
    void setPan(float p) { pan.store(std::clamp(p, -1.0f, 1.0f)); }
    float getPan() const { return pan.load(); }

    // ============================================
    // PER-LAYER 3-BAND EQ
    // Low shelf (200Hz), Mid peak (1kHz), High shelf (4kHz)
    // Gain range: -12dB to +12dB (0.25 to 4.0 linear)
    // ============================================

    void setEQLow(float gainDB)
    {
        float clampedDB = std::clamp(gainDB, -12.0f, 12.0f);
        float linear = std::pow(10.0f, clampedDB / 20.0f);
        eqLowGain.store(linear);
        eqCoeffsDirty = true;
    }

    void setEQMid(float gainDB)
    {
        float clampedDB = std::clamp(gainDB, -12.0f, 12.0f);
        float linear = std::pow(10.0f, clampedDB / 20.0f);
        eqMidGain.store(linear);
        eqCoeffsDirty = true;
    }

    void setEQHigh(float gainDB)
    {
        float clampedDB = std::clamp(gainDB, -12.0f, 12.0f);
        float linear = std::pow(10.0f, clampedDB / 20.0f);
        eqHighGain.store(linear);
        eqCoeffsDirty = true;
    }

    // Get EQ gains in dB for UI display
    float getEQLowDB() const { return 20.0f * std::log10(std::max(eqLowGain.load(), 0.001f)); }
    float getEQMidDB() const { return 20.0f * std::log10(std::max(eqMidGain.load(), 0.001f)); }
    float getEQHighDB() const { return 20.0f * std::log10(std::max(eqHighGain.load(), 0.001f)); }

    // Check if EQ is active (any band not at unity)
    bool isEQActive() const
    {
        const float low = eqLowGain.load();
        const float mid = eqMidGain.load();
        const float high = eqHighGain.load();
        return std::abs(low - 1.0f) > 0.01f ||
               std::abs(mid - 1.0f) > 0.01f ||
               std::abs(high - 1.0f) > 0.01f;
    }

    // Apply soft clipping to entire buffer (for additive recording runaway prevention)
    void applyBufferSoftClip()
    {
        if (loopLength <= 0)
            return;

        for (int i = 0; i < loopLength; ++i)
        {
            bufferL[i] = softClip(bufferL[i]);
            bufferR[i] = softClip(bufferR[i]);
        }
        DBG("applyBufferSoftClip() - Applied soft clipping to " + juce::String(loopLength) + " samples");
    }

    // ============================================
    // ADDITIVE RECORDING (Blooper-style punch-in/out)
    // Records effected audio directly into this layer in realtime
    // ============================================

    // Prepare this layer for additive recording
    // Sets up the buffer with the target loop length and syncs to current playhead
    void prepareForAdditiveRecording(int targetLoopLength, int startPlayheadSamples)
    {
        // Safety checks
        if (targetLoopLength <= 0 || targetLoopLength > maxLoopSamples)
        {
            DBG("prepareForAdditiveRecording() - INVALID targetLoopLength=" + juce::String(targetLoopLength));
            return;
        }

        // Clear any existing content
        clear();

        // Set up the loop length to match master
        loopLength = targetLoopLength;

        // Clamp startPlayheadSamples to valid range
        if (startPlayheadSamples < 0) startPlayheadSamples = 0;
        if (startPlayheadSamples >= loopLength) startPlayheadSamples = startPlayheadSamples % loopLength;

        // Initialize buffer (should already be allocated in prepare())
        // Use safe bounds
        int fillEnd = std::min(loopLength, static_cast<int>(bufferL.size()));
        std::fill(bufferL.begin(), bufferL.begin() + fillEnd, 0.0f);
        std::fill(bufferR.begin(), bufferR.begin() + fillEnd, 0.0f);

        // Set playhead to match the current position
        playHead = static_cast<float>(startPlayheadSamples);
        writeHead = startPlayheadSamples;

        // Mark as playing (so it contributes to output and advances playhead)
        state = State::Playing;
        hasContentFlag = false;  // Will be set true when we write content

        // Track that we're in additive recording mode
        additiveRecordingMode = true;
        additiveWriteHead = startPlayheadSamples;

        DBG("prepareForAdditiveRecording() - loopLength=" + juce::String(loopLength) +
            " startPlayhead=" + juce::String(startPlayheadSamples));
    }

    // Write effected audio into this layer (called from PluginProcessor)
    // Writes at the current additive write head position
    void writeAdditiveAudio(const juce::AudioBuffer<float>& buffer, int numSamples)
    {
        if (!additiveRecordingMode || loopLength <= 0)
            return;

        // Safety: ensure we have valid buffer size
        const int bufferSize = static_cast<int>(bufferL.size());
        if (bufferSize <= 0 || loopLength > bufferSize)
            return;

        const int numChannels = buffer.getNumChannels();
        const float* leftIn = numChannels > 0 ? buffer.getReadPointer(0) : nullptr;
        const float* rightIn = numChannels > 1 ? buffer.getReadPointer(1) : nullptr;

        for (int i = 0; i < numSamples; ++i)
        {
            // Ensure write head is in bounds
            if (additiveWriteHead < 0 || additiveWriteHead >= loopLength)
            {
                additiveWriteHead = additiveWriteHead % loopLength;
                if (additiveWriteHead < 0) additiveWriteHead = 0;
            }

            // Soft clip the input to prevent runaway accumulation
            float sampleL = leftIn ? softClip(leftIn[i]) : 0.0f;
            float sampleR = rightIn ? softClip(rightIn[i]) : sampleL;

            // Write to buffer at current position
            bufferL[additiveWriteHead] = sampleL;
            bufferR[additiveWriteHead] = sampleR;

            // Track if we've written any significant content
            if (std::abs(sampleL) > 0.001f || std::abs(sampleR) > 0.001f)
            {
                hasContentFlag = true;
            }

            // Advance write head with wrap
            additiveWriteHead++;
            if (additiveWriteHead >= loopLength)
            {
                additiveWriteHead = 0;
            }
        }

        // Sync playhead with write head during additive recording
        playHead = static_cast<float>(additiveWriteHead);
    }

    // Stop additive recording mode
    void stopAdditiveRecording()
    {
        if (!additiveRecordingMode)
            return;

        additiveRecordingMode = false;

        // Keep state as Playing if we have content
        if (hasContentFlag)
        {
            state = State::Playing;
            DBG("stopAdditiveRecording() - Layer has content, continuing playback");
        }
        else
        {
            state = State::Idle;
            DBG("stopAdditiveRecording() - No content recorded");
        }

        // Invalidate waveform cache to show new content
        waveformCacheDirty = true;
    }

    // Check if currently in additive recording mode
    bool isInAdditiveRecordingMode() const
    {
        return additiveRecordingMode;
    }

    // Get waveform data for UI visualization (downsampled)
    // Works during recording (uses writeHead) and playback (uses loopLength)
    // ALWAYS applies current fade multiplier so waveform visually reflects faded audio
    std::vector<float> getWaveformData(int numPoints) const
    {
        // During playback, use cached waveform for efficiency
        // During recording/overdubbing, update cache more frequently
        updateWaveformCacheIfNeeded();

        // Apply current fade multiplier to cached waveform
        const float fadeMultiplier = currentFadeMultiplier.load();
        std::vector<float> result(numPoints, 0.0f);

        // Copy cached waveform with fade applied
        size_t copyLen = std::min(static_cast<size_t>(numPoints), cachedWaveform.size());
        for (size_t i = 0; i < copyLen; ++i)
        {
            result[i] = std::min(cachedWaveform[i] * fadeMultiplier, 1.0f);
        }

        return result;
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
    int currentBlockSize = 512;

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
    juce::SmoothedValue<float> muteGainSmoothed;  // 0.0 = muted, 1.0 = unmuted (for click-free muting)
    std::atomic<bool> isReversed { false };
    std::atomic<bool> isMuted { false };
    std::atomic<bool> isSoloed { false };
    std::atomic<float> volume { 1.0f };      // Per-layer volume (0.0 to 1.0)
    std::atomic<float> pan { 0.0f };         // Per-layer pan (-1.0 to 1.0)
    std::atomic<bool> fadeActive { false };  // True when fade should apply during playback (any layer is recording)

    // Per-layer 3-band EQ parameters (linear gain, 1.0 = unity)
    std::atomic<float> eqLowGain { 1.0f };   // Low shelf at 200Hz
    std::atomic<float> eqMidGain { 1.0f };   // Mid peak at 1kHz
    std::atomic<float> eqHighGain { 1.0f };  // High shelf at 4kHz
    bool eqCoeffsDirty = true;               // Flag to recalculate coefficients

    // Biquad filter coefficients for 3-band EQ
    // Low shelf (200Hz)
    float eqLowB0 = 1.0f, eqLowB1 = 0.0f, eqLowB2 = 0.0f;
    float eqLowA1 = 0.0f, eqLowA2 = 0.0f;
    // Mid peak (1kHz)
    float eqMidB0 = 1.0f, eqMidB1 = 0.0f, eqMidB2 = 0.0f;
    float eqMidA1 = 0.0f, eqMidA2 = 0.0f;
    // High shelf (4kHz)
    float eqHighB0 = 1.0f, eqHighB1 = 0.0f, eqHighB2 = 0.0f;
    float eqHighA1 = 0.0f, eqHighA2 = 0.0f;

    // Biquad filter state (stereo, 3 bands)
    // Low shelf state
    float eqLowX1L = 0.0f, eqLowX2L = 0.0f, eqLowY1L = 0.0f, eqLowY2L = 0.0f;
    float eqLowX1R = 0.0f, eqLowX2R = 0.0f, eqLowY1R = 0.0f, eqLowY2R = 0.0f;
    // Mid peak state
    float eqMidX1L = 0.0f, eqMidX2L = 0.0f, eqMidY1L = 0.0f, eqMidY2L = 0.0f;
    float eqMidX1R = 0.0f, eqMidX2R = 0.0f, eqMidY1R = 0.0f, eqMidY2R = 0.0f;
    // High shelf state
    float eqHighX1L = 0.0f, eqHighX2L = 0.0f, eqHighY1L = 0.0f, eqHighY2L = 0.0f;
    float eqHighX1R = 0.0f, eqHighX2R = 0.0f, eqHighY1R = 0.0f, eqHighY2R = 0.0f;
    std::atomic<State> state { State::Idle };
    LayerType layerType { LayerType::Regular };  // Layer type (Regular or Override/ADD+)

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
    std::atomic<float> currentFadeMultiplier { 1.0f };  // Current fade level (starts at 1.0, decays each loop)
    float lastPlayheadPosition = 0.0f;   // For detecting loop boundary crossings

    // Overdub fade tracking (Blooper-style: fade at recording start/stop points)
    static constexpr int OVERDUB_FADE_SAMPLES = 512;  // ~10ms at 48kHz
    int overdubFadeInCounter = 0;   // Counts up from 0 to OVERDUB_FADE_SAMPLES when overdub starts
    int overdubFadeOutCounter = 0;  // Counts down from OVERDUB_FADE_SAMPLES to 0 when overdub ends
    bool isOverdubFadingOut = false;  // True during fade-out after overdub stops
    bool skipFirstBlock = false;      // Skip first audio block to avoid residual audio from state transitions

    // Additive recording mode (Blooper-style punch-in/out)
    bool additiveRecordingMode = false;  // True when receiving effected audio from ADD mode
    int additiveWriteHead = 0;           // Write position for additive recording
    bool hasContentFlag = false;         // Track if any audio was actually written

    // Block-based pitch shifter (efficient, processes entire blocks)
    StereoBlockPitchShifter blockPitchShifter;

    // Pre-allocated buffers for block-based pitch processing
    std::vector<float> pitchInputL;
    std::vector<float> pitchInputR;
    std::vector<float> pitchOutputL;
    std::vector<float> pitchOutputR;

    // Waveform cache for efficient UI updates
    // Only regenerated when buffer content changes (during recording/overdubbing)
    static constexpr int WAVEFORM_CACHE_POINTS = 100;
    mutable std::vector<float> cachedWaveform;
    mutable float cachedPeakLevel = 0.0f;
    mutable bool waveformCacheDirty = true;
    mutable int lastCachedWriteHead = 0;  // Track writeHead changes during recording

    // Legacy sample-by-sample phase vocoder (kept for compatibility)
    StereoPhaseVocoder phaseVocoder;

    // Anti-aliasing low-pass filter state (one-pole)
    // Used when playback rate > 1.0 to prevent aliasing
    float antiAliasLpfL = 0.0f;
    float antiAliasLpfR = 0.0f;
    float prevOutputL = 0.0f;  // For DC blocker
    float prevOutputR = 0.0f;
    float prevInputL = 0.0f;
    float prevInputR = 0.0f;

    // Update waveform cache if needed (called from getWaveformData and getBufferPeakLevel)
    void updateWaveformCacheIfNeeded() const
    {
        bool needsUpdate = false;
        State currentState = state.load();

        // During recording/overdubbing, ALWAYS regenerate for live animated waveform
        // This is polled at ~50ms intervals from the UI, giving smooth animation
        // The waveform is only 100 points, so this is efficient enough
        if (currentState == State::Recording || currentState == State::Overdubbing)
        {
            // Always update during recording - this is the "beautiful animated rendering"
            needsUpdate = true;
            lastCachedWriteHead = writeHead;
        }
        else if (waveformCacheDirty || cachedWaveform.empty())
        {
            // During playback, only update if cache is dirty or empty
            // Cache gets dirty at loop boundaries when fade multiplier changes
            needsUpdate = true;
        }

        if (!needsUpdate)
            return;

        // Determine visual length
        int visualLength = loopLength;
        if (currentState == State::Recording)
        {
            visualLength = (targetLoopLength > 0) ? targetLoopLength : writeHead;
        }

        if (visualLength <= 0)
        {
            cachedWaveform.clear();
            cachedWaveform.resize(WAVEFORM_CACHE_POINTS, 0.0f);
            cachedPeakLevel = 0.0f;
            waveformCacheDirty = false;
            return;
        }

        // Regenerate waveform cache (without fade - fade applied at read time)
        cachedWaveform.clear();
        cachedWaveform.resize(WAVEFORM_CACHE_POINTS, 0.0f);

        const int samplesPerPoint = visualLength / WAVEFORM_CACHE_POINTS;
        if (samplesPerPoint <= 0)
        {
            waveformCacheDirty = false;
            return;
        }

        const int maxSampleToShow = (currentState == State::Recording) ? writeHead : visualLength;
        float peakLevel = 0.0f;

        for (int i = 0; i < WAVEFORM_CACHE_POINTS; ++i)
        {
            float maxVal = 0.0f;
            const int startSample = i * samplesPerPoint;
            const int endSample = std::min(startSample + samplesPerPoint, maxSampleToShow);

            if (startSample < maxSampleToShow)
            {
                for (int j = startSample; j < endSample; ++j)
                {
                    float val = std::abs(bufferL[j]) + std::abs(bufferR[j]);
                    maxVal = std::max(maxVal, val * 0.5f);
                }
            }
            cachedWaveform[i] = maxVal;
            peakLevel = std::max(peakLevel, maxVal);
        }

        cachedPeakLevel = peakLevel;
        waveformCacheDirty = false;
    }

    void initGrains()
    {
        // Reset granular pitch shifter state
        grainPhase = 0.0f;
        pitchReadPos1 = 0.0f;
        pitchReadPos2 = static_cast<float>(GRAIN_SIZE) / 2.0f;  // Offset by half grain
        wasPitchShifting = false;
    }

    // Reset EQ filter state (called from clear())
    void resetEQState()
    {
        // Reset all biquad states to zero
        eqLowX1L = eqLowX2L = eqLowY1L = eqLowY2L = 0.0f;
        eqLowX1R = eqLowX2R = eqLowY1R = eqLowY2R = 0.0f;
        eqMidX1L = eqMidX2L = eqMidY1L = eqMidY2L = 0.0f;
        eqMidX1R = eqMidX2R = eqMidY1R = eqMidY2R = 0.0f;
        eqHighX1L = eqHighX2L = eqHighY1L = eqHighY2L = 0.0f;
        eqHighX1R = eqHighX2R = eqHighY1R = eqHighY2R = 0.0f;
        eqCoeffsDirty = true;
    }

    // Calculate biquad coefficients for all 3 EQ bands
    void updateEQCoefficients()
    {
        if (!eqCoeffsDirty || currentSampleRate <= 0)
            return;

        const float sampleRate = static_cast<float>(currentSampleRate);

        // Low shelf at 200Hz
        {
            const float freq = 200.0f;
            const float gain = eqLowGain.load();
            const float Q = 0.707f;  // Butterworth Q for smooth shelf

            const float A = std::sqrt(gain);
            const float w0 = 2.0f * juce::MathConstants<float>::pi * freq / sampleRate;
            const float cosW0 = std::cos(w0);
            const float sinW0 = std::sin(w0);
            const float alpha = sinW0 / (2.0f * Q);

            const float a0 = (A + 1.0f) + (A - 1.0f) * cosW0 + 2.0f * std::sqrt(A) * alpha;
            eqLowB0 = (A * ((A + 1.0f) - (A - 1.0f) * cosW0 + 2.0f * std::sqrt(A) * alpha)) / a0;
            eqLowB1 = (2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosW0)) / a0;
            eqLowB2 = (A * ((A + 1.0f) - (A - 1.0f) * cosW0 - 2.0f * std::sqrt(A) * alpha)) / a0;
            eqLowA1 = (-2.0f * ((A - 1.0f) + (A + 1.0f) * cosW0)) / a0;
            eqLowA2 = ((A + 1.0f) + (A - 1.0f) * cosW0 - 2.0f * std::sqrt(A) * alpha) / a0;
        }

        // Mid peak at 1kHz
        {
            const float freq = 1000.0f;
            const float gain = eqMidGain.load();
            const float Q = 1.0f;  // Moderate Q for musical mid control

            const float A = std::sqrt(gain);
            const float w0 = 2.0f * juce::MathConstants<float>::pi * freq / sampleRate;
            const float cosW0 = std::cos(w0);
            const float sinW0 = std::sin(w0);
            const float alpha = sinW0 / (2.0f * Q);

            const float a0 = 1.0f + alpha / A;
            eqMidB0 = (1.0f + alpha * A) / a0;
            eqMidB1 = (-2.0f * cosW0) / a0;
            eqMidB2 = (1.0f - alpha * A) / a0;
            eqMidA1 = (-2.0f * cosW0) / a0;
            eqMidA2 = (1.0f - alpha / A) / a0;
        }

        // High shelf at 4kHz
        {
            const float freq = 4000.0f;
            const float gain = eqHighGain.load();
            const float Q = 0.707f;  // Butterworth Q for smooth shelf

            const float A = std::sqrt(gain);
            const float w0 = 2.0f * juce::MathConstants<float>::pi * freq / sampleRate;
            const float cosW0 = std::cos(w0);
            const float sinW0 = std::sin(w0);
            const float alpha = sinW0 / (2.0f * Q);

            const float a0 = (A + 1.0f) - (A - 1.0f) * cosW0 + 2.0f * std::sqrt(A) * alpha;
            eqHighB0 = (A * ((A + 1.0f) + (A - 1.0f) * cosW0 + 2.0f * std::sqrt(A) * alpha)) / a0;
            eqHighB1 = (-2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosW0)) / a0;
            eqHighB2 = (A * ((A + 1.0f) + (A - 1.0f) * cosW0 - 2.0f * std::sqrt(A) * alpha)) / a0;
            eqHighA1 = (2.0f * ((A - 1.0f) - (A + 1.0f) * cosW0)) / a0;
            eqHighA2 = ((A + 1.0f) - (A - 1.0f) * cosW0 - 2.0f * std::sqrt(A) * alpha) / a0;
        }

        eqCoeffsDirty = false;
    }

    // Apply 3-band EQ to a stereo sample pair (inline for efficiency)
    inline void processEQ(float& sampleL, float& sampleR)
    {
        // Update coefficients if needed (only when gains change)
        if (eqCoeffsDirty)
            updateEQCoefficients();

        // Process low shelf - Left
        float outL = eqLowB0 * sampleL + eqLowB1 * eqLowX1L + eqLowB2 * eqLowX2L
                   - eqLowA1 * eqLowY1L - eqLowA2 * eqLowY2L;
        eqLowX2L = eqLowX1L; eqLowX1L = sampleL;
        eqLowY2L = eqLowY1L; eqLowY1L = outL;
        sampleL = outL;

        // Process low shelf - Right
        float outR = eqLowB0 * sampleR + eqLowB1 * eqLowX1R + eqLowB2 * eqLowX2R
                   - eqLowA1 * eqLowY1R - eqLowA2 * eqLowY2R;
        eqLowX2R = eqLowX1R; eqLowX1R = sampleR;
        eqLowY2R = eqLowY1R; eqLowY1R = outR;
        sampleR = outR;

        // Process mid peak - Left
        outL = eqMidB0 * sampleL + eqMidB1 * eqMidX1L + eqMidB2 * eqMidX2L
             - eqMidA1 * eqMidY1L - eqMidA2 * eqMidY2L;
        eqMidX2L = eqMidX1L; eqMidX1L = sampleL;
        eqMidY2L = eqMidY1L; eqMidY1L = outL;
        sampleL = outL;

        // Process mid peak - Right
        outR = eqMidB0 * sampleR + eqMidB1 * eqMidX1R + eqMidB2 * eqMidX2R
             - eqMidA1 * eqMidY1R - eqMidA2 * eqMidY2R;
        eqMidX2R = eqMidX1R; eqMidX1R = sampleR;
        eqMidY2R = eqMidY1R; eqMidY1R = outR;
        sampleR = outR;

        // Process high shelf - Left
        outL = eqHighB0 * sampleL + eqHighB1 * eqHighX1L + eqHighB2 * eqHighX2L
             - eqHighA1 * eqHighY1L - eqHighA2 * eqHighY2L;
        eqHighX2L = eqHighX1L; eqHighX1L = sampleL;
        eqHighY2L = eqHighY1L; eqHighY1L = outL;
        sampleL = outL;

        // Process high shelf - Right
        outR = eqHighB0 * sampleR + eqHighB1 * eqHighX1R + eqHighB2 * eqHighX2R
             - eqHighA1 * eqHighY1R - eqHighA2 * eqHighY2R;
        eqHighX2R = eqHighX1R; eqHighX1R = sampleR;
        eqHighY2R = eqHighY1R; eqHighY1R = outR;
        sampleR = outR;
    }

    void processRecording(float inputL, float inputR, float& outputL, float& outputR)
    {
        // Consume smoothed values to keep them in sync even during recording
        // This prevents stale state when transitioning to playback
        const float pitchRatio = pitchRatioSmoothed.getNextValue();
        fadeSmoothed.getNextValue();
        playbackRateSmoothed.getNextValue();

        // Only keep phase vocoder running if pitch is shifted
        // This prevents CPU waste and accumulation issues when at unity pitch
        const float pitchDistance = std::abs(pitchRatio - 1.0f);
        if (pitchDistance >= 0.002f)
        {
            // Keep vocoder warm with input audio during recording
            phaseVocoder.setPitchRatio(pitchRatio);
            float dummyL, dummyR;
            phaseVocoder.processSample(inputL, inputR, dummyL, dummyR);
            wasPitchShifting = true;
        }
        else if (wasPitchShifting)
        {
            // Reset vocoder when returning to unity pitch
            phaseVocoder.reset();
            wasPitchShifting = false;
        }

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
            // Target loop length reached - transition to overdub mode
            // This allows continued dubbing on layer 1 until DUB/PLAY/STOP is pressed
            stopRecording(true);  // true = continue to overdub mode
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

        // When fade is at 100%, immediately set multiplier to 1.0
        // This ensures audio at full fade doesn't get any decay
        if (fadeTarget >= 0.99f)
        {
            currentFadeMultiplier.store(1.0f);
        }
        else if (loopWrapped)
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
            float fadeMult = currentFadeMultiplier.load();
            if (fadeMult < targetMultiplier)
            {
                // Recovery: gradually restore volume when knob is turned up
                float recoveryRate = 0.15f;  // Recover ~15% of the gap per loop
                fadeMult += (targetMultiplier - fadeMult) * recoveryRate;
            }
            else
            {
                // Decay: apply normal fade decay
                fadeMult *= decayMultiplier;
            }

            // Clamp to minimum to prevent complete silence
            currentFadeMultiplier.store(std::max(fadeMult, 0.001f));
        }

        // ALWAYS apply fade multiplier - once audio has faded, it stays faded
        // The fadeActive flag only controls whether decay CONTINUES, not whether it's audible
        float fadeToApply = currentFadeMultiplier.load();

        // Read from buffer with real-time crossfade at loop boundary
        const int effectiveStart = loopStart;
        const int effectiveEnd = loopEnd > 0 ? loopEnd : loopLength;
        float rawL, rawR;
        readWithCrossfade(rawL, rawR, playHead, effectiveStart, effectiveEnd);
        rawL *= fadeToApply;
        rawR *= fadeToApply;

        // Apply anti-aliasing low-pass filter when playing at high speeds
        const float currentRate = playbackRateSmoothed.getCurrentValue();
        if (currentRate > 1.01f)
        {
            const float lpfCoeff = std::min(0.9f, 1.0f / currentRate);
            antiAliasLpfL = lpfCoeff * rawL + (1.0f - lpfCoeff) * antiAliasLpfL;
            antiAliasLpfR = lpfCoeff * rawR + (1.0f - lpfCoeff) * antiAliasLpfR;
            rawL = antiAliasLpfL;
            rawR = antiAliasLpfR;
        }

        float loopL, loopR;

        // Check if pitch shifting is needed
        // Use a slightly larger threshold for hysteresis to prevent rapid toggling
        const float pitchDistance = std::abs(pitchRatio - 1.0f);
        const bool isPitchShifting = pitchDistance >= 0.002f;

        if (isPitchShifting)
        {
            // Run phase vocoder for pitch shifting
            phaseVocoder.setPitchRatio(pitchRatio);

            float pitchL, pitchR;
            phaseVocoder.processSample(rawL, rawR, pitchL, pitchR);

            // Crossfade near unity for smooth transition
            if (pitchDistance < 0.01f)
            {
                float crossfade = pitchDistance / 0.01f;
                loopL = rawL * (1.0f - crossfade) + pitchL * crossfade;
                loopR = rawR * (1.0f - crossfade) + pitchR * crossfade;
            }
            else
            {
                loopL = pitchL;
                loopR = pitchR;
            }
        }
        else
        {
            // No pitch shift - bypass vocoder entirely for CPU savings
            // Reset vocoder state when returning to unity to prevent stale data
            if (wasPitchShifting)
            {
                phaseVocoder.reset();
                wasPitchShifting = false;
            }
            loopL = rawL;
            loopR = rawR;
        }

        // Track pitch shifting state for next sample
        if (isPitchShifting)
            wasPitchShifting = true;

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

        // When fade is at 100%, immediately set multiplier to 1.0
        // This ensures new material recorded at 100% fade doesn't get faded
        if (fadeTarget >= 0.99f)
        {
            currentFadeMultiplier.store(1.0f);
        }
        else if (loopWrapped)
        {
            // Apply same fade decay formula as processPlaying for consistency
            // Quadratic curve with moderate decay factor
            float invFade = 1.0f - fadeTarget;
            float decayStrength = invFade * invFade;  // Quadratic curve
            float decayMultiplier = 1.0f - (decayStrength * 0.25f);  // Max 25% reduction per loop at 0%

            // Target multiplier based on fade knob position
            float targetMultiplier = fadeTarget;

            // Recovery or decay based on current vs target
            float fadeMult = currentFadeMultiplier.load();
            if (fadeMult < targetMultiplier)
            {
                // Recovery: gradually restore volume when knob is turned up
                float recoveryRate = 0.15f;
                fadeMult += (targetMultiplier - fadeMult) * recoveryRate;
            }
            else
            {
                // Decay: apply normal fade decay
                fadeMult *= decayMultiplier;
            }

            currentFadeMultiplier.store(std::max(fadeMult, 0.001f));
        }

        // Read existing content with real-time crossfade at loop boundary
        float fadeMult = currentFadeMultiplier.load();
        const int effectiveStart = loopStart;
        const int effectiveEnd = loopEnd > 0 ? loopEnd : loopLength;
        float existingL, existingR;
        readWithCrossfade(existingL, existingR, playHead, effectiveStart, effectiveEnd);
        existingL *= fadeMult;
        existingR *= fadeMult;

        // Get pitch ratio for monitoring
        const float pitchRatio = pitchRatioSmoothed.getNextValue();
        const float pitchDistance = std::abs(pitchRatio - 1.0f);
        const bool isPitchShifting = pitchDistance >= 0.002f;

        // Apply pitch shift to monitoring output if pitch is shifted
        // (We use the raw existingL/R for buffer operations, but pitched for output)
        float monitorL, monitorR;

        if (isPitchShifting)
        {
            // Run phase vocoder for pitch shifting during overdub monitoring
            phaseVocoder.setPitchRatio(pitchRatio);

            float pitchL, pitchR;
            phaseVocoder.processSample(existingL, existingR, pitchL, pitchR);

            // Crossfade near unity for smooth transition
            if (pitchDistance < 0.01f)
            {
                float crossfade = pitchDistance / 0.01f;
                monitorL = existingL * (1.0f - crossfade) + pitchL * crossfade;
                monitorR = existingR * (1.0f - crossfade) + pitchR * crossfade;
            }
            else
            {
                monitorL = pitchL;
                monitorR = pitchR;
            }
            wasPitchShifting = true;
        }
        else
        {
            // No pitch shift - bypass vocoder
            if (wasPitchShifting)
            {
                phaseVocoder.reset();
                wasPitchShifting = false;
            }
            monitorL = existingL;
            monitorR = existingR;
        }

        // Write position
        int writePos = static_cast<int>(playHead) % loopLength;

        // Calculate overdub input gain (Blooper-style: fade at recording start/stop)
        float overdubGain = 1.0f;

        if (isOverdubFadingOut)
        {
            // Fade-out: decrement counter and calculate gain
            if (overdubFadeOutCounter > 0)
            {
                overdubGain = static_cast<float>(overdubFadeOutCounter) / static_cast<float>(OVERDUB_FADE_SAMPLES);
                // Equal-power fade-out
                overdubGain = std::cos((1.0f - overdubGain) * juce::MathConstants<float>::halfPi);
                --overdubFadeOutCounter;
            }
            else
            {
                // Fade-out complete - switch to playing state
                overdubGain = 0.0f;
                isOverdubFadingOut = false;
                state.store(State::Playing);
                waveformCacheDirty = true;  // Regenerate waveform with new overdub content
                DBG("Overdub fade-out complete, now Playing");
            }
        }
        else if (overdubFadeInCounter < OVERDUB_FADE_SAMPLES)
        {
            // Fade-in: increment counter and calculate gain
            overdubGain = static_cast<float>(overdubFadeInCounter) / static_cast<float>(OVERDUB_FADE_SAMPLES);
            // Equal-power fade-in
            overdubGain = std::sin(overdubGain * juce::MathConstants<float>::halfPi);
            ++overdubFadeInCounter;
        }

        // CRITICAL: Apply loop-boundary crossfade to the WRITE operation
        // This prevents pops on overdub layers at the loop seam
        // When writing near the end, we crossfade-out the input
        // When writing near the start, we crossfade-in the input
        // This mirrors what readWithCrossfade does for playback
        const int effectiveLength = effectiveEnd - effectiveStart;
        const int writeXfadeLen = std::min(1024, effectiveLength / 4);

        // Calculate position relative to loop
        float relWritePos = playHead - effectiveStart;
        while (relWritePos < 0) relWritePos += effectiveLength;
        while (relWritePos >= effectiveLength) relWritePos -= effectiveLength;

        // Distance from end of loop
        float distFromEnd = effectiveLength - relWritePos;

        // Apply boundary crossfade gain
        float boundaryGain = 1.0f;
        if (writeXfadeLen > 0)
        {
            if (distFromEnd < writeXfadeLen)
            {
                // Near end: fade OUT the write (will be blended with start content on playback)
                float xfadeProgress = 1.0f - (distFromEnd / writeXfadeLen);
                boundaryGain = std::cos(xfadeProgress * juce::MathConstants<float>::halfPi);
            }
            else if (relWritePos < writeXfadeLen)
            {
                // Near start: fade IN the write (compensates for end region fade)
                float xfadeProgress = relWritePos / writeXfadeLen;
                boundaryGain = std::sin(xfadeProgress * juce::MathConstants<float>::halfPi);
            }
        }

        // Combine all gains: overdub start/stop fade + loop boundary crossfade
        float totalGain = overdubGain * boundaryGain;

        // Apply total gain to input
        float fadedInputL = inputL * totalGain;
        float fadedInputR = inputR * totalGain;

        // Apply fade to existing buffer content, then add faded new input
        // This way existing content decays while new content is added with smooth fade
        bufferL[writePos] = bufferL[writePos] * fadeMult + fadedInputL;
        bufferR[writePos] = bufferR[writePos] * fadeMult + fadedInputR;

        // Soft clip to prevent runaway (gentler curve)
        bufferL[writePos] = softClip(bufferL[writePos]);
        bufferR[writePos] = softClip(bufferR[writePos]);

        // Output: pitch-shifted existing content + input for monitoring
        // Use faded input for monitoring too so user hears the fade
        outputL = monitorL + fadedInputL;
        outputR = monitorR + fadedInputR;

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
        // Note: pitchRatioSmoothed is consumed in processPlaying/processOverdubbing,
        // so we don't consume it here - just read target value for rate calculation
        float effectiveRate = rate;
        if (applyPitch)
        {
            const float pitchRatio = pitchRatioSmoothed.getNextValue();
            effectiveRate *= pitchRatio;
        }
        // When applyPitch is false, processOverdubbing already consumed the smoothed value

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

    // Hermite (cubic) interpolation for smoother variable-speed playback
    // This significantly reduces crackling compared to linear interpolation
    float readWithInterpolation(const std::vector<float>& buffer, float position) const
    {
        if (loopLength <= 0)
            return 0.0f;

        // Get surrounding sample indices (need 4 points for cubic)
        const int idx1 = static_cast<int>(position) % loopLength;
        const int idx0 = (idx1 - 1 + loopLength) % loopLength;  // Previous sample
        const int idx2 = (idx1 + 1) % loopLength;
        const int idx3 = (idx1 + 2) % loopLength;

        const float frac = position - std::floor(position);

        // Get samples
        const float y0 = buffer[idx0];
        const float y1 = buffer[idx1];
        const float y2 = buffer[idx2];
        const float y3 = buffer[idx3];

        // Hermite interpolation coefficients
        const float c0 = y1;
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);

        // Evaluate polynomial
        return ((c3 * frac + c2) * frac + c1) * frac + c0;
    }

    // Real-time crossfade reading for seamless loop boundaries
    // When near the end of the loop, blend with the start region
    void readWithCrossfade(float& outL, float& outR, float position, int effectiveStart, int effectiveEnd) const
    {
        if (loopLength <= 0)
        {
            outL = outR = 0.0f;
            return;
        }

        const int effectiveLength = effectiveEnd - effectiveStart;
        if (effectiveLength <= 0)
        {
            outL = outR = 0.0f;
            return;
        }

        // Crossfade region size (samples from end where crossfade begins)
        const int crossfadeLen = std::min(1024, effectiveLength / 4);

        // Current position relative to loop start
        float relPos = position - effectiveStart;
        while (relPos < 0) relPos += effectiveLength;
        while (relPos >= effectiveLength) relPos -= effectiveLength;

        // Distance from end of loop
        float distFromEnd = effectiveLength - relPos;

        if (distFromEnd < crossfadeLen && crossfadeLen > 0)
        {
            // We're in the crossfade region near the end
            // Blend current position with wrapped position (start region)
            float crossfadeProgress = 1.0f - (distFromEnd / crossfadeLen);  // 0 at start of xfade, 1 at end

            // Equal-power crossfade
            float gainCurrent = std::cos(crossfadeProgress * juce::MathConstants<float>::halfPi);
            float gainWrapped = std::sin(crossfadeProgress * juce::MathConstants<float>::halfPi);

            // Read from current position (near end)
            float currentL = readWithInterpolation(bufferL, position);
            float currentR = readWithInterpolation(bufferR, position);

            // Read from wrapped position (near start)
            float wrappedPos = effectiveStart + (crossfadeLen - distFromEnd);
            float wrappedL = readWithInterpolation(bufferL, wrappedPos);
            float wrappedR = readWithInterpolation(bufferR, wrappedPos);

            // Blend
            outL = currentL * gainCurrent + wrappedL * gainWrapped;
            outR = currentR * gainCurrent + wrappedR * gainWrapped;
        }
        else
        {
            // Normal reading outside crossfade region
            outL = readWithInterpolation(bufferL, position);
            outR = readWithInterpolation(bufferR, position);
        }
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

    void applyCrossfade(bool isInitialRecording = true)
    {
        // Real-time crossfade is now handled in readWithCrossfade()
        // Additionally, we adjust the loop length to end on a zero crossing
        // for cleaner loop boundaries
        (void)isInitialRecording;  // Unused

        if (loopLength > 0)
        {
            // Try to adjust loop end to nearest zero crossing
            int adjusted = findNearestZeroCrossing(loopLength - 1, 256);
            if (adjusted != loopLength - 1 && adjusted > loopLength / 2)
            {
                int oldLength = loopLength;
                loopLength = adjusted + 1;
                loopEnd = loopLength;
                DBG("applyCrossfade() - adjusted loop length from " + juce::String(oldLength) +
                    " to " + juce::String(loopLength) + " for zero crossing");
            }
            else
            {
                DBG("applyCrossfade() - using real-time crossfade, no length adjustment needed");
            }
        }
    }

    // Find the nearest zero crossing within searchRange samples of position
    // Returns the position of the zero crossing, or the original position if none found
    int findNearestZeroCrossing(int position, int searchRange) const
    {
        if (loopLength <= 0 || position < 0 || position >= loopLength)
            return position;

        // Search outward from position for a zero crossing
        // A zero crossing is where consecutive samples have opposite signs
        int bestPos = position;
        int bestDist = searchRange + 1;

        for (int offset = 0; offset <= searchRange; ++offset)
        {
            // Check position + offset
            int pos = position + offset;
            if (pos < loopLength - 1)
            {
                float curr = (bufferL[pos] + bufferR[pos]) * 0.5f;
                float next = (bufferL[pos + 1] + bufferR[pos + 1]) * 0.5f;

                // Zero crossing: signs differ or one is exactly zero
                if ((curr >= 0.0f && next < 0.0f) || (curr < 0.0f && next >= 0.0f) ||
                    std::abs(curr) < 0.001f)
                {
                    if (offset < bestDist)
                    {
                        bestDist = offset;
                        bestPos = pos;
                    }
                    break;  // Found closest in forward direction
                }
            }

            // Check position - offset
            if (offset > 0)
            {
                int posBack = position - offset;
                if (posBack >= 0 && posBack < loopLength - 1)
                {
                    float curr = (bufferL[posBack] + bufferR[posBack]) * 0.5f;
                    float next = (bufferL[posBack + 1] + bufferR[posBack + 1]) * 0.5f;

                    if ((curr >= 0.0f && next < 0.0f) || (curr < 0.0f && next >= 0.0f) ||
                        std::abs(curr) < 0.001f)
                    {
                        if (offset < bestDist)
                        {
                            bestDist = offset;
                            bestPos = posBack;
                        }
                        break;  // Found closest in backward direction
                    }
                }
            }
        }

        return bestPos;
    }

    void removeDCOffset()
    {
        if (loopLength <= 0)
            return;

        // Calculate mean (DC offset) for each channel
        double sumL = 0.0, sumR = 0.0;
        for (int i = 0; i < loopLength; ++i)
        {
            sumL += bufferL[i];
            sumR += bufferR[i];
        }
        const float dcL = static_cast<float>(sumL / loopLength);
        const float dcR = static_cast<float>(sumR / loopLength);

        // Only remove if there's significant DC offset (> 0.001)
        if (std::abs(dcL) > 0.001f || std::abs(dcR) > 0.001f)
        {
            for (int i = 0; i < loopLength; ++i)
            {
                bufferL[i] -= dcL;
                bufferR[i] -= dcR;
            }
            DBG("Removed DC offset: L=" + juce::String(dcL, 4) + " R=" + juce::String(dcR, 4));
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
