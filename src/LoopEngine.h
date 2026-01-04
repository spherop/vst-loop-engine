#pragma once

#include "LoopBuffer.h"
#include <array>
#include <atomic>
#include <cmath>

class LoopEngine
{
private:
    // Safe sample processing - prevents NaN/Inf and applies soft clipping
    static inline float safeSample(float sample)
    {
        // Check for NaN or Inf - reset to 0
        if (std::isnan(sample) || std::isinf(sample))
            return 0.0f;

        // Soft clip using tanh-style curve, transparent below 0.9
        if (std::abs(sample) > 0.9f)
        {
            return std::tanh(sample * 1.5f) * 0.95f;
        }
        return sample;
    }


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

        // Initialize smear buffers
        smearBufferL.resize(SMEAR_BUFFER_SIZE, 0.0f);
        smearBufferR.resize(SMEAR_BUFFER_SIZE, 0.0f);
        smearWritePos = 0;
        smearCaptureLength = 0;
        smearActive = false;

        // Initialize input mute gain smoother (15ms fade for click-free muting)
        inputMuteGainSmoothed.reset(sampleRate, 0.015);
        inputMuteGainSmoothed.setCurrentAndTargetValue(inputMuted.load() ? 0.0f : 1.0f);

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
            layers[i].setReverse(isReversed);  // Reset to current global reverse state
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

        // If additive capture is active, stop it (PLAY stops DUB in ADD+ mode)
        if (additiveRecordingActive.load())
        {
            DBG("play() - Stopping ADD+ capture");
            stopAdditiveCapture();
        }

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

        // Stop additive recording if active
        if (additiveRecordingActive.load())
        {
            stopAdditiveCapture();
        }

