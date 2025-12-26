#pragma once

#include "LoopBuffer.h"
#include <array>
#include <atomic>

class LoopEngine
{
public:
    static constexpr int NUM_LAYERS = 8;

    LoopEngine() = default;

    void prepare(double sampleRate, int samplesPerBlock)
    {
        currentSampleRate = sampleRate;

        // Prepare all layers
        for (int i = 0; i < NUM_LAYERS; ++i)
        {
            layers[i].prepare(sampleRate, samplesPerBlock);
        }

        // Pre-allocate buffers to avoid allocation in processBlock
        // Using stereo (2 channels) as that's the typical case
        inputBuffer.setSize(2, samplesPerBlock);
        layerBuffer.setSize(2, samplesPerBlock);
        dummyBuffer.setSize(2, samplesPerBlock);
        loopOnlyBuffer.setSize(2, samplesPerBlock);

        // Reset state
        currentLayer = 0;
        highestLayer = 0;
        masterLoopLength = 0;
    }

    // Transport controls - Blooper-style workflow:
    // 1. REC (idle) -> Start recording
    // 2. REC (recording) -> Stop recording, start playback
    // 3. REC (playing) -> Start overdubbing (record on top of playback)
    // 4. REC (overdubbing) -> Stop overdubbing, continue playback
    void record()
    {
        LoopBuffer::State currentState = getCurrentState();
        DBG("LoopEngine::record() called, currentState=" + juce::String(static_cast<int>(currentState)));

        if (currentState == LoopBuffer::State::Idle)
        {
            // Calculate target length based on preset bars
            int targetLength = getTargetLoopLengthSamples();
            DBG("record() - Idle state, targetLength=" + juce::String(targetLength));

            // Find first available (empty) layer
            int availableLayer = findFirstAvailableLayer();

            if (availableLayer < 0)
            {
                DBG("record() - All layers full, cannot record");
                return;
            }

            // If no content exists anywhere, this is a fresh recording
            if (!hasContent())
            {
                currentLayer = availableLayer;
                // Reset loop parameters to defaults when starting new recording
                resetLoopParams();
                DBG("record() - Starting fresh recording on layer " + juce::String(currentLayer));
                layers[currentLayer].startRecording(targetLength);
            }
            else
            {
                // There's existing content, so use master loop length
                currentLayer = availableLayer;
                // Use master loop length for subsequent layers
                int layerTarget = (masterLoopLength > 0) ? masterLoopLength : targetLength;
                DBG("record() - Starting recording on layer " + juce::String(currentLayer) + " with target " + juce::String(layerTarget));
                layers[currentLayer].startRecording(layerTarget);
            }
        }
        else if (currentState == LoopBuffer::State::Recording)
        {
            // Stop initial recording -> start overdub on NEW layer
            // Each REC tap always creates a new layer
            DBG("record() - Stopping recording, starting overdub on new layer");
            stopRecording(false);  // false = go to Playing first

            // Set master loop length from first recording if not already set
            if (masterLoopLength == 0)
            {
                masterLoopLength = layers[currentLayer].getLoopLengthSamples();
            }

            // Immediately start overdubbing on a new layer
            overdub();
        }
        else if (currentState == LoopBuffer::State::Playing)
        {
            // REC while playing starts overdubbing on a NEW layer
            DBG("record() - Playing state, calling overdub() for new layer");
            overdub();
        }
        else if (currentState == LoopBuffer::State::Overdubbing)
        {
            // REC while overdubbing: stop current layer, start NEW layer immediately
            // Each REC tap always creates a new layer (no toggle off)
            DBG("record() - Overdubbing state, stopping layer " + juce::String(currentLayer) + " and starting new layer");
            layers[currentLayer].stopOverdub();

            // Immediately start overdubbing on next layer if available
            if (highestLayer < NUM_LAYERS - 1)
            {
                overdub();  // This will create a new layer
            }
            else
            {
                DBG("record() - Max layers reached, continuing playback");
            }
        }
    }

    void resetLoopParams()
    {
        // Reset master reverse state
        isReversed = false;

        // Reset all layers to default loop parameters
        // NOTE: Pitch and Fade are NOT reset here because they are controlled
        // by APVTS parameters which are updated on every audio callback.
        // Resetting them here causes oscillation between 0 and the parameter value.
        for (int i = 0; i < NUM_LAYERS; ++i)
        {
            layers[i].setLoopStart(0.0f);
            layers[i].setLoopEnd(1.0f);
            layers[i].setPlaybackRate(1.0f);
            layers[i].setReverse(false);
            // Pitch and fade are controlled by APVTS, don't reset here
        }
    }

