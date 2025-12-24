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

            // Start recording on layer 0 (first layer)
            if (highestLayer == 0 && !layers[0].hasContent())
            {
                currentLayer = 0;
                // Reset loop parameters to defaults when starting new recording
                resetLoopParams();
                DBG("record() - Starting recording on layer 0");
                layers[0].startRecording(targetLength);
            }
            else if (highestLayer < NUM_LAYERS - 1)
            {
                // Start recording on next available layer (overdub)
                currentLayer = highestLayer + 1;
                // Use master loop length for subsequent layers (or target if first had preset)
                int layerTarget = (masterLoopLength > 0) ? masterLoopLength : targetLength;
                DBG("record() - Starting recording on layer " + juce::String(currentLayer));
                layers[currentLayer].startRecording(layerTarget);
            }
        }
        else if (currentState == LoopBuffer::State::Recording)
        {
            // Stop recording -> starts playback
            DBG("record() - Stopping recording");
            stopRecording();
        }
        else if (currentState == LoopBuffer::State::Playing)
        {
            // Blooper-style: REC while playing starts overdubbing
            DBG("record() - Playing state, calling overdub()");
            overdub();
        }
        else if (currentState == LoopBuffer::State::Overdubbing)
        {
            // REC while overdubbing stops overdub, continues playback
            DBG("record() - Overdubbing state, stopping overdub");
            layers[currentLayer].stopOverdub();
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
        // Play all layers that have content
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
        // Stop all layers
        for (int i = 0; i < NUM_LAYERS; ++i)
        {
            layers[i].stop();
        }
    }

    void overdub()
    {
        LoopBuffer::State currentState = getCurrentState();
        DBG("LoopEngine::overdub() called, currentState=" + juce::String(static_cast<int>(currentState)) +
            " currentLayer=" + juce::String(currentLayer) +
            " highestLayer=" + juce::String(highestLayer) +
            " hasContent=" + juce::String(layers[0].hasContent() ? "true" : "false"));

        if (currentState == LoopBuffer::State::Playing)
        {
            // Create a NEW layer for overdub (Blooper-style)
            // Each overdub creates a separate layer that can be undone
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
        else if (currentState == LoopBuffer::State::Overdubbing)
        {
            // Stop overdubbing
            DBG("Stopping overdub on layer " + juce::String(currentLayer));
            layers[currentLayer].stopOverdub();
        }
        else if (currentState == LoopBuffer::State::Idle && highestLayer >= 0 && layers[0].hasContent())
        {
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
            // Stop the current layer (don't clear - keep data for redo)
            layers[currentLayer].stop();
            --currentLayer;

            // Track highest undone layer for redo
            // highestLayer stays the same - it tracks max recorded, not current
            DBG("undo() - moved to layer " + juce::String(currentLayer) +
                ", highestLayer still " + juce::String(highestLayer));
        }
    }

    void redo()
    {
        // Can only redo if there are undone layers above current
        if (currentLayer < highestLayer)
        {
            ++currentLayer;
            // Re-enable the layer by playing it
            if (layers[currentLayer].hasContent())
            {
                layers[currentLayer].play();
            }
            DBG("redo() - restored layer " + juce::String(currentLayer));
        }
    }

    void clear()
    {
        for (int i = 0; i < NUM_LAYERS; ++i)
        {
            layers[i].clear();
        }
        currentLayer = 0;
        highestLayer = 0;
        masterLoopLength = 0;
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

    // Process audio
    void processBlock(juce::AudioBuffer<float>& buffer)
    {
        const int numSamples = buffer.getNumSamples();
        const int numChannels = buffer.getNumChannels();

        // Create a temp buffer for mixing
        juce::AudioBuffer<float> mixBuffer(numChannels, numSamples);
        mixBuffer.clear();

        // Get input for recording/monitoring
        juce::AudioBuffer<float> inputBuffer;
        inputBuffer.makeCopyOf(buffer);

        // Clear output
        buffer.clear();

        // Process ALL layers with content up to highestLayer
        // Each layer can be independently muted regardless of currentLayer
        bool anyPlaying = false;

        // Debug: log layer processing once per second
        static int debugCounter = 0;
        bool shouldLog = (++debugCounter % 1000 == 0);

        for (int i = 0; i <= highestLayer; ++i)
        {
            bool hasContent = layers[i].hasContent();
            bool isRecording = (layers[i].getState() == LoopBuffer::State::Recording);
            bool isMuted = layers[i].getMuted();

            if (!hasContent && !isRecording)
                continue;

            if (shouldLog)
            {
                DBG("Layer " + juce::String(i) + ": hasContent=" + juce::String(hasContent ? 1 : 0) +
                    " state=" + juce::String(static_cast<int>(layers[i].getState())) +
                    " muted=" + juce::String(isMuted ? 1 : 0));
            }

            // Skip muted layers (but still advance playhead so they stay in sync)
            if (isMuted)
            {
                // Create a dummy buffer just to advance the layer's state
                juce::AudioBuffer<float> dummyBuffer(numChannels, numSamples);
                dummyBuffer.clear();
                layers[i].processBlock(dummyBuffer);
                // Don't mix muted layer into output
                continue;
            }

            // Copy input to temp buffer for this layer's processing
            juce::AudioBuffer<float> layerBuffer;
            layerBuffer.makeCopyOf(inputBuffer);

            layers[i].processBlock(layerBuffer);

            // Mix into output
            for (int ch = 0; ch < numChannels; ++ch)
            {
                buffer.addFrom(ch, 0, layerBuffer, ch, 0, numSamples);
            }

            if (layers[i].getState() != LoopBuffer::State::Idle)
            {
                anyPlaying = true;
            }
        }

        // If nothing is playing/recording, pass through input
        if (!anyPlaying && highestLayer == 0 && !layers[0].hasContent())
        {
            buffer.makeCopyOf(inputBuffer);
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
    std::vector<std::vector<float>> getLayerWaveforms(int numPoints) const
    {
        std::vector<std::vector<float>> layerWaveforms;

        for (int i = 0; i <= highestLayer; ++i)
        {
            bool isRecording = (layers[i].getState() == LoopBuffer::State::Recording);
            if (layers[i].hasContent() || isRecording)
            {
                auto waveform = layers[i].getWaveformData(numPoints);

                // Normalize this layer
                float maxVal = 0.0f;
                for (float val : waveform)
                {
                    maxVal = std::max(maxVal, std::abs(val));
                }
                if (maxVal > 0.0f)
                {
                    for (float& val : waveform)
                    {
                        val /= maxVal;
                    }
                }

                layerWaveforms.push_back(waveform);
            }
            else
            {
                // Empty placeholder for this layer
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
