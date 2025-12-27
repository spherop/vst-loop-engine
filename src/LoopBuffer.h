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
        playbackRateSmoothed.reset(sampleRate, 0.05);  // 50ms smoothing
        playbackRateSmoothed.setCurrentAndTargetValue(1.0f);

        pitchRatioSmoothed.reset(sampleRate, 0.02);  // 20ms smoothing for pitch (faster response)
        pitchRatioSmoothed.setCurrentAndTargetValue(1.0f);

        fadeSmoothed.reset(sampleRate, 0.1);  // 100ms smoothing for fade
        fadeSmoothed.setCurrentAndTargetValue(1.0f);  // Default to 100% (no fade)

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
        fadeActive.store(false);
        playbackRateSmoothed.setCurrentAndTargetValue(1.0f);
        pitchRatioSmoothed.setCurrentAndTargetValue(1.0f);
        fadeSmoothed.setCurrentAndTargetValue(1.0f);
        currentFadeMultiplier.store(1.0f);
        lastPlayheadPosition = 0.0f;

        // Reset pitch shifters
        blockPitchShifter.reset();
        phaseVocoder.reset();
        initGrains();
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
        fadeActive.store(other.fadeActive.load());
        currentFadeMultiplier.store(other.currentFadeMultiplier.load());
        lastPlayheadPosition = other.lastPlayheadPosition;

        // Reset pitch shifters (they have internal state that shouldn't be copied)
        blockPitchShifter.reset();
        phaseVocoder.reset();
        initGrains();

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
                currentFadeMultiplier.store(1.0f);
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

        float maxVal = 0.0f;
        for (int i = 0; i < loopLength; ++i)
        {
            float val = std::abs(bufferL[i]) + std::abs(bufferR[i]);
            maxVal = std::max(maxVal, val * 0.5f);
        }
        return maxVal;
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

        // Phase 1: Read raw loop audio for entire block
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

            // Read from loop buffer with crossfade handling
            float rawL = 0.0f;
            float rawR = 0.0f;

            if (effectiveLength > CROSSFADE_SAMPLES * 2)
            {
                const float posInLoop = playHead - effectiveStart;
                const float distToEnd = effectiveEnd - playHead;
                const bool reversed = isReversed.load();

                bool inCrossfadeRegion = false;
                float crossfadeProgress = 0.0f;

                if (!reversed && distToEnd < CROSSFADE_SAMPLES && distToEnd >= 0)
                {
                    inCrossfadeRegion = true;
                    crossfadeProgress = distToEnd / static_cast<float>(CROSSFADE_SAMPLES);
                }
                else if (reversed && posInLoop < CROSSFADE_SAMPLES && posInLoop >= 0)
                {
                    inCrossfadeRegion = true;
                    crossfadeProgress = posInLoop / static_cast<float>(CROSSFADE_SAMPLES);
                }

                if (inCrossfadeRegion)
                {
                    const float fadeOutGain = std::sin(crossfadeProgress * juce::MathConstants<float>::halfPi);
                    const float fadeInGain = std::cos(crossfadeProgress * juce::MathConstants<float>::halfPi);

                    float currentL = readWithInterpolation(bufferL, playHead) * fadeToApply * fadeOutGain;
                    float currentR = readWithInterpolation(bufferR, playHead) * fadeToApply * fadeOutGain;

                    float wrappedPos;
                    if (!reversed)
                        wrappedPos = effectiveStart + (CROSSFADE_SAMPLES - distToEnd);
                    else
                        wrappedPos = effectiveEnd - (CROSSFADE_SAMPLES - posInLoop);

                    float wrapL = readWithInterpolation(bufferL, wrappedPos) * fadeToApply * fadeInGain;
                    float wrapR = readWithInterpolation(bufferR, wrappedPos) * fadeToApply * fadeInGain;

                    rawL = currentL + wrapL;
                    rawR = currentR + wrapR;
                }
                else
                {
                    rawL = readWithInterpolation(bufferL, playHead) * fadeToApply;
                    rawR = readWithInterpolation(bufferR, playHead) * fadeToApply;
                }
            }
            else
            {
                rawL = readWithInterpolation(bufferL, playHead) * fadeToApply;
                rawR = readWithInterpolation(bufferR, playHead) * fadeToApply;
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
            leftChannel[i] = outL + leftChannel[i];
            if (rightChannel)
                rightChannel[i] = outR + rightChannel[i];
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

    // Per-layer volume (0.0 to 1.0)
    void setVolume(float vol) { volume.store(std::clamp(vol, 0.0f, 1.0f)); }
    float getVolume() const { return volume.load(); }

    // Per-layer pan (-1.0 = full left, 0.0 = center, 1.0 = full right)
    void setPan(float p) { pan.store(std::clamp(p, -1.0f, 1.0f)); }
    float getPan() const { return pan.load(); }

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
        const float fadeMultiplier = currentFadeMultiplier.load();

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
    std::atomic<bool> isReversed { false };
    std::atomic<bool> isMuted { false };
    std::atomic<float> volume { 1.0f };      // Per-layer volume (0.0 to 1.0)
    std::atomic<float> pan { 0.0f };         // Per-layer pan (-1.0 to 1.0)
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
    std::atomic<float> currentFadeMultiplier { 1.0f };  // Current fade level (starts at 1.0, decays each loop)
    float lastPlayheadPosition = 0.0f;   // For detecting loop boundary crossings

    // Block-based pitch shifter (efficient, processes entire blocks)
    StereoBlockPitchShifter blockPitchShifter;

    // Pre-allocated buffers for block-based pitch processing
    std::vector<float> pitchInputL;
    std::vector<float> pitchInputR;
    std::vector<float> pitchOutputL;
    std::vector<float> pitchOutputR;

    // Legacy sample-by-sample phase vocoder (kept for compatibility)
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

        // Calculate loop boundary crossfade to prevent pops
        // Uses equal-power (sine-based) crossfade for smooth transitions
        const int effectiveStart = loopStart;
        const int effectiveEnd = loopEnd > 0 ? loopEnd : loopLength;
        const int effectiveLength = effectiveEnd - effectiveStart;

        float rawL = 0.0f;
        float rawR = 0.0f;

        if (effectiveLength > CROSSFADE_SAMPLES * 2)
        {
            const float posInLoop = playHead - effectiveStart;
            const float distToEnd = effectiveEnd - playHead;
            const bool reversed = isReversed.load();

            // Determine if we're in a crossfade region
            bool inCrossfadeRegion = false;
            float crossfadeProgress = 0.0f;  // 0 = at boundary, 1 = out of crossfade region

            if (!reversed)
            {
                // Forward: crossfade near end (fade out current, fade in from start)
                if (distToEnd < CROSSFADE_SAMPLES && distToEnd >= 0)
                {
                    inCrossfadeRegion = true;
                    crossfadeProgress = distToEnd / static_cast<float>(CROSSFADE_SAMPLES);
                }
            }
            else
            {
                // Reversed: crossfade near start (fade out current, fade in from end)
                if (posInLoop < CROSSFADE_SAMPLES && posInLoop >= 0)
                {
                    inCrossfadeRegion = true;
                    crossfadeProgress = posInLoop / static_cast<float>(CROSSFADE_SAMPLES);
                }
            }

            if (inCrossfadeRegion)
            {
                // Equal-power crossfade using sine curves
                // This maintains constant loudness through the crossfade
                const float fadeOutGain = std::sin(crossfadeProgress * juce::MathConstants<float>::halfPi);
                const float fadeInGain = std::cos(crossfadeProgress * juce::MathConstants<float>::halfPi);

                // Read from current position (fading out)
                float currentL = readWithInterpolation(bufferL, playHead) * fadeToApply * fadeOutGain;
                float currentR = readWithInterpolation(bufferR, playHead) * fadeToApply * fadeOutGain;

                // Calculate wrapped position (what we're fading into)
                float wrappedPos;
                if (!reversed)
                {
                    // Fading out at end, blending with start
                    wrappedPos = effectiveStart + (CROSSFADE_SAMPLES - distToEnd);
                }
                else
                {
                    // Fading out at start, blending with end
                    wrappedPos = effectiveEnd - (CROSSFADE_SAMPLES - posInLoop);
                }

                // Read from wrapped position (fading in)
                float wrapL = readWithInterpolation(bufferL, wrappedPos) * fadeToApply * fadeInGain;
                float wrapR = readWithInterpolation(bufferR, wrappedPos) * fadeToApply * fadeInGain;

                rawL = currentL + wrapL;
                rawR = currentR + wrapR;
            }
            else
            {
                // Not in crossfade region - normal playback
                rawL = readWithInterpolation(bufferL, playHead) * fadeToApply;
                rawR = readWithInterpolation(bufferR, playHead) * fadeToApply;
            }
        }
        else
        {
            // Loop too short for crossfade - just read directly
            rawL = readWithInterpolation(bufferL, playHead) * fadeToApply;
            rawR = readWithInterpolation(bufferR, playHead) * fadeToApply;
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

        // Read existing content (with fade applied)
        float fadeMult = currentFadeMultiplier.load();
        float existingL = readWithInterpolation(bufferL, playHead) * fadeMult;
        float existingR = readWithInterpolation(bufferR, playHead) * fadeMult;

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

        // Apply fade to existing buffer content, then add new input
        // This way existing content decays while new content is added at full volume
        bufferL[writePos] = bufferL[writePos] * fadeMult + inputL;
        bufferR[writePos] = bufferR[writePos] * fadeMult + inputR;

        // Soft clip to prevent runaway (gentler curve)
        bufferL[writePos] = softClip(bufferL[writePos]);
        bufferR[writePos] = softClip(bufferR[writePos]);

        // Output: pitch-shifted existing content + input for monitoring
        outputL = monitorL + inputL;
        outputR = monitorR + inputR;

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

        // Step 1: Remove DC offset from entire buffer
        // This prevents low-frequency pops at loop boundaries
        removeDCOffset();

        // Step 2: Apply equal-power crossfade at the loop boundary for seamless looping
        // Use a longer fade region and create a symmetric crossfade where
        // the end region and start region blend together
        for (int i = 0; i < CROSSFADE_SAMPLES; ++i)
        {
            // Equal-power crossfade using sine/cosine curves
            const float progress = static_cast<float>(i) / static_cast<float>(CROSSFADE_SAMPLES);
            const float fadeIn = std::sin(progress * juce::MathConstants<float>::halfPi);
            const float fadeOut = std::cos(progress * juce::MathConstants<float>::halfPi);

            const int endIdx = loopLength - CROSSFADE_SAMPLES + i;
            const int startIdx = i;

            // Create a temporary blend of end and start
            const float blendL = bufferL[startIdx] * fadeIn + bufferL[endIdx] * fadeOut;
            const float blendR = bufferR[startIdx] * fadeIn + bufferR[endIdx] * fadeOut;

            // Apply to both start AND end regions for symmetry
            // This ensures both ends of the loop crossfade smoothly
            bufferL[startIdx] = blendL;
            bufferR[startIdx] = blendR;
            bufferL[endIdx] = bufferL[startIdx] * fadeOut + bufferL[endIdx] * fadeIn;
            bufferR[endIdx] = bufferR[startIdx] * fadeOut + bufferR[endIdx] * fadeIn;
        }

        // Step 3: Also apply a gentle fade-in at the very start to catch any initial transient
        const int shortFade = std::min(256, loopLength / 4);
        for (int i = 0; i < shortFade; ++i)
        {
            const float gain = static_cast<float>(i) / static_cast<float>(shortFade);
            const float smoothGain = gain * gain * (3.0f - 2.0f * gain); // Smoothstep
            bufferL[i] *= smoothGain;
            bufferR[i] *= smoothGain;
        }
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