    void stopRecording(bool continueToOverdub = false)
    {
        LoopBuffer::State currentState = getCurrentState();

        if (currentState == LoopBuffer::State::Recording)
        {
            layers[currentLayer].stopRecording(continueToOverdub);

            // Set master loop length from first recording
            if (masterLoopLength == 0)
            {
                masterLoopLength = layers[currentLayer].getLoopLengthSamples();
            }

            highestLayer = std::max(highestLayer, currentLayer);
        }
    }

    void play()
    {
        LoopBuffer::State currentState = getCurrentState();

        // If currently overdubbing, just stop the overdub (smooth transition)
        // Don't restart playback - keep position and continue playing
        if (currentState == LoopBuffer::State::Overdubbing)
        {
            DBG("play() - Stopping overdub on layer " + juce::String(currentLayer) + " (smooth transition)");
            layers[currentLayer].stopOverdub();
            return;
        }

        // Otherwise, start/restart playback of all layers
        for (int i = 0; i <= highestLayer; ++i)
        {
            if (layers[i].hasContent())
            {
                layers[i].play();
            }
        }
    }

    void stop()
    {
        // Stop all layers and clear their pitch shifter state
        for (int i = 0; i < NUM_LAYERS; ++i)
        {
            layers[i].stop();
        }
        DBG("LoopEngine::stop() - All layers stopped");
    }

    // Clear a specific layer (1-indexed for UI)
    void clearLayer(int layer)
    {
        int idx = layer - 1;  // Convert to 0-indexed
        if (idx < 0 || idx >= NUM_LAYERS)
            return;

        DBG("clearLayer() - Clearing layer " + juce::String(layer));
        layers[idx].clear();

        // Update highestLayer if we cleared the highest one
        if (idx == highestLayer)
        {
            // Find new highest layer with content
            highestLayer = 0;
            for (int i = NUM_LAYERS - 1; i >= 0; --i)
            {
                if (layers[i].hasContent())
                {
                    highestLayer = i;
                    break;
                }
            }
        }

        // If no layers have content, reset master loop length
        bool anyContent = false;
        for (int i = 0; i < NUM_LAYERS; ++i)
        {
            if (layers[i].hasContent())
            {
                anyContent = true;
                break;
            }
        }
        if (!anyContent)
        {
            masterLoopLength = 0;
            highestLayer = 0;
            currentLayer = 0;
        }
    }

    // Delete a specific layer and shuffle all subsequent layers down (1-indexed for UI)
    // This "clips out" the layer so layers after it move to fill the gap
    void deleteLayer(int layer)
    {
        int idx = layer - 1;  // Convert to 0-indexed
        if (idx < 0 || idx >= NUM_LAYERS)
            return;

        DBG("deleteLayer() - Deleting layer " + juce::String(layer) + " and shuffling");

        // Clear the target layer
        layers[idx].clear();

        // Shuffle all subsequent layers down by one
        for (int i = idx; i < NUM_LAYERS - 1; ++i)
        {
            if (layers[i + 1].hasContent())
            {
                // Copy content from layer i+1 to layer i
                layers[i].copyFrom(layers[i + 1]);
                layers[i + 1].clear();
                DBG("  Moved layer " + juce::String(i + 2) + " to layer " + juce::String(i + 1));
            }
        }

        // Recalculate highest layer
        highestLayer = 0;
        for (int i = NUM_LAYERS - 1; i >= 0; --i)
        {
            if (layers[i].hasContent())
            {
                highestLayer = i;
                break;
            }
        }

        // Adjust current layer if it was above the deleted layer
        if (currentLayer > idx && currentLayer > 0)
        {
            currentLayer--;
        }
        // If current layer was the deleted one, stay at same index (now has different content)
        // but clamp to valid range
        if (currentLayer > highestLayer)
        {
            currentLayer = highestLayer;
        }

        // If no layers have content, reset everything
        bool anyContent = false;
        for (int i = 0; i < NUM_LAYERS; ++i)
        {
            if (layers[i].hasContent())
            {
                anyContent = true;
                break;
            }
        }
        if (!anyContent)
        {
            masterLoopLength = 0;
            highestLayer = 0;
            currentLayer = 0;
        }
    }

    // Find the first available (empty) layer index (0-indexed)
    int findFirstAvailableLayer() const
    {
        for (int i = 0; i < NUM_LAYERS; ++i)
        {
            if (!layers[i].hasContent())
            {
                return i;
            }
        }
        return -1;  // All layers full
    }

