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

    // Transport controls
    void record()
    {
        LoopBuffer::State currentState = getCurrentState();

        if (currentState == LoopBuffer::State::Idle)
        {
            // Start recording on layer 0 (first layer)
            if (highestLayer == 0 && !layers[0].hasContent())
            {
                currentLayer = 0;
                // Reset loop parameters to defaults when starting new recording
                resetLoopParams();
                layers[0].startRecording();
            }
            else if (highestLayer < NUM_LAYERS - 1)
            {
                // Start recording on next available layer (overdub)
                currentLayer = highestLayer + 1;
                layers[currentLayer].startRecording();
            }
        }
        else if (currentState == LoopBuffer::State::Recording)
        {
            // Stop recording
            stopRecording();
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

    void stopRecording()
    {
        LoopBuffer::State currentState = getCurrentState();

        if (currentState == LoopBuffer::State::Recording)
        {
            layers[currentLayer].stopRecording();

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

        if (currentState == LoopBuffer::State::Playing)
        {
            // Start overdubbing on current layer
            layers[currentLayer].startOverdub();
        }
        else if (currentState == LoopBuffer::State::Overdubbing)
        {
            // Stop overdubbing
            layers[currentLayer].stopOverdub();
        }
        else if (currentState == LoopBuffer::State::Idle && highestLayer >= 0 && layers[0].hasContent())
        {
            // If idle with content, play and immediately overdub
            play();
            layers[currentLayer].startOverdub();
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

        for (int i = 0; i <= highestLayer; ++i)
        {
            if (layers[i].hasContent())
            {
                auto layerWaveform = layers[i].getWaveformData(numPoints);
                for (int j = 0; j < numPoints; ++j)
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

private:
    std::array<LoopBuffer, NUM_LAYERS> layers;
    int currentLayer = 0;
    int highestLayer = 0;
    int masterLoopLength = 0;
    double currentSampleRate = 44100.0;
    bool isReversed = false;  // Master reverse state

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