        DBG("LoopEngine::stop() - All layers stopped, additive recording stopped");
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
            " hasContent=" + juce::String(layers[0].hasContent() ? "true" : "false") +
            " additiveModeEnabled=" + juce::String(additiveModeEnabled.load() ? "true" : "false"));

        // Ensure masterLoopLength is set if layer 0 has content
        // This handles the case where fixed-length recording auto-stopped
        // (auto-stop bypasses LoopEngine::stopRecording so masterLoopLength wasn't set)
        if (masterLoopLength == 0 && layers[0].hasContent())
        {
            masterLoopLength = layers[0].getLoopLengthSamples();
            highestLayer = std::max(highestLayer, 0);
            DBG("overdub() - Fixed masterLoopLength from layer 0: " + juce::String(masterLoopLength));
        }

        // If ADD+ mode is enabled, start additive capture instead of normal overdub
        // Additive mode captures the effected output and creates override layers
        if (additiveModeEnabled.load() && hasContent())
        {
            if (!additiveRecordingActive.load())
            {
                startAdditiveCapture();
                DBG("overdub() - Started ADD+ capture mode");
            }
            // Don't do normal overdub when in ADD+ mode - we're just capturing
            return;
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
            // LAYER MODE: DUB while recording just stops recording (like PLAY)
            // User must press DUB again while playing to create a new layer
            // This prevents accidental layer creation and matches Blooper behavior
            if (layerModeEnabled.load())
            {
                DBG("LAYER MODE: Stopping overdub on layer " + juce::String(currentLayer) + " (DUB = stop, not new layer)");
                layers[currentLayer].stopOverdub();
                // Don't create new layer - just continue playback
                return;
            }

            // TRACK MODE: pressing DUB while overdubbing creates a NEW layer immediately
            // Stop current layer's overdub immediately (no fade needed - new layer takes over)
            DBG("TRACK MODE: Stopping overdub on layer " + juce::String(currentLayer) + " to create new layer");
            layers[currentLayer].stopOverdubImmediate();

            // Create new layer and start overdubbing
            if (highestLayer < NUM_LAYERS - 1)
            {
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
                DBG("Cannot create new layer - max layers reached, just stopping overdub");
            }
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
            // If current layer is actively recording/overdubbing, stop it first
            // This is critical for proper undo behavior - we can't just mute while recording
            LoopBuffer::State layerState = layers[currentLayer].getState();
            if (layerState == LoopBuffer::State::Overdubbing)
            {
                DBG("undo() - stopping overdub on layer " + juce::String(currentLayer) + " before undo");
                layers[currentLayer].stopOverdub();
            }
            else if (layerState == LoopBuffer::State::Recording)
            {
                DBG("undo() - stopping recording on layer " + juce::String(currentLayer) + " before undo");
                layers[currentLayer].stopRecording(false);
            }

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

    // Per-layer solo (1-indexed for UI)
    // When any layer is soloed, only soloed layers play
    void setLayerSoloed(int layer, bool soloed)
    {
        int idx = layer - 1;
        if (idx >= 0 && idx < NUM_LAYERS)
        {
            bool wasSoloed = layers[idx].getSoloed();
            layers[idx].setSoloed(soloed);

            // Update solo count
            if (soloed && !wasSoloed)
                soloCount.fetch_add(1);
            else if (!soloed && wasSoloed)
                soloCount.fetch_sub(1);

            DBG("Layer " + juce::String(layer) + " soloed: " + juce::String(soloed ? "true" : "false") +
                " (total soloed: " + juce::String(soloCount.load()) + ")");
        }
    }

    bool getLayerSoloed(int layer) const
    {
        int idx = layer - 1;
        if (idx >= 0 && idx < NUM_LAYERS)
        {
            return layers[idx].getSoloed();
        }
        return false;
    }

    // Check if any layer is soloed
    bool hasAnySoloed() const { return soloCount.load() > 0; }

    // Get solo states for all layers (for UI sync)
    std::vector<bool> getLayerSoloStates() const
    {
        std::vector<bool> states;
        states.reserve(NUM_LAYERS);
        for (int i = 0; i < NUM_LAYERS; ++i)
            states.push_back(layers[i].getSoloed());
        return states;
    }

    // Per-layer volume (1-indexed for UI)
    void setLayerVolume(int layer, float vol)
    {
        int idx = layer - 1;
        if (idx >= 0 && idx < NUM_LAYERS)
        {
            layers[idx].setVolume(vol);
        }
    }

    float getLayerVolume(int layer) const
    {
        int idx = layer - 1;
        if (idx >= 0 && idx < NUM_LAYERS)
        {
            return layers[idx].getVolume();
        }
        return 1.0f;
    }

    // Per-layer pan (1-indexed for UI)
    void setLayerPan(int layer, float p)
    {
        int idx = layer - 1;
        if (idx >= 0 && idx < NUM_LAYERS)
        {
            layers[idx].setPan(p);
        }
    }

    float getLayerPan(int layer) const
    {
        int idx = layer - 1;
        if (idx >= 0 && idx < NUM_LAYERS)
        {
            return layers[idx].getPan();
        }
        return 0.0f;
    }

    // ============================================
    // PER-LAYER EQ (1-indexed for UI)
    // ============================================

    void setLayerEQLow(int layer, float gainDB)
    {
        int idx = layer - 1;
        if (idx >= 0 && idx < NUM_LAYERS)
        {
            layers[idx].setEQLow(gainDB);
        }
    }

    void setLayerEQMid(int layer, float gainDB)
    {
        int idx = layer - 1;
        if (idx >= 0 && idx < NUM_LAYERS)
        {
            layers[idx].setEQMid(gainDB);
        }
    }

    void setLayerEQHigh(int layer, float gainDB)
    {
        int idx = layer - 1;
        if (idx >= 0 && idx < NUM_LAYERS)
        {
            layers[idx].setEQHigh(gainDB);
        }
    }

    float getLayerEQLowDB(int layer) const
    {
        int idx = layer - 1;
        if (idx >= 0 && idx < NUM_LAYERS)
        {
            return layers[idx].getEQLowDB();
        }
        return 0.0f;
    }

    float getLayerEQMidDB(int layer) const
    {
        int idx = layer - 1;
        if (idx >= 0 && idx < NUM_LAYERS)
        {
            return layers[idx].getEQMidDB();
        }
        return 0.0f;
    }

    float getLayerEQHighDB(int layer) const
    {
        int idx = layer - 1;
        if (idx >= 0 && idx < NUM_LAYERS)
        {
            return layers[idx].getEQHighDB();
        }
        return 0.0f;
    }

    // ============================================
    // PER-LAYER LOOP BOUNDARIES (1-indexed for UI)
    // ============================================

    void setLayerLoopStart(int layer, float normalizedPos)
    {
        int idx = layer - 1;
        if (idx >= 0 && idx < NUM_LAYERS)
        {
            layers[idx].setLoopStart(normalizedPos);
        }
    }

    void setLayerLoopEnd(int layer, float normalizedPos)
    {
        int idx = layer - 1;
        if (idx >= 0 && idx < NUM_LAYERS)
        {
            layers[idx].setLoopEnd(normalizedPos);
        }
    }

    float getLayerLoopStart(int layer) const
    {
        int idx = layer - 1;
        if (idx >= 0 && idx < NUM_LAYERS)
        {
            return layers[idx].getLoopStartNormalized();
        }
        return 0.0f;
    }

    float getLayerLoopEnd(int layer) const
    {
        int idx = layer - 1;
        if (idx >= 0 && idx < NUM_LAYERS)
        {
            return layers[idx].getLoopEndNormalized();
        }
        return 1.0f;
    }

    // ============================================
    // PER-LAYER REVERSE (1-indexed for UI)
    // ============================================

    void setLayerReverse(int layer, bool reversed)
    {
        int idx = layer - 1;
        if (idx >= 0 && idx < NUM_LAYERS)
        {
            layers[idx].setReverse(reversed);
            DBG("Layer " + juce::String(layer) + " reverse: " + juce::String(reversed ? "true" : "false"));
        }
    }

    bool getLayerReverse(int layer) const
    {
        int idx = layer - 1;
        if (idx >= 0 && idx < NUM_LAYERS)
        {
            return layers[idx].getIsReversed();
        }
        return false;
    }

    // ============================================
    // PER-LAYER PITCH SHIFT (1-indexed for UI)
    // Independent of global pitch - allows per-layer tuning
    // ============================================

    void setLayerPitch(int layer, float semitones)
    {
        int idx = layer - 1;
        if (idx >= 0 && idx < NUM_LAYERS)
        {
            layers[idx].setLayerPitch(semitones);
        }
    }

    float getLayerPitch(int layer) const
    {
        int idx = layer - 1;
        if (idx >= 0 && idx < NUM_LAYERS)
        {
            return layers[idx].getLayerPitch();
        }
        return 0.0f;
    }

    void setLayerPitchHQ(int layer, bool hq)
    {
        int idx = layer - 1;
        if (idx >= 0 && idx < NUM_LAYERS)
        {
            layers[idx].setLayerPitchHQ(hq);
        }
    }

    bool getLayerPitchHQ(int layer) const
    {
        int idx = layer - 1;
        if (idx >= 0 && idx < NUM_LAYERS)
        {
            return layers[idx].getLayerPitchHQ();
        }
        return false;
    }

    // Parameters (apply to all layers when global value changes)
    // These are called every processBlock from APVTS parameters, so we must
    // guard against overwriting per-layer settings by only applying when changed
    void setLoopStart(float normalizedPos)
    {
        // Only apply if the global loop start actually changed
        // This prevents overwriting per-layer settings every block
        if (std::abs(globalLoopStart - normalizedPos) < 0.0001f)
            return;

        globalLoopStart = normalizedPos;

        for (int i = 0; i < NUM_LAYERS; ++i)
        {
            layers[i].setLoopStart(normalizedPos);
        }
        DBG("Global loop start changed to: " + juce::String(normalizedPos, 3));
    }

    void setLoopEnd(float normalizedPos)
    {
        // Only apply if the global loop end actually changed
        // This prevents overwriting per-layer settings every block
        if (std::abs(globalLoopEnd - normalizedPos) < 0.0001f)
            return;

        globalLoopEnd = normalizedPos;

        for (int i = 0; i < NUM_LAYERS; ++i)
        {
            layers[i].setLoopEnd(normalizedPos);
        }
        DBG("Global loop end changed to: " + juce::String(normalizedPos, 3));
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
        // Only apply if the global reverse state actually changed
        // This prevents the global param from overwriting per-layer reverse settings every block
        if (isReversed == reversed)
            return;

        // Store the master reverse state
        isReversed = reversed;

        // Apply to all layers (including ones that might be recorded later)
        for (int i = 0; i < NUM_LAYERS; ++i)
        {
            layers[i].setReverse(reversed);
        }

        DBG("Global reverse changed to: " + juce::String(reversed ? "true" : "false"));
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

        // Apply smoothed input mute for click-free transitions
        bool isInputMuted = inputMuted.load();
        bool isInputMuteTransitioning = inputMuteGainSmoothed.isSmoothing();
        if (isInputMuted || isInputMuteTransitioning)
        {
            // Get pointers for all channels
            float* channelData[2] = { nullptr, nullptr };
            for (int ch = 0; ch < numChannels && ch < 2; ++ch)
                channelData[ch] = inputBuffer.getWritePointer(ch);

            // Apply gain per-sample (smoother advances once per sample, applied to all channels)
            for (int s = 0; s < numSamples; ++s)
            {
                float muteGain = inputMuteGainSmoothed.getNextValue();
                for (int ch = 0; ch < numChannels && ch < 2; ++ch)
                {
                    if (channelData[ch])
                        channelData[ch][s] *= muteGain;
                }
            }
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

        // FIX: Ensure masterLoopLength is set when layer 0 has content
        // This handles the case where LoopBuffer auto-stops recording (fixed-length mode)
        // and bypasses LoopEngine::stopRecording, leaving masterLoopLength unset
        if (masterLoopLength == 0 && layers[0].hasContent())
        {
            masterLoopLength = layers[0].getLoopLengthSamples();
            highestLayer = std::max(highestLayer, 0);
            DBG("processBlock() - Fixed masterLoopLength from layer 0: " + juce::String(masterLoopLength));
        }

        // Override layer skip logic (v7.1.0)
        // Override layers are "mixdown" layers - they contain the full mix
        // and by definition mute all Regular layers below them.
        //
        // Key fix: Only skip layers if the override layer is PLAYING (has content and not idle)
        // If we're still building the override layer, don't skip anything yet.
        int highestOverride = -1;
        for (int i = highestLayer; i >= 0; --i)
        {
            if (layers[i].hasContent() &&
                layers[i].isOverrideLayer() &&
                !layers[i].getMuted() &&
                layers[i].getState() != LoopBuffer::State::Idle)
            {
                highestOverride = i;
                break;
            }
        }

        // Layer mode with recording: accumulate playback from all layers below the recording layer
        // This buffer will contain the "bounce" audio (all previous layers mixed)
        // IMPORTANT: Only bounce for exactly ONE LOOP PASS to prevent volume buildup
        const bool layerMode = layerModeEnabled.load();
        juce::AudioBuffer<float> bounceBuffer;
        int recordingLayerIndex = -1;
        bool shouldBounce = false;  // Whether to add bounce audio this block

        if (layerMode)
        {
            // Find recording layer
            for (int i = 0; i <= highestLayer; ++i)
            {
                if (layers[i].getState() == LoopBuffer::State::Recording ||
                    layers[i].getState() == LoopBuffer::State::Overdubbing)
                {
                    recordingLayerIndex = i;
                    break;
                }
            }

            // If recording in Layer mode on layer > 0, manage bounce state
            if (recordingLayerIndex > 0)
            {
                // Check if this is a NEW recording session (different layer or wasn't recording before)
                if (layerModeBounceLayer != recordingLayerIndex)
                {
                    // New recording session - initialize bounce tracking
                    layerModeBounceLayer = recordingLayerIndex;
                    layerModeBounceStartSample = static_cast<int>(layers[0].getRawPlayhead());
                    layerModeBounceProgress = 0;
                    layerModeBounceComplete = false;
                    DBG("Layer mode bounce START: layer=" + juce::String(recordingLayerIndex) +
                        " startSample=" + juce::String(layerModeBounceStartSample) +
                        " loopLength=" + juce::String(masterLoopLength));
                }

                // Check if we've completed one full loop pass
                if (!layerModeBounceComplete && masterLoopLength > 0)
                {
                    layerModeBounceProgress += numSamples;

                    if (layerModeBounceProgress >= masterLoopLength)
                    {
                        // One full loop complete - stop bouncing
                        layerModeBounceComplete = true;
                        DBG("Layer mode bounce COMPLETE: recorded " + juce::String(layerModeBounceProgress) +
                            " samples, bounce now OFF");
                    }
                }

                // Only bounce if we haven't completed one full loop pass
                shouldBounce = !layerModeBounceComplete;

                if (shouldBounce)
                {
                    bounceBuffer.setSize(numChannels, numSamples, false, false, true);
                    bounceBuffer.clear();

                    // In Layer Mode, only bounce from the HIGHEST unmuted layer below recording layer
                    // This is because each layer already contains ALL prior layers' content baked in
                    // (from when that layer was recorded). Bouncing multiple layers would cause
                    // volume buildup as early layers' content gets included multiple times.
                    //
                    // Find the highest unmuted layer below recording layer
                    int bounceFromLayer = -1;
                    for (int i = recordingLayerIndex - 1; i >= 0; --i)
                    {
                        if (!layers[i].hasContent() || layers[i].getMuted())
                            continue;
                        // Skip if below an override layer (override layers supersede all below)
                        if (highestOverride >= 0 && i < highestOverride && !layers[i].isOverrideLayer())
                            continue;
                        bounceFromLayer = i;
                        break;  // Found highest valid layer
                    }

                    if (bounceFromLayer >= 0)
                    {
                        // Get this layer's RAW playback (no fade/volume/pan)
                        // We want to record the actual buffer content, not attenuated output
                        juce::AudioBuffer<float> tempLayer(numChannels, numSamples);
                        tempLayer.clear();
                        layers[bounceFromLayer].peekPlaybackRaw(tempLayer);

                        // Copy to bounce buffer (only one layer, so copy not add)
                        for (int ch = 0; ch < numChannels; ++ch)
                            bounceBuffer.copyFrom(ch, 0, tempLayer, ch, 0, numSamples);
                    }
                }
            }
            else if (recordingLayerIndex < 0)
            {
                // No recording happening - reset bounce state
                if (layerModeBounceLayer >= 0)
                {
                    DBG("Layer mode bounce RESET: recording stopped");
                    layerModeBounceLayer = -1;
                    layerModeBounceStartSample = -1;
                    layerModeBounceProgress = 0;
                    layerModeBounceComplete = false;
                }
            }
        }
        else
        {
            // Layer mode disabled - reset bounce state
            if (layerModeBounceLayer >= 0)
            {
                layerModeBounceLayer = -1;
                layerModeBounceStartSample = -1;
                layerModeBounceProgress = 0;
                layerModeBounceComplete = false;
            }
        }

        // Check if solo is active (any layer is soloed)
        bool anySoloActive = soloCount.load() > 0;

        for (int i = 0; i <= highestLayer; ++i)
        {
            bool hasContent = layers[i].hasContent();
            bool isRecording = (layers[i].getState() == LoopBuffer::State::Recording);
            bool isOverdubbing = (layers[i].getState() == LoopBuffer::State::Overdubbing);
            bool isMutedState = layers[i].getMuted();
            bool isMuteTransitioning = layers[i].isMuteTransitioning();
            bool isSoloed = layers[i].getSoloed();

            // Solo logic: if any layer is soloed, treat non-soloed layers as muted
            // (Recording/overdubbing layers always play for monitoring)
            bool effectivelyMuted = isMutedState || (anySoloActive && !isSoloed && !isRecording && !isOverdubbing);

            if (!hasContent && !isRecording)
                continue;

            // Check if layer is fully muted and not transitioning (can skip for efficiency)
            // But if transitioning, we need to process to get the smooth fade
            if (effectivelyMuted && !isMuteTransitioning)
            {
                // Fully muted and stable - advance playhead but skip processing
                dummyBuffer.clear();
                layers[i].processBlock(dummyBuffer);
                // Consume the mute gain value to keep smoother in sync
                layers[i].getMuteGain();
                continue;
            }

            // Override layer skip: if there's an override layer above this one,
            // skip all Regular layers below it (they're "muted" by the mixdown)
            // Only skip Regular layers - override layers always play
            if (highestOverride >= 0 && i < highestOverride && !layers[i].isOverrideLayer())
            {
                // This Regular layer is below an override layer - skip it
                // Still advance playhead to stay in sync
                dummyBuffer.clear();
                layers[i].processBlock(dummyBuffer);
                // Consume the mute gain value to keep smoother in sync
                layers[i].getMuteGain();
                continue;
            }

            // LAYER MODE BOUNCE: Prior layers being bounced should NOT be added to output
            // individually - they'll be heard via the bounce buffer added to the output once.
            // This prevents volume doubling where prior audio is heard twice.
            bool skipOutputMixing = false;
            if (layerMode && shouldBounce && recordingLayerIndex > 0 && i < recordingLayerIndex)
            {
                // This layer is being bounced - skip individual output mixing
                // The bounce buffer will be added to output once after all layers are processed
                skipOutputMixing = true;
            }

            // Only pass input to recording/overdubbing layers
            // Playback layers should only output their loop content (no input passthrough)
            if (isRecording || isOverdubbing)
            {
                // Recording/overdubbing layer gets the input
                // In Layer mode: also include bounced audio from all previous layers (for first loop pass only)
                for (int ch = 0; ch < numChannels; ++ch)
                    layerBuffer.copyFrom(ch, 0, inputBuffer, ch, 0, numSamples);

                // Layer mode bounce: add previous layers' playback to what we're recording
                // ONLY for the first loop pass - after that, just record input to prevent volume buildup
                // NOTE: The bounce is recorded INTO the layer, but for MONITORING during recording
                // we need to hear it. Since the recording layer's processBlock only outputs what
                // it has recorded so far (which starts empty), we add bounceBuffer to OUTPUT below.
                if (layerMode && shouldBounce && recordingLayerIndex > 0 && i == recordingLayerIndex)
                {
                    for (int ch = 0; ch < numChannels; ++ch)
                        layerBuffer.addFrom(ch, 0, bounceBuffer, ch, 0, numSamples);
                }

                inputAddedToOutput = true;  // processBlock will add input to output for monitoring
            }
            else
            {
                // Playback layer gets silence as "input" - it will just output loop content
                layerBuffer.clear();
            }

            layers[i].processBlock(layerBuffer);

            // Apply smoothed mute gain for click-free muting
            // This must be done per-sample to get the smooth transition
            if (isMuteTransitioning || isMutedState)
            {
                // Get pointers for all channels
                float* channelData[2] = { nullptr, nullptr };
                for (int ch = 0; ch < numChannels && ch < 2; ++ch)
                    channelData[ch] = layerBuffer.getWritePointer(ch);

                // Apply gain per-sample (smoother advances once per sample, applied to all channels)
                for (int s = 0; s < numSamples; ++s)
                {
                    float muteGain = layers[i].getMuteGain();
                    for (int ch = 0; ch < numChannels && ch < 2; ++ch)
                    {
                        if (channelData[ch])
                            channelData[ch][s] *= muteGain;
                    }
                }
            }

            // Check per-layer peak levels for diagnostic metering and VU meters
            int layerClips = 0;
            float layerPeak = 0.0f;
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float* data = layerBuffer.getReadPointer(ch);
                for (int s = 0; s < numSamples; ++s)
                {
                    float absVal = std::abs(data[s]);
                    if (absVal > layerPeak)
                        layerPeak = absVal;
                    if (absVal > 1.0f)
                        layerClips++;
                }
            }
            if (layerClips > 0)
                layerClipCounts[i].fetch_add(layerClips);

            // Update layer peak level with decay (for VU meter smoothing)
            // This is called once per audio block, not per sample
            // At 44.1kHz with 512 sample blocks, we get ~86 blocks/sec
            // Decay of 0.9 per block = ~300ms time constant (reaches 10% after ~20 blocks)
            float currentPeak = layerPeakLevels[i].load();
            if (layerPeak > currentPeak) {
                layerPeakLevels[i].store(layerPeak);  // Attack: immediate
            } else {
                // Decay per block: 0.9 = fast meter response, 0.95 = slower/smoother
                layerPeakLevels[i].store(currentPeak * 0.92f);
            }

            // Mix into main output (loop + input combined)
            // Track mode: sum all layers (additive mixing)
            // Layer mode: higher layers completely override lower layers DURING PLAYBACK
            //
            // SKIP OUTPUT MIXING for layers being bounced - they're already included
            // in the recording layer's output via the bounce buffer
            if (!skipOutputMixing)
            {
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    if (!layerMode)
                    {
                        // Track mode: simple additive mixing
                        buffer.addFrom(ch, 0, layerBuffer, ch, 0, numSamples);
                    }
                    else
                    {
                        // Layer mode: ALL layers use copyFrom to override prior layers
                        // This includes recording/overdubbing layers - they contain the full mix
                        // (bounce + input + existing content) and should completely replace
                        // any prior layer content in the output buffer.
                        //
                        // During bounce: prior layers are skipped via skipOutputMixing
                        // After bounce complete: recording layer has the bounced content baked in,
                        // so it should override (not add to) prior layers' output.
                        buffer.copyFrom(ch, 0, layerBuffer, ch, 0, numSamples);
                    }
                }
            }

            // Also accumulate JUST the loop playback portion (without input) for separate processing
            // This allows effects like degrade to only affect loop playback, not input
            // Also skip for bounced layers to prevent double-counting in effects
            if (!skipOutputMixing)
            {
                if (!isRecording && !isOverdubbing)
                {
                    // Pure playback layer - apply same mixing logic as main buffer
                    for (int ch = 0; ch < numChannels; ++ch)
                    {
                        if (!layerMode)
                        {
                            loopOnlyBuffer.addFrom(ch, 0, layerBuffer, ch, 0, numSamples);
                        }
                        else
                        {
                            // Layer mode PLAYBACK: complete override
                            loopOnlyBuffer.copyFrom(ch, 0, layerBuffer, ch, 0, numSamples);
                        }
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
                    //
                    // IMPORTANT for Layer mode bounce: layerBuffer = existingLoop + (input + bounce)
                    // We must subtract BOTH input AND bounce to get just existing loop content
                    // Otherwise the bounce audio (prior layers) leaks into loopOnlyBuffer and gets
                    // re-added in PluginProcessor, causing volume doubling at recording start
                    for (int ch = 0; ch < numChannels; ++ch)
                    {
                        const float* layerData = layerBuffer.getReadPointer(ch);
                        const float* inputData = inputBuffer.getReadPointer(ch);
                        float* loopOnlyData = loopOnlyBuffer.getWritePointer(ch);
                        for (int s = 0; s < numSamples; ++s)
                        {
                            float loopPortion = layerData[s] - inputData[s];

                            // In Layer mode with bounce, also subtract the bounce audio
                            // to prevent it from appearing in loopOnlyBuffer
                            if (layerMode && shouldBounce && i == recordingLayerIndex && bounceBuffer.getNumSamples() > 0)
                            {
                                loopPortion -= bounceBuffer.getSample(ch, s);
                            }

                            // Layer mode: override (recording layer has full content baked in)
                            // Track mode: additive (sum all layers)
                            if (layerMode)
                                loopOnlyData[s] = loopPortion;
                            else
                                loopOnlyData[s] += loopPortion;
                        }
                    }
                }
            }

            if (layers[i].getState() != LoopBuffer::State::Idle)
            {
                anyPlaying = true;
            }
        }

        // LAYER MODE BOUNCE: Add bounce to loopOnlyBuffer for effects processing only
        // The main buffer already has bounce via the recording layer's output (input+bounce),
        // so we do NOT add bounceBuffer to buffer again - that would cause double summing.
        // We only need bounce in loopOnlyBuffer so effects (degrade, reverb) apply to prior layers' audio.
        if (layerMode && shouldBounce && bounceBuffer.getNumSamples() > 0)
        {
            // Only add to loopOnlyBuffer for effects processing
            for (int ch = 0; ch < numChannels; ++ch)
            {
                loopOnlyBuffer.addFrom(ch, 0, bounceBuffer, ch, 0, numSamples);
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

        // Measure pre-clip peak levels and count clip events for diagnostics
        float peakPreClipL = 0.0f;
        float peakPreClipR = 0.0f;
        int clipsThisBlock = 0;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float* channelData = buffer.getReadPointer(ch);
            float& peak = (ch == 0) ? peakPreClipL : peakPreClipR;
            for (int i = 0; i < numSamples; ++i)
            {
                float absVal = std::abs(channelData[i]);
                if (absVal > peak) peak = absVal;
                if (absVal > 1.0f) clipsThisBlock++;
            }
        }

        // Update atomic peak meters (fast attack, slow release)
        float curPreClipL = preClipPeakL.load();
        float curPreClipR = preClipPeakR.load();
        preClipPeakL.store(peakPreClipL > curPreClipL ? peakPreClipL : curPreClipL * 0.99f);
        preClipPeakR.store(peakPreClipR > curPreClipR ? peakPreClipR : curPreClipR * 0.99f);

        if (clipsThisBlock > 0)
            clipEventCount.fetch_add(clipsThisBlock);

        // Also measure loop-only peak levels
        float peakLoopL = 0.0f;
        float peakLoopR = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float* data = loopOnlyBuffer.getReadPointer(ch);
            float& peak = (ch == 0) ? peakLoopL : peakLoopR;
            for (int i = 0; i < numSamples; ++i)
            {
                float absVal = std::abs(data[i]);
                if (absVal > peak) peak = absVal;
            }
        }
        float curLoopL = loopOutputPeakL.load();
        float curLoopR = loopOutputPeakR.load();
        loopOutputPeakL.store(peakLoopL > curLoopL ? peakLoopL : curLoopL * 0.99f);
        loopOutputPeakR.store(peakLoopR > curLoopR ? peakLoopR : curLoopR * 0.99f);

        // Soft clip the mixed output
        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* channelData = buffer.getWritePointer(ch);
            for (int i = 0; i < numSamples; ++i)
            {
                channelData[i] = softClip(channelData[i]);
            }
        }

        // Apply anti-click ducking at loop boundaries
        // BEFORE the boundary: fade OUT as we approach
        // AFTER the boundary: fade IN as we move away
        // Uses dynamic parameters from UI settings

        // DEBUG: Log BEFORE the condition to see what values we have
        xfadeDebugCounter++;
        if (xfadeDebugCounter >= 500)  // Log every ~500 blocks (~10ms)
        {
            xfadeDebugCounter = 0;
            DBG("*** XFADE CHECK: loopLen=" + juce::String(masterLoopLength) +
                " content=" + juce::String(layers[0].hasContent() ? 1 : 0) +
                " state=" + juce::String(static_cast<int>(layers[0].getState())) +
                " highest=" + juce::String(highestLayer));
        }

        if (masterLoopLength > 0 && layers[0].hasContent())
        {
            // Load current crossfade parameters (LP filter + volume ducking)
            const int preTimeMs = xfadePreTimeMs.load();
            const int postTimeMs = xfadePostTimeMs.load();
            const float filterFreq = xfadeFilterFreq.load();  // LP filter cutoff (Hz)
            const float filterMix = xfadeFilterDepth.load();  // Filter wet mix (0-1)
            const float volDepth = xfadeVolDepth.load();      // Volume duck depth (0-1)

            // Convert pre-time to position threshold (normalized 0-1)
            // threshold = preTimeMs / loopLengthMs
            const float loopLengthMs = static_cast<float>(masterLoopLength) / static_cast<float>(currentSampleRate) * 1000.0f;
            const float preThreshold = std::min(0.5f, static_cast<float>(preTimeMs) / loopLengthMs);

            // Convert post-time to samples
            const int postSamples = static_cast<int>(static_cast<float>(postTimeMs) * static_cast<float>(currentSampleRate) / 1000.0f);

            float currentMasterPos = layers[0].getPlayheadPosition();

            // Detect loop boundary crossing
            bool loopWrapped = false;
            float posDelta = currentMasterPos - lastMasterPlayheadPos;

            if (!isReversed)
            {
                loopWrapped = (posDelta < -0.5f);
            }
            else
            {
                loopWrapped = (posDelta > 0.5f);
            }

            if (loopWrapped)
            {
                antiClickCountdown = postSamples;
                approachingBoundary = false;
                // Snapshot the smear write position for playback
                // This is where we stopped capturing, so we read backwards from here
                smearPlaybackStart = smearWritePos;
                smearActive = true;

                // Handle additive recording reprint at loop boundary
                handleLoopBoundaryForAdditive();
            }

            // Check if we're APPROACHING the boundary (pre-boundary zone)
            bool inPreBoundaryZone = false;
            float distanceFromBoundary = 0.0f;

            if (!isReversed)
            {
                if (currentMasterPos > (1.0f - preThreshold))
                {
                    inPreBoundaryZone = true;
                    distanceFromBoundary = 1.0f - currentMasterPos;
                }
            }
            else
            {
                if (currentMasterPos < preThreshold)
                {
                    inPreBoundaryZone = true;
                    distanceFromBoundary = currentMasterPos;
                }
            }

            lastMasterPlayheadPos = currentMasterPos;

            // DEBUG: Log position periodically
            static int debugCounter = 0;
            if (++debugCounter >= 1000)  // Log every ~1000 blocks
            {
                debugCounter = 0;
                DBG("XFADE DEBUG: pos=" + juce::String(currentMasterPos, 3) +
                    " preThreshold=" + juce::String(preThreshold, 3) +
                    " inPre=" + juce::String(inPreBoundaryZone ? 1 : 0) +
                    " countdown=" + juce::String(antiClickCountdown) +
                    " filterFreq=" + juce::String(filterFreq, 0) +
                    " filterMix=" + juce::String(filterMix, 2));
            }

            // Calculate effect strength based on where we are
            float effectStrength = 0.0f;  // 0 = no effect, 1 = full effect at boundary
            const float smearAmount = xfadeSmearAmount.load();

            // FIRST: Capture clean audio for smear BEFORE any effects are applied
            // We capture throughout the pre-boundary zone so we have clean audio to blend
            if (inPreBoundaryZone && smearAmount > 0.0f)
            {
                const float* leftData = buffer.getReadPointer(0);
                const float* rightData = numChannels > 1 ? buffer.getReadPointer(1) : nullptr;

                for (int i = 0; i < numSamples; ++i)
                {
                    smearBufferL[smearWritePos] = leftData[i];
                    smearBufferR[smearWritePos] = rightData ? rightData[i] : leftData[i];
                    smearWritePos = (smearWritePos + 1) % SMEAR_BUFFER_SIZE;
                }
                // Track how many samples we've captured (up to buffer size)
                smearCaptureLength = std::min(smearCaptureLength + numSamples, SMEAR_BUFFER_SIZE);
            }

            if (inPreBoundaryZone && antiClickCountdown == 0 && preThreshold > 0.0f)
            {
                // PRE-BOUNDARY: ramp up effect as we approach
                float fadeProgress = distanceFromBoundary / preThreshold;  // 1.0 = far, 0.0 = at boundary
                effectStrength = 1.0f - fadeProgress;  // 0.0 far, 1.0 at boundary
            }
            else if (antiClickCountdown > 0 && postSamples > 0)
            {
                // POST-BOUNDARY: ramp down effect as we move away
                float fadeProgress = static_cast<float>(antiClickCountdown) / static_cast<float>(postSamples);
                effectStrength = fadeProgress;  // 1.0 just crossed, 0.0 when done
                // smearActive is already set when we crossed the boundary
            }
            else
            {
                // Not in any boundary zone - reset smear state
                if (!inPreBoundaryZone && antiClickCountdown == 0)
                {
                    smearActive = false;
                    // Don't reset smearCaptureLength here - we want to keep capturing
                    // It will naturally fill the circular buffer
                }
            }

            // Apply LP filter ducking if strength > 0
            // IMPORTANT: Apply to loopOnlyBuffer, not buffer, because PluginProcessor
            // reconstructs output from loopOnlyBuffer + inputPassthrough, discarding buffer
            if (effectStrength > 0.0f && filterFreq > 0.0f && filterMix > 0.0f)
            {
                float* leftData = loopOnlyBuffer.getWritePointer(0);
                float* rightData = numChannels > 1 ? loopOnlyBuffer.getWritePointer(1) : nullptr;

                // LP filter coefficient - lower value = more aggressive filtering
                // For 200Hz at 48kHz: coeff = 2*pi*200/48000 = 0.026
                float filterCoeff = std::clamp(2.0f * juce::MathConstants<float>::pi * filterFreq / static_cast<float>(currentSampleRate), 0.01f, 1.0f);

                for (int i = 0; i < numSamples; ++i)
                {
                    // One-pole LP: y[n] = coeff * x[n] + (1 - coeff) * y[n-1]
                    float filteredL = filterCoeff * leftData[i] + (1.0f - filterCoeff) * antiClickFilterL;
                    float filteredR = rightData ? filterCoeff * rightData[i] + (1.0f - filterCoeff) * antiClickFilterR : 0.0f;
                    antiClickFilterL = filteredL;
                    antiClickFilterR = filteredR;

                    // Mix filtered with dry based on effectStrength and filterMix
                    float wetAmount = effectStrength * filterMix;
                    leftData[i] = leftData[i] * (1.0f - wetAmount) + filteredL * wetAmount;
                    if (rightData) rightData[i] = rightData[i] * (1.0f - wetAmount) + filteredR * wetAmount;
                }
            }

            // Apply VOLUME ducking if strength > 0 and volDepth > 0
            // IMPORTANT: Apply to loopOnlyBuffer, same as LP filter
            if (effectStrength > 0.0f && volDepth > 0.0f)
            {
                float* leftData = loopOnlyBuffer.getWritePointer(0);
                float* rightData = numChannels > 1 ? loopOnlyBuffer.getWritePointer(1) : nullptr;

                // Volume reduction: 1.0 = full volume, (1 - volDepth) = ducked volume
                // effectStrength ramps from 0 (far) to 1 (at boundary)
                float volumeMultiplier = 1.0f - (effectStrength * volDepth);

                for (int i = 0; i < numSamples; ++i)
                {
                    leftData[i] *= volumeMultiplier;
                    if (rightData) rightData[i] *= volumeMultiplier;
                }
            }

            // Decrement countdown for post-boundary phase
            if (antiClickCountdown > 0)
            {
                antiClickCountdown = std::max(0, antiClickCountdown - numSamples);
            }

            // SMEAR FILL: Apply AFTER all ducking effects, as a separate pass
            // This ensures the clean captured audio is not affected by LP filter or volume duck
            // Smear runs for smearLength * postTime samples, independent of effectStrength
            if (smearActive && smearAmount > 0.0f && smearCaptureLength > 0)
            {
                float* leftData = buffer.getWritePointer(0);
                float* rightData = numChannels > 1 ? buffer.getWritePointer(1) : nullptr;

                const float smearAttackFrac = xfadeSmearAttack.load();  // 0.01 to 0.5
                const float smearLengthMult = xfadeSmearLength.load();  // 0.25 to 2.0
                const int smearTotalSamples = static_cast<int>(static_cast<float>(postSamples) * smearLengthMult);

                for (int i = 0; i < numSamples; ++i)
                {
                    // How many samples since we crossed the boundary
                    int samplesSinceCrossing = (postSamples - antiClickCountdown) + i;

                    // Only process if within smear window
                    if (samplesSinceCrossing >= smearTotalSamples)
                    {
                        smearActive = false;
                        break;
                    }

                    // Read BACKWARDS from where we stopped writing (smearPlaybackStart)
                    int reverseOffset = samplesSinceCrossing;
                    if (reverseOffset >= smearCaptureLength) reverseOffset = reverseOffset % smearCaptureLength;
                    int readPos = (smearPlaybackStart - 1 - reverseOffset + SMEAR_BUFFER_SIZE) % SMEAR_BUFFER_SIZE;

                    float smearL = smearBufferL[readPos];
                    float smearR = smearBufferR[readPos];

                    // Apply envelope: attack, sustain, release based on parameters
                    float smearProgress = static_cast<float>(samplesSinceCrossing) / static_cast<float>(smearTotalSamples);

                    float smearEnvelope;
                    const float sustainEnd = 0.5f;  // Sustain ends at 50% of total length

                    if (smearProgress < smearAttackFrac)
                    {
                        // Attack phase - fade in
                        float attackProgress = smearProgress / smearAttackFrac;
                        smearEnvelope = 0.5f * (1.0f - std::cos(attackProgress * juce::MathConstants<float>::pi));
                    }
                    else if (smearProgress < sustainEnd)
                    {
                        // Sustain at full level
                        smearEnvelope = 1.0f;
                    }
                    else
                    {
                        // Release phase - fade out with squared cosine for gentle tail
                        float releaseProgress = (smearProgress - sustainEnd) / (1.0f - sustainEnd);
                        float cosVal = std::cos(releaseProgress * juce::MathConstants<float>::halfPi);
                        smearEnvelope = cosVal * cosVal;
                    }

                    // Add clean smear audio on top of ducked signal
                    float smearMix = smearAmount * smearEnvelope;
                    leftData[i] += smearL * smearMix;
                    if (rightData) rightData[i] += smearR * smearMix;
                }
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

    // Get playhead position for a specific layer (1-indexed)
    float getLayerPlayheadPosition(int layer) const
    {
        int idx = layer - 1;
        if (idx >= 0 && idx < NUM_LAYERS)
        {
            return layers[idx].getPlayheadPosition();
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
    void setInputMuted(bool muted)
    {
        inputMuted.store(muted);
        inputMuteGainSmoothed.setTargetValue(muted ? 0.0f : 1.0f);
    }
    bool getInputMuted() const { return inputMuted.load(); }
    float getInputLevelL() const { return inputLevelL.load(); }
    float getInputLevelR() const { return inputLevelR.load(); }

    // Diagnostic metering getters for debugging audio issues
    float getPreClipPeakL() const { return preClipPeakL.load(); }
    float getPreClipPeakR() const { return preClipPeakR.load(); }
    float getLoopOutputPeakL() const { return loopOutputPeakL.load(); }
    float getLoopOutputPeakR() const { return loopOutputPeakR.load(); }
    int getClipEventCount() const { return clipEventCount.load(); }
    void resetClipEventCount() { clipEventCount.store(0); }
    int getLayerClipCount(int layer) const {
        if (layer >= 0 && layer < NUM_LAYERS)
            return layerClipCounts[layer].load();
        return 0;
    }
    void resetLayerClipCounts() {
        for (int i = 0; i < NUM_LAYERS; ++i)
            layerClipCounts[i].store(0);
    }

    // Get per-layer peak levels for VU meters
    std::vector<float> getLayerLevels() const {
        std::vector<float> levels;
        levels.reserve(NUM_LAYERS);
        for (int i = 0; i < NUM_LAYERS; ++i) {
            levels.push_back(layerPeakLevels[i].load());
        }
        return levels;
    }

    // Set crossfade parameters from UI (LP filter + volume ducking)
    void setCrossfadeParams(int preTimeMs, int postTimeMs, float volDepth, float filterFreq, float filterDepth,
                            float smearAmount = 0.0f, float smearAttack = 0.1f, float smearLength = 1.0f)
    {
        xfadePreTimeMs.store(preTimeMs);
        xfadePostTimeMs.store(postTimeMs);
        xfadeVolDepth.store(volDepth);
        xfadeFilterFreq.store(filterFreq);
        xfadeFilterDepth.store(filterDepth);
        xfadeSmearAttack.store(smearAttack);
        xfadeSmearLength.store(smearLength);
        xfadeSmearAmount.store(smearAmount);
    }

    // Check if we can add another layer (for ADD or DUB)
    bool canAddLayer() const
    {
        return highestLayer < NUM_LAYERS - 1;
    }

    // Get layer type for UI display (1-indexed)
    bool isLayerOverride(int layer) const
    {
        int idx = layer - 1;
        if (idx >= 0 && idx < NUM_LAYERS)
        {
            return layers[idx].isOverrideLayer();
        }
        return false;
    }

    // Find the highest override layer index (0-indexed, -1 if none)
    // All regular layers below this should be muted during playback
    int findHighestOverrideLayer() const
    {
        for (int i = highestLayer; i >= 0; --i)
        {
            if (layers[i].hasContent() && layers[i].isOverrideLayer())
            {
                return i;
            }
        }
        return -1;  // No override layers
    }

    // ============================================================
    // ADD+ MODE - Blooper-style effect compounding
    // ============================================================
    //
    // ADD+ is a MODE TOGGLE that modifies DUB behavior:
    //   - When ADD+ is OFF: DUB works normally (adds new layers)
    //   - When ADD+ is ON: DUB creates/updates OVERRIDE layers (mixdown layers)
    //
    // Override layers:
    //   - Contain the full effected mix (loop + input + effects)
    //   - Automatically mute all Regular layers below them
    //   - Each loop cycle overwrites the SAME override layer (compounding effects)
    //   - Press DUB again while capturing to create a NEW stacked override layer
    //
    // Flow when ADD+ is ON:
    //   1. First DUB: Create override layer, start capturing
    //   2. Each loop boundary: OVERWRITE the same override layer with new capture
    //   3. Press DUB again: Create NEW override layer on top, continue capturing
    //   4. Press PLAY: Stop capturing, keep current override layers

    // Toggle ADD+ mode on/off (does NOT start recording)
    void setAdditiveModeEnabled(bool enabled)
    {
        additiveModeEnabled.store(enabled);
        DBG("ADD+ MODE: " + juce::String(enabled ? "ENABLED" : "DISABLED"));

        // If disabling while actively capturing, stop capture
        if (!enabled && additiveRecordingActive.load())
        {
            stopAdditiveCapture();
        }
    }

    bool isAdditiveModeEnabled() const
    {
        return additiveModeEnabled.load();
    }

    // Layer mode: Track (false) = sum all layers, Layer (true) = punch-through
    // In Layer mode, higher layers override lower layers where they have content
    void setLayerModeEnabled(bool enabled)
    {
        layerModeEnabled.store(enabled);
        DBG("LAYER MODE: " + juce::String(enabled ? "LAYER (punch-through)" : "TRACK (sum)"));
    }

    bool isLayerModeEnabled() const
    {
        return layerModeEnabled.load();
    }

    // Start additive capture (called when DUB is pressed while ADD+ mode is ON)
    // If already capturing, this creates a NEW override layer on top
    void startAdditiveCapture()
    {
        if (!additiveModeEnabled.load())
        {
            DBG("ADD+ CAPTURE: Cannot start - ADD+ mode not enabled");
            return;
        }

        if (masterLoopLength <= 0 || !hasContent())
        {
            DBG("ADD+ CAPTURE: Cannot start - no loop content");
            return;
        }

        // If already capturing, pressing DUB creates a NEW override layer on top
        if (additiveRecordingActive.load())
        {
            // Force create a new layer for the NEXT capture cycle
            additiveCreateNewLayer = true;
            DBG("ADD+ CAPTURE: Will create NEW override layer on next boundary");
            return;
        }

        // Allocate capture buffer if needed
        if (additiveCaptureBuffer.getNumSamples() < masterLoopLength)
        {
            additiveCaptureBuffer.setSize(2, masterLoopLength);
        }
        additiveCaptureBuffer.clear();
        additiveCaptureWritePos = 0;
        additiveNeedsReprint = false;
        additiveCreateNewLayer = true;  // First capture always creates new layer
        additiveTargetLayer = -1;       // No target yet

        additiveRecordingActive.store(true);
        DBG("ADD+ CAPTURE: Started capturing effected output");
    }

    // Stop additive capture (called when DUB ends while in ADD+ mode)
    void stopAdditiveCapture()
    {
        if (!additiveRecordingActive.load())
            return;

        additiveRecordingActive.store(false);

        // If we have enough captured, update/create the override layer now
        if (additiveCaptureWritePos >= masterLoopLength)
        {
            updateOrCreateOverrideLayer();
        }

        additiveNeedsReprint = false;
        additiveCaptureWritePos = 0;
        additiveTargetLayer = -1;
        additiveCreateNewLayer = false;
        DBG("ADD+ CAPTURE: Stopped");
    }

    // Called from processBlock when loop boundary is crossed
    void handleLoopBoundaryForAdditive()
    {
        if (!additiveRecordingActive.load())
            return;

        // If we have a full loop captured, update/create override layer
        if (additiveCaptureWritePos >= masterLoopLength)
        {
            updateOrCreateOverrideLayer();

            // Reset for next cycle if still capturing
            additiveCaptureBuffer.clear();
            additiveCaptureWritePos = 0;
            DBG("ADD+ BOUNDARY: Updated override layer, starting new capture cycle");
        }
    }

    // Update existing override layer OR create new one based on additiveCreateNewLayer flag
    void updateOrCreateOverrideLayer()
    {
        if (additiveCaptureWritePos < masterLoopLength)
        {
            DBG("ADD+ UPDATE: Not enough captured audio");
            return;
        }

        // Get current playback state
        const float savedPlayhead = layers[0].getRawPlayhead();
        const LoopBuffer::State savedState = layers[0].getState();

        if (additiveCreateNewLayer || additiveTargetLayer < 0)
        {
            // Create NEW override layer
            if (highestLayer >= NUM_LAYERS - 1)
            {
                DBG("ADD+ CREATE: Max layers reached, overwriting top layer instead");
                // Overwrite the top layer instead
                additiveTargetLayer = highestLayer;
            }
            else
            {
                int newLayerIdx = highestLayer + 1;
                highestLayer = newLayerIdx;
                currentLayer = newLayerIdx;
                additiveTargetLayer = newLayerIdx;
                DBG("ADD+ CREATE: Created new override layer " + juce::String(newLayerIdx + 1));
            }
            additiveCreateNewLayer = false;  // Only create once per DUB press
        }

        // Update the target layer with captured audio
        layers[additiveTargetLayer].setFromBufferSeamless(additiveCaptureBuffer, masterLoopLength, savedPlayhead, savedState);
        layers[additiveTargetLayer].setLayerType(LoopBuffer::LayerType::Override);
        layers[additiveTargetLayer].applyBufferSoftClip();

        currentLayer = additiveTargetLayer;

        DBG("ADD+ UPDATE: Updated override layer " + juce::String(additiveTargetLayer + 1) +
            " with " + juce::String(masterLoopLength) + " samples");
    }

    bool isAdditiveRecordingActive() const
    {
        return additiveRecordingActive.load();
    }

    // Called from PluginProcessor after effects chain to capture effected audio
    void captureForAdditive(const juce::AudioBuffer<float>& buffer, int numSamples)
    {
        if (!additiveRecordingActive.load() || masterLoopLength <= 0)
            return;

        // Ensure buffer is allocated
        if (additiveCaptureBuffer.getNumSamples() < masterLoopLength)
            return;

        const int numChannels = std::min(buffer.getNumChannels(), 2);

        // Get the current playhead position to sync capture with playback
        int playheadSamples = static_cast<int>(layers[0].getRawPlayhead());
        if (playheadSamples < 0) playheadSamples = 0;
        if (playheadSamples >= masterLoopLength) playheadSamples = playheadSamples % masterLoopLength;

        // Write to capture buffer at playhead-synced position
        for (int i = 0; i < numSamples; ++i)
        {
            int writePos = (playheadSamples + i) % masterLoopLength;
            for (int ch = 0; ch < numChannels; ++ch)
            {
                additiveCaptureBuffer.setSample(ch, writePos, buffer.getSample(ch, i));
            }
        }

        // Track total samples captured
        additiveCaptureWritePos += numSamples;
    }

    // Flatten all non-muted layers into layer 0
    // Sums all active layer buffers into layer 0 and clears the others
    // SEAMLESS: preserves playhead position and continues playback without interruption
    // NOW APPLIES ALL PER-LAYER EFFECTS: volume, pan, EQ, pitch shift, reverse, loop bounds
    void flattenLayers()
    {
        // Check if we have anything to flatten
        bool hasLayers = (masterLoopLength > 0 && layers[0].hasContent());

        if (!hasLayers)
        {
            DBG("flattenLayers() - Nothing to flatten");
            return;
        }

        if (highestLayer == 0)
        {
            DBG("flattenLayers() - Only one layer, nothing to flatten");
            return;
        }

        DBG("flattenLayers() - Flattening " + juce::String(highestLayer + 1) + " layers into layer 0 (seamless, with effects)");

        // Capture current playback state BEFORE modifying anything
        const float savedPlayhead = layers[0].getRawPlayhead();
        const LoopBuffer::State savedState = layers[0].getState();

        DBG("  Saved playhead: " + juce::String(savedPlayhead) + " state: " + juce::String(static_cast<int>(savedState)));

        // Create a temporary buffer to accumulate all layers
        const int numChannels = 2;  // Stereo
        juce::AudioBuffer<float> flattenedBuffer(numChannels, masterLoopLength);
        flattenedBuffer.clear();

        // Sum all non-muted layers WITH EFFECTS APPLIED
        for (int i = 0; i <= highestLayer; ++i)
        {
            if (!layers[i].getMuted() && layers[i].hasContent())
            {
                // Get this layer's buffer with all effects applied and add to flattened
                layers[i].addToBufferWithEffects(flattenedBuffer, currentSampleRate);
                DBG("  Added layer " + juce::String(i + 1) + " with effects");
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

        // Reset all layer settings to default (effects are now baked into the audio)
        for (int i = 0; i < NUM_LAYERS; ++i)
        {
            layers[i].setVolume(1.0f);      // Default volume
            layers[i].setPan(0.0f);         // Center pan
            layers[i].setMuted(false);      // Unmute
            layers[i].setReverse(false);    // Forward playback
            layers[i].setLayerPitch(0.0f);  // No pitch shift
            layers[i].setEQLow(0.0f);       // Flat EQ
            layers[i].setEQMid(0.0f);
            layers[i].setEQHigh(0.0f);
            layers[i].setLoopStart(0.0f);   // Full loop
            layers[i].setLoopEnd(1.0f);
        }

        // Reset state
        currentLayer = 0;
        highestLayer = 0;

        DBG("flattenLayers() - Complete, layer 0 now contains flattened audio with all effects baked in" +
            juce::String(", playback continues at ") + juce::String(savedPlayhead));
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
    float globalLoopStart = 0.0f;  // Master loop start (for change detection)
    float globalLoopEnd = 1.0f;    // Master loop end (for change detection)
    std::atomic<int> presetLengthBars { 0 };  // 0 = free, 1-16 = bars
    std::atomic<int> presetLengthBeats { 0 }; // 0-7 additional beats
    std::atomic<float> hostBpm { 120.0f };

    // Input monitoring
    std::atomic<float> inputLevelL { 0.0f };
    std::atomic<float> inputLevelR { 0.0f };
    std::atomic<bool> inputMuted { false };
    juce::SmoothedValue<float> inputMuteGainSmoothed;  // For click-free input muting
    std::atomic<int> soloCount { 0 };  // Number of layers currently soloed

    // Diagnostic metering for debugging audio issues
    std::atomic<float> preClipPeakL { 0.0f };   // Peak level before soft clipping
    std::atomic<float> preClipPeakR { 0.0f };
    std::atomic<float> loopOutputPeakL { 0.0f }; // Peak level of loop-only output
    std::atomic<float> loopOutputPeakR { 0.0f };
    std::atomic<int> clipEventCount { 0 };       // Number of samples that exceeded 1.0
    std::atomic<int> layerClipCounts[NUM_LAYERS] { {0}, {0}, {0}, {0}, {0}, {0}, {0}, {0} };  // Per-layer clip counts
    std::atomic<float> layerPeakLevels[NUM_LAYERS] { {0.0f}, {0.0f}, {0.0f}, {0.0f}, {0.0f}, {0.0f}, {0.0f}, {0.0f} };  // Per-layer peak levels for VU meters

    // Pre-allocated buffers to avoid allocation in processBlock
    juce::AudioBuffer<float> inputBuffer;
    juce::AudioBuffer<float> layerBuffer;
    juce::AudioBuffer<float> dummyBuffer;
    juce::AudioBuffer<float> loopOnlyBuffer;  // Loop playback only, no input (for Blooper-style effects)

    // Anti-click ducking state - fades out BEFORE boundary and fades in AFTER
    // This creates a smooth volume envelope that straddles the loop crossing point
    float lastMasterPlayheadPos = 0.0f;  // For detecting loop boundary crossing
    int antiClickCountdown = 0;          // Samples remaining in POST-boundary fade-in
    bool approachingBoundary = false;    // True when we're in PRE-boundary fade-out zone

    // Crossfade parameters (settable from UI)
    // TEST: AGGRESSIVE 500ms pre/post, 200Hz LP filter - should be VERY obvious
    std::atomic<int> xfadePreTimeMs { 500 };       // Pre-boundary fade time (ms)
    std::atomic<int> xfadePostTimeMs { 500 };      // Post-boundary fade time (ms)
    std::atomic<float> xfadeFilterFreq { 200.0f }; // LP filter cutoff at boundary (Hz) - VERY dark
    std::atomic<float> xfadeFilterDepth { 1.0f };  // Filter mix at boundary (0-1) - 100%
    std::atomic<float> xfadeVolDepth { 0.5f };     // Volume duck depth (0-1) - 50% default
    float antiClickFilterL = 0.0f;  // One-pole LP filter state
    float antiClickFilterR = 0.0f;

    // Smear/fill parameters - captures audio before boundary and blends it across
    std::atomic<float> xfadeSmearAmount { 0.0f };  // 0 = off, 1 = full smear blend
    std::atomic<float> xfadeSmearAttack { 0.1f };  // Attack time as fraction of post time (0.01 to 0.5)
    std::atomic<float> xfadeSmearLength { 1.0f };  // Length multiplier (0.25 to 2.0) - extends beyond post time
    static constexpr int SMEAR_BUFFER_SIZE = 8192; // ~185ms at 44.1kHz
    std::vector<float> smearBufferL;
    std::vector<float> smearBufferR;
    int smearWritePos = 0;
    int smearCaptureLength = 0;      // How many samples captured before boundary
    int smearPlaybackStart = 0;      // Write position snapshot at boundary crossing
    bool smearActive = false;        // True when playing back smear buffer

    // Layer mode: Track (false) = sum all layers, Layer (true) = punch-through (higher layers override)
    std::atomic<bool> layerModeEnabled { false };         // false = Track mode, true = Layer mode

    // Layer mode bounce tracking - only bounce for exactly one loop pass
    // After one full loop, stop adding bounce audio to prevent volume buildup
    int layerModeBounceLayer = -1;           // Which layer is being bounced to (-1 = none)
    int layerModeBounceStartSample = -1;     // Playhead position when bounce started
    int layerModeBounceProgress = 0;         // Samples recorded since bounce started
    bool layerModeBounceComplete = false;    // True after one full loop pass - stop bouncing

    // Additive mode state (Blooper-style effect compounding)
    // additiveModeEnabled = toggle state (UI button)
    // additiveRecordingActive = actually capturing (mode ON + DUB pressed)
    std::atomic<bool> additiveModeEnabled { false };      // ADD+ mode toggle
    std::atomic<bool> additiveRecordingActive { false };  // Currently capturing
    bool additiveNeedsReprint = false;            // True when capture is complete
    juce::AudioBuffer<float> additiveCaptureBuffer;  // Buffer to capture effected audio
    int additiveCaptureWritePos = 0;              // Tracks total samples captured
    int additiveTargetLayer = -1;                 // Which override layer we're updating (-1 = none)
    bool additiveCreateNewLayer = false;          // If true, next boundary creates NEW override layer

    // Debug counter (member variable to avoid static issues)
    int xfadeDebugCounter = 0;

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