    // Check if a specific layer (1-indexed) has content
    bool layerHasContent(int layer) const
    {
        int idx = layer - 1;  // Convert to 0-indexed
        if (idx >= 0 && idx < NUM_LAYERS)
        {
            return layers[idx].hasContent();
        }
        return false;
    }

    void overdub()
    {
        LoopBuffer::State currentState = getCurrentState();
        DBG("LoopEngine::overdub() called, currentState=" + juce::String(static_cast<int>(currentState)) +
            " currentLayer=" + juce::String(currentLayer) +
            " highestLayer=" + juce::String(highestLayer) +
            " hasContent=" + juce::String(layers[0].hasContent() ? "true" : "false"));

        // Ensure masterLoopLength is set if layer 0 has content
        // This handles the case where fixed-length recording auto-stopped
        // (auto-stop bypasses LoopEngine::stopRecording so masterLoopLength wasn't set)
        if (masterLoopLength == 0 && layers[0].hasContent())
        {
            masterLoopLength = layers[0].getLoopLengthSamples();
            highestLayer = std::max(highestLayer, 0);
            DBG("overdub() - Fixed masterLoopLength from layer 0: " + juce::String(masterLoopLength));
        }

        if (currentState == LoopBuffer::State::Playing)
        {
            // Blooper behavior: Recording after undo clears all undone layers
            // This "commits" the undo - you can no longer redo after recording new content
            clearUndoneLayers();

            // Check if current layer already has content
            // If it does, create a NEW layer for overdub (Blooper-style)
            // If it's empty, overdub on the current layer (no new layer creation)
            if (layers[currentLayer].hasContent())
            {
                // Current layer has content - create new layer
                if (highestLayer < NUM_LAYERS - 1)
                {
                    // Get current playhead from layer 0 to sync new layer
                    float masterPlayhead = layers[0].getRawPlayhead();

                    currentLayer = highestLayer + 1;
                    highestLayer = currentLayer;
                    DBG("Starting overdub on NEW layer " + juce::String(currentLayer) +
                        " syncing playhead to " + juce::String(masterPlayhead));
                    layers[currentLayer].startOverdubOnNewLayer(masterLoopLength);
                    layers[currentLayer].setPlayhead(masterPlayhead);
                }
                else
                {
                    DBG("Cannot overdub - max layers reached");
                }
            }
            else
            {
                // Current layer is empty - overdub on this layer (don't create new)
                float masterPlayhead = layers[0].getRawPlayhead();
                DBG("Starting overdub on EXISTING empty layer " + juce::String(currentLayer) +
                    " syncing playhead to " + juce::String(masterPlayhead));
                layers[currentLayer].startOverdubOnNewLayer(masterLoopLength);
                layers[currentLayer].setPlayhead(masterPlayhead);
                highestLayer = std::max(highestLayer, currentLayer);
            }
        }
        else if (currentState == LoopBuffer::State::Overdubbing)
        {
            // Stop overdubbing
            DBG("Stopping overdub on layer " + juce::String(currentLayer));
            layers[currentLayer].stopOverdub();
        }
        else if (currentState == LoopBuffer::State::Idle && highestLayer >= 0 && layers[0].hasContent())
        {
            // Blooper behavior: Recording after undo clears all undone layers
            clearUndoneLayers();

            // If idle with content, play and immediately overdub on a new layer
            DBG("Idle with content - starting play + overdub on new layer");
            play();
            if (highestLayer < NUM_LAYERS - 1)
            {
                // Get current playhead from layer 0 (should be 0 since we just started playing)
                float masterPlayhead = layers[0].getRawPlayhead();

                currentLayer = highestLayer + 1;
                highestLayer = currentLayer;
                layers[currentLayer].startOverdubOnNewLayer(masterLoopLength);
                layers[currentLayer].setPlayhead(masterPlayhead);
            }
        }
        else
        {
            DBG("overdub() - no action taken");
        }
    }

    void undo()
    {
        if (currentLayer > 0)
        {
            // Mute the current layer instead of stopping it completely
            // This keeps the layer content but makes it silent and visually "undone"
            layers[currentLayer].setMuted(true);
            --currentLayer;

            // Track highest undone layer for redo
            // highestLayer stays the same - it tracks max recorded, not current
            DBG("undo() - muted layer " + juce::String(currentLayer + 1) +
                ", now on layer " + juce::String(currentLayer) +
                ", highestLayer still " + juce::String(highestLayer));
        }
    }

