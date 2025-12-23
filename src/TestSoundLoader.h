#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <vector>
#include <memory>

/**
 * TestSoundLoader - Loads audio samples from disk for testing the delay effect.
 *
 * Samples are loaded from a configurable folder path. The loader scans for
 * WAV, AIFF, MP3, FLAC files and makes them available for playback.
 *
 * Default sample folder: ~/Documents/FuzzDelaySamples/
 * You can also set a custom path.
 */
class TestSoundLoader
{
public:
    static constexpr int MAX_SAMPLES = 20;

    TestSoundLoader()
    {
        formatManager.registerBasicFormats();
    }

    void prepare(double sampleRate, int /*samplesPerBlock*/)
    {
        currentSampleRate = sampleRate;
        isPrepared = true;

        // Try to load samples from default location
        loadSamplesFromFolder(getDefaultSampleFolder());
    }

    static juce::File getDefaultSampleFolder()
    {
        // Default: ~/Documents/FuzzDelaySamples/
        return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile("FuzzDelaySamples");
    }

    void loadSamplesFromFolder(const juce::File& folder)
    {
        sampleFolder = folder;
        sampleBuffers.clear();
        sampleNames.clear();
        sampleFilePaths.clear();

        if (!folder.exists())
        {
            // Create the folder if it doesn't exist
            folder.createDirectory();
            DBG("Created sample folder: " + folder.getFullPathName());
            return;
        }

        // Find all audio files
        juce::Array<juce::File> audioFiles;
        folder.findChildFiles(audioFiles, juce::File::findFiles, false,
            "*.wav;*.aiff;*.aif;*.mp3;*.flac;*.ogg");

        // Sort alphabetically
        audioFiles.sort();

        // Load up to MAX_SAMPLES
        for (int i = 0; i < std::min(audioFiles.size(), MAX_SAMPLES); ++i)
        {
            const auto& file = audioFiles[i];
            if (loadSample(file))
            {
                sampleNames.push_back(file.getFileNameWithoutExtension().toStdString());
                sampleFilePaths.push_back(file.getFullPathName().toStdString());
                DBG("Loaded sample: " + file.getFileName());
            }
        }

        DBG("Loaded " + juce::String(static_cast<int>(sampleBuffers.size())) + " samples from " + folder.getFullPathName());
    }

    void reloadSamples()
    {
        if (sampleFolder.exists())
            loadSamplesFromFolder(sampleFolder);
    }

    int getNumSamples() const { return static_cast<int>(sampleBuffers.size()); }

    juce::String getSampleName(int index) const
    {
        if (index >= 0 && index < static_cast<int>(sampleNames.size()))
            return juce::String(sampleNames[static_cast<size_t>(index)]);
        return "---";
    }

    juce::StringArray getAllSampleNames() const
    {
        juce::StringArray names;
        for (const auto& name : sampleNames)
            names.add(juce::String(name));
        return names;
    }

    juce::String getSampleFolderPath() const
    {
        return sampleFolder.getFullPathName();
    }

    void trigger(int sampleIndex)
    {
        if (!isPrepared || sampleIndex < 0 || sampleIndex >= static_cast<int>(sampleBuffers.size()))
            return;

        currentSample.store(sampleIndex);
        playbackPosition.store(0);
        isPlaying.store(true);
    }

    void stop()
    {
        isPlaying.store(false);
        playbackPosition.store(0);
    }

    void setLoopEnabled(bool enabled)
    {
        loopEnabled.store(enabled);
    }

    bool getLoopEnabled() const
    {
        return loopEnabled.load();
    }

    void processBlock(juce::AudioBuffer<float>& buffer)
    {
        if (!isPlaying.load())
            return;

        const int sampleIdx = currentSample.load();
        if (sampleIdx < 0 || sampleIdx >= static_cast<int>(sampleBuffers.size()))
            return;

        const auto& sourceBuffer = sampleBuffers[static_cast<size_t>(sampleIdx)];
        if (sourceBuffer.getNumSamples() == 0)
            return;

        const int numSamples = buffer.getNumSamples();
        const int numChannels = buffer.getNumChannels();
        const int sourceLength = sourceBuffer.getNumSamples();
        const int sourceChannels = sourceBuffer.getNumChannels();
        int pos = playbackPosition.load();

        for (int sample = 0; sample < numSamples; ++sample)
        {
            if (pos >= sourceLength)
            {
                if (loopEnabled.load())
                    pos = 0;
                else
                {
                    isPlaying.store(false);
                    playbackPosition.store(0);
                    return;
                }
            }

            for (int channel = 0; channel < numChannels; ++channel)
            {
                const int sourceChannel = std::min(channel, sourceChannels - 1);
                buffer.addSample(channel, sample,
                    sourceBuffer.getSample(sourceChannel, pos));
            }

            ++pos;
        }

        playbackPosition.store(pos);
    }

    bool getIsPlaying() const { return isPlaying.load(); }

private:
    bool loadSample(const juce::File& file)
    {
        std::unique_ptr<juce::AudioFormatReader> reader(
            formatManager.createReaderFor(file));

        if (reader == nullptr)
            return false;

        // Read the entire file
        juce::AudioBuffer<float> tempBuffer(
            static_cast<int>(reader->numChannels),
            static_cast<int>(reader->lengthInSamples));

        reader->read(&tempBuffer, 0, static_cast<int>(reader->lengthInSamples), 0, true, true);

        // Resample if needed
        if (std::abs(reader->sampleRate - currentSampleRate) > 1.0)
        {
            const double ratio = currentSampleRate / reader->sampleRate;
            const int newLength = static_cast<int>(static_cast<double>(tempBuffer.getNumSamples()) * ratio);

            juce::AudioBuffer<float> resampledBuffer(tempBuffer.getNumChannels(), newLength);

            // Simple linear interpolation resampling
            for (int ch = 0; ch < tempBuffer.getNumChannels(); ++ch)
            {
                const float* src = tempBuffer.getReadPointer(ch);
                float* dst = resampledBuffer.getWritePointer(ch);

                for (int i = 0; i < newLength; ++i)
                {
                    const double srcPos = static_cast<double>(i) / ratio;
                    const int srcIdx = static_cast<int>(srcPos);
                    const float frac = static_cast<float>(srcPos - static_cast<double>(srcIdx));

                    if (srcIdx + 1 < tempBuffer.getNumSamples())
                        dst[i] = src[srcIdx] * (1.0f - frac) + src[srcIdx + 1] * frac;
                    else
                        dst[i] = src[srcIdx];
                }
            }

            sampleBuffers.push_back(std::move(resampledBuffer));
        }
        else
        {
            sampleBuffers.push_back(std::move(tempBuffer));
        }

        return true;
    }

    juce::AudioFormatManager formatManager;
    double currentSampleRate = 44100.0;
    bool isPrepared = false;

    juce::File sampleFolder;
    std::vector<juce::AudioBuffer<float>> sampleBuffers;
    std::vector<std::string> sampleNames;
    std::vector<std::string> sampleFilePaths;

    std::atomic<int> currentSample { 0 };
    std::atomic<int> playbackPosition { 0 };
    std::atomic<bool> isPlaying { false };
    std::atomic<bool> loopEnabled { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TestSoundLoader)
};
