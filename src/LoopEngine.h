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
        for (int i = 0; i < NUM_LAYERS; ++i)
        {
            layers[i].setLoopStart(0.0f);
            layers[i].setLoopEnd(1.0f);
            layers[i].setPlaybackRate(1.0f);
            layers[i].setReverse(false);
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
            // Start overdubbing on current layer
            DBG("Starting overdub on layer " + juce::String(currentLayer));
            layers[currentLayer].startOverdub();
        }
        else if (currentState == LoopBuffer::State::Overdubbing)
        {
            // Stop overdubbing
            DBG("Stopping overdub on layer " + juce::String(currentLayer));
            layers[currentLayer].stopOverdub();
        }
        else if (currentState == LoopBuffer::State::Idle && highestLayer >= 0 && layers[0].hasContent())
        {
            // If idle with content, play and immediately overdub
            DBG("Idle with content - starting play + overdub");
            play();
            layers[currentLayer].startOverdub();
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
            // Clear current layer and go back
            layers[currentLayer].clear();
            --currentLayer;

            // Update highest layer
            highestLayer = std::min(highestLayer, currentLayer);
        }
    }

    void redo()
    {
        // Not implemented in this version - would need to store cleared layers
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

        // Process each layer
        bool anyPlaying = false;
        for (int i = 0; i <= highestLayer; ++i)
        {
            if (!layers[i].hasContent() && layers[i].getState() != LoopBuffer::State::Recording)
                continue;

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

        // Include layers with content OR currently recording
        for (int i = 0; i <= std::max(highestLayer, currentLayer); ++i)
        {
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

    // Set preset loop length (in bars, 0 = free/unlimited)
    void setLoopLengthBars(int bars)
    {
        presetLengthBars.store(bars);
        DBG("LoopEngine::setLoopLengthBars(" + juce::String(bars) + ")");
    }

    int getLoopLengthBars() const { return presetLengthBars.load(); }

    // Set host BPM for calculating bar lengths
    void setHostBpm(float bpm)
    {
        hostBpm.store(bpm);
    }

    // Get target loop length in samples (0 = unlimited)
    int getTargetLoopLengthSamples() const
    {
        int bars = presetLengthBars.load();
        if (bars <= 0)
            return 0;  // Free mode - no limit

        float bpm = hostBpm.load();
        if (bpm <= 0.0f)
            bpm = 120.0f;  // Default fallback

        // 4 beats per bar, samples per beat = sampleRate * 60 / BPM
        double samplesPerBeat = currentSampleRate * 60.0 / static_cast<double>(bpm);
        double samplesPerBar = samplesPerBeat * 4.0;
        return static_cast<int>(samplesPerBar * bars);
    }

private:
    std::array<LoopBuffer, NUM_LAYERS> layers;
    int currentLayer = 0;
    int highestLayer = 0;
    int masterLoopLength = 0;
    double currentSampleRate = 44100.0;
    bool isReversed = false;  // Master reverse state
    std::atomic<int> presetLengthBars { 0 };  // 0 = free, 1/2/4/8 = bars
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