    void redo()
    {
        // Can only redo if there are undone layers above current
        if (currentLayer < highestLayer)
        {
            ++currentLayer;
            // Unmute the layer to restore it
            layers[currentLayer].setMuted(false);
            // Make sure it's playing if it was playing before undo
            if (layers[currentLayer].hasContent() && layers[currentLayer].getState() == LoopBuffer::State::Idle)
            {
                layers[currentLayer].play();
            }
            DBG("redo() - unmuted and restored layer " + juce::String(currentLayer));
        }
    }

    // Blooper behavior: Clear all undone layers when recording new content
    // This "commits" the undo - recording after undo permanently removes undone layers
    void clearUndoneLayers()
    {
        // Any layers above currentLayer that are muted are "undone" layers
        for (int i = currentLayer + 1; i <= highestLayer; ++i)
        {
            if (layers[i].getMuted())
            {
                DBG("clearUndoneLayers() - permanently clearing undone layer " + juce::String(i));
                layers[i].clear();
            }
        }
        // Update highestLayer to current since undone layers are gone
        highestLayer = currentLayer;
    }

    void clear()
    {
        // Blooper-style clear behavior depends on current state:
        // - If STOPPED (Idle): Full reset, clear loop length so it can be recreated
        // - If ACTIVE (Recording/Playing/Overdubbing): Keep loop length and continue in DUB mode
        LoopBuffer::State currentState = getCurrentState();
        bool wasActive = (currentState != LoopBuffer::State::Idle);
        int preservedLength = masterLoopLength;

        for (int i = 0; i < NUM_LAYERS; ++i)
        {
            layers[i].clear();
        }
        currentLayer = 0;
        highestLayer = 0;

        // If we were actively playing/recording, preserve the length and start overdubbing
        if (wasActive && preservedLength > 0)
        {
            masterLoopLength = preservedLength;
            // Start overdubbing on layer 0 with the preserved loop length
            // startOverdubOnNewLayer sets up buffer and puts layer in Overdubbing state
            layers[0].startOverdubOnNewLayer(masterLoopLength);
            DBG("clear() - Active state: preserved loop length " + juce::String(masterLoopLength) +
                " and started DUB on layer 0");
        }
        else
        {
            // Stopped/idle: full reset including loop length so user can create fresh loop
            masterLoopLength = 0;
            DBG("clear() - Idle state: full reset, loop length cleared");
        }
    }

    // Layer navigation
    void jumpToLayer(int layer)
    {
        if (layer >= 0 && layer < NUM_LAYERS && layer <= highestLayer)
        {
            currentLayer = layer;
        }
    }

    // Per-layer mute (1-indexed for UI)
    void setLayerMuted(int layer, bool muted)
    {
        int idx = layer - 1;  // Convert to 0-indexed
        if (idx >= 0 && idx < NUM_LAYERS)
        {
            layers[idx].setMuted(muted);
            DBG("Layer " + juce::String(layer) + " muted: " + juce::String(muted ? "true" : "false"));
        }
    }

    bool getLayerMuted(int layer) const
    {
        int idx = layer - 1;  // Convert to 0-indexed
        if (idx >= 0 && idx < NUM_LAYERS)
        {
            return layers[idx].getMuted();
        }
        return false;
    }

    // Parameters (apply to all layers)
    void setLoopStart(float normalizedPos)
    {
        for (int i = 0; i <= highestLayer; ++i)
        {
            layers[i].setLoopStart(normalizedPos);
        }
    }

    void setLoopEnd(float normalizedPos)
    {
        for (int i = 0; i <= highestLayer; ++i)
        {
            layers[i].setLoopEnd(normalizedPos);
        }
    }

    void setSpeed(float rate)
    {
        for (int i = 0; i <= highestLayer; ++i)
        {
            layers[i].setPlaybackRate(rate);
        }
    }

    void setReverse(bool reversed)
    {
        // Store the master reverse state
        isReversed = reversed;

        // Apply to all layers (including ones that might be recorded later)
        for (int i = 0; i < NUM_LAYERS; ++i)
        {
            layers[i].setReverse(reversed);
        }
    }

    void setPitchShift(float semitones)
    {
        // Apply to ALL layers so new layers get the current setting
        for (int i = 0; i < NUM_LAYERS; ++i)
        {
            layers[i].setPitchShift(semitones);
        }
    }

    // Fade/decay: 0.0 = fade completely after one loop, 1.0 = no fade (infinite)
    void setFade(float fadeAmount)
    {
        for (int i = 0; i < NUM_LAYERS; ++i)
        {
            layers[i].setFade(fadeAmount);
        }
    }

