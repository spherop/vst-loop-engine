#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

class TestToneGenerator
{
public:
    enum class SoundType
    {
        Click,      // Short percussive transient
        DrumLoop,   // Simple kick/snare pattern
        SynthChord  // Filtered chord stab
    };

    TestToneGenerator() = default;

    void prepare(double sampleRate, int samplesPerBlock)
    {
        currentSampleRate = sampleRate;
        maxBlockSize = samplesPerBlock;

        // Pre-generate the test sounds
        generateClick();
        generateDrumLoop();
        generateSynthChord();
    }

    void trigger(SoundType type)
    {
        currentSound = type;
        playbackPosition = 0;
        isPlaying = true;
    }

    void stop()
    {
        isPlaying = false;
        playbackPosition = 0;
    }

    void processBlock(juce::AudioBuffer<float>& buffer)
    {
        if (!isPlaying)
            return;

        const auto* sourceBuffer = getBufferForSound(currentSound);
        if (sourceBuffer == nullptr)
            return;

        const int numSamples = buffer.getNumSamples();
        const int numChannels = buffer.getNumChannels();
        const int sourceLength = sourceBuffer->getNumSamples();

        for (int sample = 0; sample < numSamples; ++sample)
        {
            if (playbackPosition >= sourceLength)
            {
                // Loop drum pattern, stop others
                if (currentSound == SoundType::DrumLoop)
                    playbackPosition = 0;
                else
                {
                    isPlaying = false;
                    return;
                }
            }

            for (int channel = 0; channel < numChannels; ++channel)
            {
                const int sourceChannel = std::min(channel, sourceBuffer->getNumChannels() - 1);
                buffer.addSample(channel, sample,
                    sourceBuffer->getSample(sourceChannel, playbackPosition));
            }

            ++playbackPosition;
        }
    }

    bool getIsPlaying() const { return isPlaying; }

private:
    double currentSampleRate = 44100.0;
    int maxBlockSize = 512;

    juce::AudioBuffer<float> clickBuffer;
    juce::AudioBuffer<float> drumLoopBuffer;
    juce::AudioBuffer<float> synthChordBuffer;

    SoundType currentSound = SoundType::Click;
    int playbackPosition = 0;
    bool isPlaying = false;

    const juce::AudioBuffer<float>* getBufferForSound(SoundType type) const
    {
        switch (type)
        {
            case SoundType::Click:      return &clickBuffer;
            case SoundType::DrumLoop:   return &drumLoopBuffer;
            case SoundType::SynthChord: return &synthChordBuffer;
            default: return nullptr;
        }
    }

    void generateClick()
    {
        // Short transient click - 50ms
        const int numSamples = static_cast<int>(currentSampleRate * 0.05);
        clickBuffer.setSize(1, numSamples);
        clickBuffer.clear();

        auto* data = clickBuffer.getWritePointer(0);

        // Noise burst with exponential decay
        juce::Random random;
        for (int i = 0; i < numSamples; ++i)
        {
            const float envelope = std::exp(-static_cast<float>(i) / (numSamples * 0.1f));
            data[i] = (random.nextFloat() * 2.0f - 1.0f) * envelope * 0.8f;
        }

        // Add a pitched transient at the start
        const float clickFreq = 1000.0f;
        for (int i = 0; i < std::min(numSamples, static_cast<int>(currentSampleRate * 0.01)); ++i)
        {
            const float env = std::exp(-static_cast<float>(i) / (currentSampleRate * 0.002f));
            data[i] += std::sin(2.0f * juce::MathConstants<float>::pi * clickFreq * i / currentSampleRate) * env * 0.5f;
        }
    }

    void generateDrumLoop()
    {
        // 2-bar loop at 120 BPM (4 seconds)
        const float bpm = 120.0f;
        const float beatsPerLoop = 8.0f;
        const float loopDuration = (60.0f / bpm) * beatsPerLoop;
        const int numSamples = static_cast<int>(currentSampleRate * loopDuration);

        drumLoopBuffer.setSize(1, numSamples);
        drumLoopBuffer.clear();

        auto* data = drumLoopBuffer.getWritePointer(0);

        const float samplesPerBeat = currentSampleRate * 60.0f / bpm;

        // Pattern: Kick on 1, 3, 5, 7 | Snare on 3, 7 | Hi-hat on every 8th
        for (int beat = 0; beat < 8; ++beat)
        {
            const int beatStart = static_cast<int>(beat * samplesPerBeat);

            // Kick on beats 0, 2, 4, 6
            if (beat % 2 == 0)
                addKick(data, beatStart, numSamples);

            // Snare on beats 2, 6
            if (beat == 2 || beat == 6)
                addSnare(data, beatStart, numSamples);

            // Hi-hat on every 8th note
            addHiHat(data, beatStart, numSamples);
            addHiHat(data, beatStart + static_cast<int>(samplesPerBeat / 2), numSamples);
        }
    }

