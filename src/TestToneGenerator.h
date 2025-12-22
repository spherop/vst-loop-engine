#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <atomic>

class TestToneGenerator
{
public:
    enum class SoundType
    {
        Click,       // Short percussive transient
        DrumLoop,    // Simple kick/snare pattern
        SynthChord,  // Filtered chord stab
        GuitarChord  // Electric guitar power chord
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
        generateGuitarChord();

        isPrepared = true;
    }

    void trigger(SoundType type)
    {
        if (!isPrepared)
            return;

        currentSound.store(static_cast<int>(type));
        playbackPosition.store(0);
        isPlaying.store(true);
    }

    void stop()
    {
        isPlaying.store(false);
        playbackPosition.store(0);
    }

    void processBlock(juce::AudioBuffer<float>& buffer)
    {
        if (!isPlaying.load())
            return;

        const auto* sourceBuffer = getBufferForSound(static_cast<SoundType>(currentSound.load()));
        if (sourceBuffer == nullptr || sourceBuffer->getNumSamples() == 0)
            return;

        const int numSamples = buffer.getNumSamples();
        const int numChannels = buffer.getNumChannels();
        const int sourceLength = sourceBuffer->getNumSamples();
        int pos = playbackPosition.load();

        for (int sample = 0; sample < numSamples; ++sample)
        {
            if (pos >= sourceLength)
            {
                // Loop drum pattern, stop others
                if (currentSound.load() == static_cast<int>(SoundType::DrumLoop))
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
                const int sourceChannel = std::min(channel, sourceBuffer->getNumChannels() - 1);
                buffer.addSample(channel, sample,
                    sourceBuffer->getSample(sourceChannel, pos));
            }

            ++pos;
        }

        playbackPosition.store(pos);
    }

    bool getIsPlaying() const { return isPlaying.load(); }

private:
    double currentSampleRate = 44100.0;
    int maxBlockSize = 512;
    bool isPrepared = false;

    juce::AudioBuffer<float> clickBuffer;
    juce::AudioBuffer<float> drumLoopBuffer;
    juce::AudioBuffer<float> synthChordBuffer;
    juce::AudioBuffer<float> guitarChordBuffer;

    std::atomic<int> currentSound { 0 };
    std::atomic<int> playbackPosition { 0 };
    std::atomic<bool> isPlaying { false };

    const juce::AudioBuffer<float>* getBufferForSound(SoundType type) const
    {
        switch (type)
        {
            case SoundType::Click:       return &clickBuffer;
            case SoundType::DrumLoop:    return &drumLoopBuffer;
            case SoundType::SynthChord:  return &synthChordBuffer;
            case SoundType::GuitarChord: return &guitarChordBuffer;
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

    void generateGuitarChord()
    {
        // Electric guitar power chord (E5) with distortion - 1.5 seconds
        const float duration = 1.5f;
        const int numSamples = static_cast<int>(currentSampleRate * duration);

        guitarChordBuffer.setSize(1, numSamples);
        guitarChordBuffer.clear();

        auto* data = guitarChordBuffer.getWritePointer(0);

        // Power chord frequencies: E2 (82.41 Hz) and B2 (123.47 Hz)
        const float freq1 = 82.41f;  // E2 - root
        const float freq2 = 123.47f; // B2 - fifth

        float phase1 = 0.0f;
        float phase2 = 0.0f;

        // Karplus-Strong inspired pluck with feedback
        const int pluckLength = static_cast<int>(currentSampleRate / freq1);
        std::vector<float> pluckBuffer(pluckLength, 0.0f);

        // Initialize with noise burst for pluck
        juce::Random random(777);
        for (int i = 0; i < pluckLength; ++i)
            pluckBuffer[i] = random.nextFloat() * 2.0f - 1.0f;

        int pluckIndex = 0;
        float lastPluckSample = 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            const float t = static_cast<float>(i) / numSamples;

            // Guitar-like envelope: quick attack, slow decay
            float envelope;
            if (t < 0.005f)
                envelope = t / 0.005f;
            else
                envelope = std::exp(-(t - 0.005f) * 2.0f);

            // Karplus-Strong for realistic string sound
            const float currentPluck = pluckBuffer[pluckIndex];
            const float filtered = 0.5f * (currentPluck + lastPluckSample);
            pluckBuffer[pluckIndex] = filtered * 0.996f; // Decay factor
            lastPluckSample = currentPluck;
            pluckIndex = (pluckIndex + 1) % pluckLength;

            // Add harmonics for power chord character
            float sample = filtered * 0.6f;

            // Add fundamental and fifth with saw-like harmonics
            const float pi = juce::MathConstants<float>::pi;
            for (int h = 1; h <= 4; ++h)
            {
                const float harmonicAmp = 0.3f / h;
                sample += std::sin(phase1 * h) * harmonicAmp;
                sample += std::sin(phase2 * h) * harmonicAmp * 0.7f;
            }

            phase1 += 2.0f * pi * freq1 / static_cast<float>(currentSampleRate);
            phase2 += 2.0f * pi * freq2 / static_cast<float>(currentSampleRate);

            // Apply soft distortion for electric guitar character
            sample = sample * envelope;
            sample = std::tanh(sample * 2.5f) * 0.7f;

            data[i] = sample;
        }

        // Apply cabinet simulation (simple lowpass)
        float cabState = 0.0f;
        const float cabCoeff = 0.15f;
        for (int i = 0; i < numSamples; ++i)
        {
            cabState += cabCoeff * (data[i] - cabState);
            data[i] = data[i] * 0.6f + cabState * 0.4f;
        }
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TestToneGenerator)
};