    // Process audio - returns separate loop playback and input buffers for Blooper-style processing
    // Effects like degrade should only be applied to loopPlaybackBuffer, not inputBuffer
    void processBlock(juce::AudioBuffer<float>& buffer,
                      juce::AudioBuffer<float>* loopPlaybackBuffer = nullptr,
                      juce::AudioBuffer<float>* inputPassthroughBuffer = nullptr)
    {
        const int numSamples = buffer.getNumSamples();
        const int numChannels = buffer.getNumChannels();

        // Calculate input levels for metering (before any processing)
        float peakL = 0.0f;
        float peakR = 0.0f;
        const float* inL = buffer.getReadPointer(0);
        const float* inR = numChannels > 1 ? buffer.getReadPointer(1) : inL;
        for (int i = 0; i < numSamples; ++i)
        {
            peakL = std::max(peakL, std::abs(inL[i]));
            peakR = std::max(peakR, std::abs(inR[i]));
        }
        // Apply smoothing (fast attack, slow release)
        float currentL = inputLevelL.load();
        float currentR = inputLevelR.load();
        inputLevelL.store(peakL > currentL ? peakL : currentL * 0.95f + peakL * 0.05f);
        inputLevelR.store(peakR > currentR ? peakR : currentR * 0.95f + peakR * 0.05f);

        // Ensure our pre-allocated buffers are correctly sized for this block
        // (This is a no-op if they're already the right size)
        if (inputBuffer.getNumSamples() < numSamples || inputBuffer.getNumChannels() < numChannels)
            inputBuffer.setSize(numChannels, numSamples, false, false, true);
        if (layerBuffer.getNumSamples() < numSamples || layerBuffer.getNumChannels() < numChannels)
            layerBuffer.setSize(numChannels, numSamples, false, false, true);
        if (dummyBuffer.getNumSamples() < numSamples || dummyBuffer.getNumChannels() < numChannels)
            dummyBuffer.setSize(numChannels, numSamples, false, false, true);
        if (loopOnlyBuffer.getNumSamples() < numSamples || loopOnlyBuffer.getNumChannels() < numChannels)
            loopOnlyBuffer.setSize(numChannels, numSamples, false, false, true);

        // Copy input to our pre-allocated buffer
        for (int ch = 0; ch < numChannels; ++ch)
            inputBuffer.copyFrom(ch, 0, buffer, ch, 0, numSamples);

        // Apply input mute
        if (inputMuted.load())
        {
            inputBuffer.clear();
        }

        // Clear output buffers - we'll add layers to them
        buffer.clear();
        loopOnlyBuffer.clear();

        // Process ALL layers with content up to highestLayer
        // Each layer can be independently muted regardless of currentLayer
        bool anyPlaying = false;
        bool inputAddedToOutput = false;  // Track if we've added input for monitoring

        // Each layer manages its own fade independently based on when it was created
        // No need to set fadeActive globally - fade decay happens per-layer on loop wrap

        for (int i = 0; i <= highestLayer; ++i)
        {
            bool hasContent = layers[i].hasContent();
            bool isRecording = (layers[i].getState() == LoopBuffer::State::Recording);
            bool isOverdubbing = (layers[i].getState() == LoopBuffer::State::Overdubbing);
            bool isMuted = layers[i].getMuted();

            if (!hasContent && !isRecording)
                continue;

            // Skip muted layers (but still advance playhead so they stay in sync)
            if (isMuted)
            {
                // Use pre-allocated dummy buffer to advance the layer's state
                dummyBuffer.clear();
                layers[i].processBlock(dummyBuffer);
                // Don't mix muted layer into output
                continue;
            }

            // Only pass input to recording/overdubbing layers
            // Playback layers should only output their loop content (no input passthrough)
            if (isRecording || isOverdubbing)
            {
                // Recording/overdubbing layer gets the input - copy to layer buffer
                for (int ch = 0; ch < numChannels; ++ch)
                    layerBuffer.copyFrom(ch, 0, inputBuffer, ch, 0, numSamples);
                inputAddedToOutput = true;  // processBlock will add input to output for monitoring
            }
            else
            {
                // Playback layer gets silence as "input" - it will just output loop content
                layerBuffer.clear();
            }

            layers[i].processBlock(layerBuffer);

            // Mix into main output (loop + input combined)
            for (int ch = 0; ch < numChannels; ++ch)
            {
                buffer.addFrom(ch, 0, layerBuffer, ch, 0, numSamples);
            }

            // Also accumulate JUST the loop playback portion (without input) for separate processing
            // This allows effects like degrade to only affect loop playback, not input
            if (!isRecording && !isOverdubbing)
            {
                // Pure playback layer - add to loop-only buffer
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    loopOnlyBuffer.addFrom(ch, 0, layerBuffer, ch, 0, numSamples);
                }
            }
            else
            {
                // Recording/overdubbing: the layer buffer contains input + loop playback mixed
                // We need to subtract input to get just the loop portion for degrading
                // Actually, during recording the loop portion IS the input being written
                // During overdubbing, the layer outputs existing loop + input
                // For Blooper-style: we want to degrade existing loop content, not new input
                // So we read the existing content BEFORE it's mixed with input

                // For overdubbing, processBlock already outputs existingLoop + input
                // We need the existing loop part only. Let's get it by subtracting input.
                // Note: This is approximate since layerBuffer = existingLoop + input
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    const float* layerData = layerBuffer.getReadPointer(ch);
                    const float* inputData = inputBuffer.getReadPointer(ch);
                    float* loopOnlyData = loopOnlyBuffer.getWritePointer(ch);
                    for (int s = 0; s < numSamples; ++s)
                    {
                        // Subtract input to get just the loop portion
                        loopOnlyData[s] += layerData[s] - inputData[s];
                    }
                }
            }