    void addKick(float* data, int startSample, int bufferLength)
    {
        // Kick: sine wave with pitch drop
        const int kickLength = static_cast<int>(currentSampleRate * 0.15);
        const float startFreq = 150.0f;
        const float endFreq = 50.0f;

        float phase = 0.0f;
        for (int i = 0; i < kickLength && (startSample + i) < bufferLength; ++i)
        {
            const float t = static_cast<float>(i) / kickLength;
            const float freq = startFreq + (endFreq - startFreq) * t;
            const float envelope = std::exp(-t * 5.0f);

            phase += 2.0f * juce::MathConstants<float>::pi * freq / currentSampleRate;
            data[startSample + i] += std::sin(phase) * envelope * 0.7f;
        }
    }

    void addSnare(float* data, int startSample, int bufferLength)
    {
        // Snare: noise + tone
        const int snareLength = static_cast<int>(currentSampleRate * 0.12);
        juce::Random random(42);

        for (int i = 0; i < snareLength && (startSample + i) < bufferLength; ++i)
        {
            const float t = static_cast<float>(i) / snareLength;
            const float envelope = std::exp(-t * 8.0f);

            // Noise component
            const float noise = (random.nextFloat() * 2.0f - 1.0f) * 0.4f;

            // Tone component (200 Hz)
            const float tone = std::sin(2.0f * juce::MathConstants<float>::pi * 200.0f * i / currentSampleRate) * 0.3f;

            data[startSample + i] += (noise + tone) * envelope;
        }
    }

    void addHiHat(float* data, int startSample, int bufferLength)
    {
        // Hi-hat: filtered noise
        const int hatLength = static_cast<int>(currentSampleRate * 0.05);
        juce::Random random(123);

        for (int i = 0; i < hatLength && (startSample + i) < bufferLength; ++i)
        {
            const float t = static_cast<float>(i) / hatLength;
            const float envelope = std::exp(-t * 15.0f);
            const float noise = (random.nextFloat() * 2.0f - 1.0f);

            data[startSample + i] += noise * envelope * 0.15f;
        }
    }

    void generateSynthChord()
    {
        // Chord stab: C major (C4, E4, G4) - 500ms with filter sweep
        const float duration = 0.5f;
        const int numSamples = static_cast<int>(currentSampleRate * duration);

        synthChordBuffer.setSize(1, numSamples);
        synthChordBuffer.clear();

        auto* data = synthChordBuffer.getWritePointer(0);

        // Frequencies for C major chord (C4, E4, G4)
        const float frequencies[] = { 261.63f, 329.63f, 392.00f };
        float phases[] = { 0.0f, 0.0f, 0.0f };

        for (int i = 0; i < numSamples; ++i)
        {
            const float t = static_cast<float>(i) / numSamples;

            // ADSR-ish envelope
            float envelope;
            if (t < 0.01f)
                envelope = t / 0.01f; // Attack
            else if (t < 0.1f)
                envelope = 1.0f - (t - 0.01f) * 2.0f; // Decay to 0.8
            else
                envelope = 0.8f * std::exp(-(t - 0.1f) * 3.0f); // Release

            float sample = 0.0f;
            for (int note = 0; note < 3; ++note)
            {
                // Saw wave with 3 harmonics
                for (int harmonic = 1; harmonic <= 3; ++harmonic)
                {
                    const float harmonicFreq = frequencies[note] * harmonic;
                    const float harmonicAmp = 1.0f / harmonic;
                    sample += std::sin(phases[note] * harmonic) * harmonicAmp;
                }

                phases[note] += 2.0f * juce::MathConstants<float>::pi * frequencies[note] / currentSampleRate;
            }

            // Simple lowpass effect (moving average) for warmth
            data[i] = sample * envelope * 0.2f;
        }

        // Apply simple smoothing
        for (int i = 1; i < numSamples; ++i)
            data[i] = data[i] * 0.7f + data[i - 1] * 0.3f;
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TestToneGenerator)
};