            if (layers[i].getState() != LoopBuffer::State::Idle)
            {
                anyPlaying = true;
            }
        }

        // If nothing is playing/recording, pass through input
        // Also add input if we have playback but no recording (for monitoring during playback)
        if (!anyPlaying && highestLayer == 0 && !layers[0].hasContent())
        {
            buffer.makeCopyOf(inputBuffer);
            // No loop content, so loopOnlyBuffer stays empty (just input passthrough)
        }
        else if (anyPlaying && !inputAddedToOutput)
        {
            // Playback-only mode: add input for monitoring
            for (int ch = 0; ch < numChannels; ++ch)
            {
                buffer.addFrom(ch, 0, inputBuffer, ch, 0, numSamples);
            }
        }

        // Soft clip the mixed output
        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* channelData = buffer.getWritePointer(ch);
            for (int i = 0; i < numSamples; ++i)
            {
                channelData[i] = softClip(channelData[i]);
            }
        }

        // Copy separated buffers to output parameters if provided
        // This allows PluginProcessor to apply degrade only to loop playback
        if (loopPlaybackBuffer != nullptr)
        {
            if (loopPlaybackBuffer->getNumSamples() < numSamples || loopPlaybackBuffer->getNumChannels() < numChannels)
                loopPlaybackBuffer->setSize(numChannels, numSamples, false, false, true);
            for (int ch = 0; ch < numChannels; ++ch)
            {
                loopPlaybackBuffer->copyFrom(ch, 0, loopOnlyBuffer, ch, 0, numSamples);
            }
        }

        if (inputPassthroughBuffer != nullptr)
        {
            if (inputPassthroughBuffer->getNumSamples() < numSamples || inputPassthroughBuffer->getNumChannels() < numChannels)
                inputPassthroughBuffer->setSize(numChannels, numSamples, false, false, true);
            for (int ch = 0; ch < numChannels; ++ch)
            {
                inputPassthroughBuffer->copyFrom(ch, 0, inputBuffer, ch, 0, numSamples);
            }
        }
    }

    // State getters
    LoopBuffer::State getState() const
    {
        return getCurrentState();
    }

    int getCurrentLayer() const { return currentLayer + 1; }  // 1-indexed for UI
    int getHighestLayer() const { return highestLayer + 1; }  // 1-indexed for UI

    float getPlayheadPosition() const
    {
        // Return playhead from first layer with content
        for (int i = 0; i <= highestLayer; ++i)
        {
            if (layers[i].hasContent())
            {
                return layers[i].getPlayheadPosition();
            }
        }
        return 0.0f;
    }

    float getLoopLengthSeconds() const
    {
        if (masterLoopLength <= 0 || currentSampleRate <= 0)
            return 0.0f;
        return static_cast<float>(masterLoopLength) / static_cast<float>(currentSampleRate);
    }

    int getLoopLengthSamples() const
    {
        return masterLoopLength;
    }

    bool hasContent() const
    {
        return layers[0].hasContent();
    }

    bool getIsReversed() const
    {
        // Return the master reverse state
        return isReversed;
    }

    // Get combined waveform data for UI
    std::vector<float> getWaveformData(int numPoints) const
    {
        std::vector<float> combinedWaveform(numPoints, 0.0f);

        // Include all layers up to highestLayer (independent muting)
        for (int i = 0; i <= highestLayer; ++i)
        {
            // Skip muted layers in waveform display
            if (layers[i].getMuted())
                continue;

            bool isRecording = (layers[i].getState() == LoopBuffer::State::Recording);
            if (layers[i].hasContent() || isRecording)
            {
                auto layerWaveform = layers[i].getWaveformData(numPoints);
                for (size_t j = 0; j < static_cast<size_t>(numPoints); ++j)
                {
                    combinedWaveform[j] += layerWaveform[j];
                }
            }
        }

        // Normalize
        float maxVal = 0.0f;
        for (float val : combinedWaveform)
        {
            maxVal = std::max(maxVal, val);
        }
        if (maxVal > 0.0f)
        {
            for (float& val : combinedWaveform)
            {
                val /= maxVal;
            }
        }

        return combinedWaveform;
    }

    // Get per-layer waveform data for colored visualization
    // Preserves absolute fade levels - faded audio shows as smaller waveforms
    std::vector<std::vector<float>> getLayerWaveforms(int numPoints) const
    {
        std::vector<std::vector<float>> layerWaveforms;

        // Find the ORIGINAL (unfaded) max across all layers for consistent baseline scaling
        // We need to know what the max WOULD be at full volume to scale properly
        float originalMax = 0.0f;

        for (int i = 0; i <= highestLayer; ++i)
        {
            bool isRecording = (layers[i].getState() == LoopBuffer::State::Recording);
            if (layers[i].hasContent() || isRecording)
            {
                // Get the raw buffer max (before fade multiplier)
                float layerMax = layers[i].getBufferPeakLevel();
                originalMax = std::max(originalMax, layerMax);
            }
        }

        // Now collect waveforms (which have fade applied) and scale by original max
        // This way, a layer at 50% fade will show at 50% height relative to its original peak
        for (int i = 0; i <= highestLayer; ++i)
        {
            bool isRecording = (layers[i].getState() == LoopBuffer::State::Recording);
            if (layers[i].hasContent() || isRecording)
            {
                auto waveform = layers[i].getWaveformData(numPoints);

                // Scale by original max so fade effect is visible
                // If originalMax is 1.0 and fadeMultiplier is 0.5, waveform values will be ~0.5
                if (originalMax > 0.0f)
                {
                    for (float& val : waveform)
                    {
                        val /= originalMax;
                    }
                }

                layerWaveforms.push_back(waveform);
            }
            else
            {
                layerWaveforms.push_back(std::vector<float>(numPoints, 0.0f));
            }
        }

        return layerWaveforms;
    }

    // Check if a specific layer is muted (for UI sync)
    std::vector<bool> getLayerMuteStates() const
    {
        std::vector<bool> states;
        for (int i = 0; i <= highestLayer; ++i)
        {
            states.push_back(layers[i].getMuted());
        }
        return states;
    }

    // Set preset loop length (in bars, 0 = free/unlimited)
    void setLoopLengthBars(int bars)
    {
        presetLengthBars.store(bars);
        DBG("LoopEngine::setLoopLengthBars(" + juce::String(bars) + ")");
    }

    // Set additional beats (0-7, adds to bars)
    void setLoopLengthBeats(int beats)
    {
        presetLengthBeats.store(std::clamp(beats, 0, 7));
        DBG("LoopEngine::setLoopLengthBeats(" + juce::String(beats) + ")");
    }

    int getLoopLengthBars() const { return presetLengthBars.load(); }
    int getLoopLengthBeats() const { return presetLengthBeats.load(); }

    // Set host BPM for calculating bar lengths
    void setHostBpm(float bpm)
    {
        hostBpm.store(bpm);
    }

    // Input monitoring controls
    void setInputMuted(bool muted) { inputMuted.store(muted); }
    bool getInputMuted() const { return inputMuted.load(); }
    float getInputLevelL() const { return inputLevelL.load(); }
    float getInputLevelR() const { return inputLevelR.load(); }

    // Flatten all non-muted layers into layer 1
    // This sums all active layer buffers into layer 1 and clears the others
    // SEAMLESS: preserves playhead position and continues playback without interruption
    void flattenLayers()
    {
        if (masterLoopLength <= 0 || !layers[0].hasContent())
        {
            DBG("flattenLayers() - Nothing to flatten");
            return;
        }

        // Only flatten if there's more than one layer
        if (highestLayer == 0)
        {
            DBG("flattenLayers() - Only one layer, nothing to flatten");
            return;
        }

        DBG("flattenLayers() - Flattening " + juce::String(highestLayer + 1) + " layers into layer 1 (seamless)");

        // Capture current playback state BEFORE modifying anything
        const float savedPlayhead = layers[0].getRawPlayhead();
        const LoopBuffer::State savedState = layers[0].getState();
        const bool wasPlaying = (savedState == LoopBuffer::State::Playing ||
                                  savedState == LoopBuffer::State::Overdubbing);

        DBG("  Saved playhead: " + juce::String(savedPlayhead) + " state: " + juce::String(static_cast<int>(savedState)));

        // Create a temporary buffer to accumulate all layers
        const int numChannels = 2;  // Stereo
        juce::AudioBuffer<float> flattenedBuffer(numChannels, masterLoopLength);
        flattenedBuffer.clear();

        // Sum all non-muted layers
        for (int i = 0; i <= highestLayer; ++i)
        {
            if (!layers[i].getMuted() && layers[i].hasContent())
            {
                // Get this layer's buffer and add to flattened
                layers[i].addToBuffer(flattenedBuffer);
                DBG("  Added layer " + juce::String(i + 1));
            }
        }

        // Soft clip the summed buffer to prevent clipping
        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* data = flattenedBuffer.getWritePointer(ch);
            for (int s = 0; s < masterLoopLength; ++s)
            {
                data[s] = softClip(data[s]);
            }
        }

        // Clear layers 1+ (not layer 0 yet - we'll replace its buffer in place)
        for (int i = 1; i < NUM_LAYERS; ++i)
        {
            layers[i].clear();
        }

        // Replace layer 0's buffer content in-place while preserving playback state
        layers[0].setFromBufferSeamless(flattenedBuffer, masterLoopLength, savedPlayhead, savedState);

        // Reset state
        currentLayer = 0;
        highestLayer = 0;

        DBG("flattenLayers() - Complete, layer 1 now contains flattened audio, playback continues at " + juce::String(savedPlayhead));
    }

    // Get target loop length in samples (0 = unlimited)
    int getTargetLoopLengthSamples() const
    {
        int bars = presetLengthBars.load();
        int beats = presetLengthBeats.load();

        // If both are 0, it's free mode
        if (bars <= 0 && beats <= 0)
            return 0;  // Free mode - no limit

        float bpm = hostBpm.load();
        if (bpm <= 0.0f)
            bpm = 120.0f;  // Default fallback

        // Calculate total beats: bars * 4 + additional beats
        int totalBeats = (bars * 4) + beats;

        // samples per beat = sampleRate * 60 / BPM
        double samplesPerBeat = currentSampleRate * 60.0 / static_cast<double>(bpm);
        return static_cast<int>(samplesPerBeat * totalBeats);
    }

private:
    std::array<LoopBuffer, NUM_LAYERS> layers;
    int currentLayer = 0;
    int highestLayer = 0;
    int masterLoopLength = 0;
    double currentSampleRate = 44100.0;
    bool isReversed = false;  // Master reverse state
    std::atomic<int> presetLengthBars { 0 };  // 0 = free, 1-16 = bars
    std::atomic<int> presetLengthBeats { 0 }; // 0-7 additional beats
    std::atomic<float> hostBpm { 120.0f };

    // Input monitoring
    std::atomic<float> inputLevelL { 0.0f };
    std::atomic<float> inputLevelR { 0.0f };
    std::atomic<bool> inputMuted { false };

    // Pre-allocated buffers to avoid allocation in processBlock
    juce::AudioBuffer<float> inputBuffer;
    juce::AudioBuffer<float> layerBuffer;
    juce::AudioBuffer<float> dummyBuffer;
    juce::AudioBuffer<float> loopOnlyBuffer;  // Loop playback only, no input (for Blooper-style effects)

    LoopBuffer::State getCurrentState() const
    {
        // Return the most "active" state across all layers
        for (int i = 0; i <= highestLayer; ++i)
        {
            LoopBuffer::State layerState = layers[i].getState();
            if (layerState == LoopBuffer::State::Recording)
                return LoopBuffer::State::Recording;
            if (layerState == LoopBuffer::State::Overdubbing)
                return LoopBuffer::State::Overdubbing;
        }

        for (int i = 0; i <= highestLayer; ++i)
        {
            if (layers[i].getState() == LoopBuffer::State::Playing)
                return LoopBuffer::State::Playing;
        }

        return LoopBuffer::State::Idle;
    }

    static float softClip(float x)
    {
        if (x > 1.0f)
            return 1.0f - std::exp(-(x - 1.0f));
        else if (x < -1.0f)
            return -1.0f + std::exp(-(-x - 1.0f));
        return x;
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LoopEngine)
};
