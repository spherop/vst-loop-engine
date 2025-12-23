#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <cmath>

class TestToneGenerator
{
public:
    enum class SoundType
    {
        Click,          // 0 - Crisp rimshot/sidestick
        DrumLoop,       // 1 - Funky breakbeat-style pattern
        SynthPad,       // 2 - Lush analog pad
        ElectricGuitar, // 3 - Vintage electric riff
        BassGroove,     // 4 - Funky bass line
        PianoChord,     // 5 - Jazz piano chord
        VocalPhrase,    // 6 - Synth vocal "ooh"
        Percussion,     // 7 - Conga/bongo pattern
        AmbientTexture, // 8 - Evolving pad texture
        NoiseBurst      // 9 - White noise burst for testing
    };

    TestToneGenerator() = default;

    void prepare(double sampleRate, int samplesPerBlock)
    {
        currentSampleRate = sampleRate;
        maxBlockSize = samplesPerBlock;

        // Pre-generate all test sounds
        generateClick();
        generateDrumLoop();
        generateSynthPad();
        generateElectricGuitar();
        generateBassGroove();
        generatePianoChord();
        generateVocalPhrase();
        generatePercussion();
        generateAmbientTexture();
        generateNoiseBurst();

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

        const auto* sourceBuffer = getBufferForSound(static_cast<SoundType>(currentSound.load()));
        if (sourceBuffer == nullptr || sourceBuffer->getNumSamples() == 0)
            return;

        const int numSamples = buffer.getNumSamples();
        const int numChannels = buffer.getNumChannels();
        const int sourceLength = sourceBuffer->getNumSamples();
        const int sourceChannels = sourceBuffer->getNumChannels();
        int pos = playbackPosition.load();

        for (int sample = 0; sample < numSamples; ++sample)
        {
            if (pos >= sourceLength)
            {
                // Loop if enabled OR if it's the drum loop (backward compatibility)
                if (loopEnabled.load() || currentSound.load() == static_cast<int>(SoundType::DrumLoop))
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
    juce::AudioBuffer<float> synthPadBuffer;
    juce::AudioBuffer<float> electricGuitarBuffer;
    juce::AudioBuffer<float> bassGrooveBuffer;
    juce::AudioBuffer<float> pianoChordBuffer;
    juce::AudioBuffer<float> vocalPhraseBuffer;
    juce::AudioBuffer<float> percussionBuffer;
    juce::AudioBuffer<float> ambientTextureBuffer;
    juce::AudioBuffer<float> noiseBurstBuffer;

    std::atomic<int> currentSound { 0 };
    std::atomic<int> playbackPosition { 0 };
    std::atomic<bool> isPlaying { false };
    std::atomic<bool> loopEnabled { false };

    const juce::AudioBuffer<float>* getBufferForSound(SoundType type) const
    {
        switch (type)
        {
            case SoundType::Click:          return &clickBuffer;
            case SoundType::DrumLoop:       return &drumLoopBuffer;
            case SoundType::SynthPad:       return &synthPadBuffer;
            case SoundType::ElectricGuitar: return &electricGuitarBuffer;
            case SoundType::BassGroove:     return &bassGrooveBuffer;
            case SoundType::PianoChord:     return &pianoChordBuffer;
            case SoundType::VocalPhrase:    return &vocalPhraseBuffer;
            case SoundType::Percussion:     return &percussionBuffer;
            case SoundType::AmbientTexture: return &ambientTextureBuffer;
            case SoundType::NoiseBurst:     return &noiseBurstBuffer;
            default: return nullptr;
        }
    }

    // =========================================================================
    // 0. CLICK - Professional rimshot/sidestick
    // =========================================================================
    void generateClick()
    {
        const int numSamples = static_cast<int>(currentSampleRate * 0.15);
        clickBuffer.setSize(2, numSamples);
        clickBuffer.clear();

        auto* dataL = clickBuffer.getWritePointer(0);
        auto* dataR = clickBuffer.getWritePointer(1);

        juce::Random random(12345);

        for (int i = 0; i < numSamples; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(currentSampleRate);
            float sample = 0.0f;

            if (t < 0.002f)
            {
                const float crackEnv = 1.0f - (t / 0.002f);
                sample += (random.nextFloat() * 2.0f - 1.0f) * crackEnv * crackEnv * 0.9f;
            }

            const float bodyEnv = std::exp(-t * 60.0f);
            sample += std::sin(2.0f * juce::MathConstants<float>::pi * 2500.0f * t) * bodyEnv * 0.4f;
            sample += std::sin(2.0f * juce::MathConstants<float>::pi * 3200.0f * t) * bodyEnv * 0.25f;

            const float rimEnv = std::exp(-t * 35.0f);
            sample += std::sin(2.0f * juce::MathConstants<float>::pi * 4800.0f * t) * rimEnv * 0.2f;

            const float thumpEnv = std::exp(-t * 80.0f);
            sample += std::sin(2.0f * juce::MathConstants<float>::pi * 180.0f * t) * thumpEnv * 0.35f;

            dataL[i] = sample * 0.7f;
            dataR[i] = sample * 0.7f;
        }
    }

    // =========================================================================
    // 1. DRUM LOOP - Funky breakbeat
    // =========================================================================
    void generateDrumLoop()
    {
        const float bpm = 95.0f;
        const float beatsPerLoop = 8.0f;
        const float loopDuration = (60.0f / bpm) * beatsPerLoop;
        const int numSamples = static_cast<int>(currentSampleRate * loopDuration);

        drumLoopBuffer.setSize(2, numSamples);
        drumLoopBuffer.clear();

        auto* dataL = drumLoopBuffer.getWritePointer(0);
        auto* dataR = drumLoopBuffer.getWritePointer(1);

        const float samplesPerBeat = static_cast<float>(currentSampleRate) * 60.0f / bpm;

        addPunchyKick(dataL, dataR, 0, numSamples);
        addPunchyKick(dataL, dataR, static_cast<int>(1.5f * samplesPerBeat), numSamples);
        addPunchyKick(dataL, dataR, static_cast<int>(4.0f * samplesPerBeat), numSamples);
        addPunchyKick(dataL, dataR, static_cast<int>(5.5f * samplesPerBeat), numSamples);

        addFatSnare(dataL, dataR, static_cast<int>(2.0f * samplesPerBeat), numSamples, 1.0f);
        addFatSnare(dataL, dataR, static_cast<int>(6.0f * samplesPerBeat), numSamples, 1.0f);
        addFatSnare(dataL, dataR, static_cast<int>(1.75f * samplesPerBeat), numSamples, 0.25f);
        addFatSnare(dataL, dataR, static_cast<int>(3.5f * samplesPerBeat), numSamples, 0.3f);
        addFatSnare(dataL, dataR, static_cast<int>(5.75f * samplesPerBeat), numSamples, 0.25f);

        for (int i = 0; i < 16; ++i)
        {
            float beatPos = static_cast<float>(i) * 0.5f;
            if (i % 2 == 1) beatPos += 0.12f;
            const int samplePos = static_cast<int>(beatPos * samplesPerBeat);
            const float velocity = (i % 2 == 0) ? 0.6f : 0.35f;
            addCrispHiHat(dataL, dataR, samplePos, numSamples, velocity, i % 4 == 3);
        }

        const float maxLevel = std::max(
            drumLoopBuffer.getMagnitude(0, 0, numSamples),
            drumLoopBuffer.getMagnitude(1, 0, numSamples));
        if (maxLevel > 0.0f)
            drumLoopBuffer.applyGain(0.85f / maxLevel);
    }

    void addPunchyKick(float* dataL, float* dataR, int startSample, int bufferLength)
    {
        const int kickLength = static_cast<int>(currentSampleRate * 0.25);
        float phase = 0.0f;

        for (int i = 0; i < kickLength && (startSample + i) < bufferLength; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(currentSampleRate);
            const float subFreq = 80.0f * std::exp(-t * 8.0f) + 45.0f;
            const float subEnv = std::exp(-t * 12.0f);
            phase += 2.0f * juce::MathConstants<float>::pi * subFreq / static_cast<float>(currentSampleRate);
            float sample = std::sin(phase) * subEnv * 0.8f;

            const float punchFreq = 180.0f * std::exp(-t * 15.0f) + 80.0f;
            const float punchEnv = std::exp(-t * 25.0f);
            sample += std::sin(phase * (punchFreq / subFreq)) * punchEnv * 0.4f;

            sample = std::tanh(sample * 1.5f);
            dataL[startSample + i] += sample * 0.75f;
            dataR[startSample + i] += sample * 0.75f;
        }
    }

    void addFatSnare(float* dataL, float* dataR, int startSample, int bufferLength, float velocity)
    {
        const int snareLength = static_cast<int>(currentSampleRate * 0.18);
        juce::Random random(static_cast<juce::int64>(startSample) + 42);
        float noiseZ1 = 0.0f;

        for (int i = 0; i < snareLength && (startSample + i) < bufferLength; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(currentSampleRate);
            float sample = 0.0f;

            const float bodyFreq = 200.0f + 40.0f * std::exp(-t * 50.0f);
            const float bodyEnv = std::exp(-t * 18.0f);
            sample += std::sin(2.0f * juce::MathConstants<float>::pi * bodyFreq * t) * bodyEnv * 0.5f;

            float noise = random.nextFloat() * 2.0f - 1.0f;
            const float hp = noise - noiseZ1 * 0.7f;
            noiseZ1 = noise;
            const float wireEnv = std::exp(-t * 12.0f) * (1.0f - std::exp(-t * 200.0f));
            sample += hp * wireEnv * 0.55f;

            sample *= velocity;
            dataL[startSample + i] += sample * 0.85f;
            dataR[startSample + i] += sample * 0.85f;
        }
    }

    void addCrispHiHat(float* dataL, float* dataR, int startSample, int bufferLength, float velocity, bool open)
    {
        const float hatDuration = open ? 0.15f : 0.04f;
        const int hatLength = static_cast<int>(currentSampleRate * hatDuration);
        juce::Random random(static_cast<juce::int64>(startSample) + 789);
        float z1 = 0.0f, z2 = 0.0f;

        for (int i = 0; i < hatLength && (startSample + i) < bufferLength; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(currentSampleRate);
            float noise = random.nextFloat() * 2.0f - 1.0f;
            const float hp = noise - 1.8f * z1 + 0.85f * z2;
            z2 = z1;
            z1 = noise;

            const float decayRate = open ? 15.0f : 80.0f;
            const float env = std::exp(-t * decayRate) * (1.0f - std::exp(-t * 500.0f));
            float sample = hp * env * velocity * 0.35f;

            const float pan = 0.55f;
            dataL[startSample + i] += sample * (1.0f - pan);
            dataR[startSample + i] += sample * pan;
        }
    }

    // =========================================================================
    // 2. SYNTH PAD - Lush analog-style pad
    // =========================================================================
    void generateSynthPad()
    {
        const float duration = 3.0f;
        const int numSamples = static_cast<int>(currentSampleRate * duration);

        synthPadBuffer.setSize(2, numSamples);
        synthPadBuffer.clear();

        auto* dataL = synthPadBuffer.getWritePointer(0);
        auto* dataR = synthPadBuffer.getWritePointer(1);

        const float baseFreqs[] = { 130.81f, 155.56f, 196.00f, 233.08f };
        const int numNotes = 4;
        const float detune[3] = { -0.08f, 0.0f, 0.07f };
        float phases[4][3] = {};
        float filterStateL = 0.0f, filterStateR = 0.0f;
        float lfoPhase = 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(currentSampleRate);

            float envelope;
            if (t < 0.5f) envelope = t / 0.5f;
            else if (t < 0.8f) envelope = 1.0f - 0.3f * ((t - 0.5f) / 0.3f);
            else if (t < 2.0f) envelope = 0.7f;
            else envelope = 0.7f * std::max(0.0f, 1.0f - (t - 2.0f) / 1.0f);
            envelope = std::max(0.0f, envelope);

            lfoPhase += 2.0f * juce::MathConstants<float>::pi * 0.3f / static_cast<float>(currentSampleRate);
            const float lfo = std::sin(lfoPhase);
            const float baseCutoff = 800.0f + 2000.0f * envelope + 300.0f * lfo;
            const float filterCoeff = std::exp(-2.0f * juce::MathConstants<float>::pi * baseCutoff / static_cast<float>(currentSampleRate));

            float sampleL = 0.0f, sampleR = 0.0f;

            for (int note = 0; note < numNotes; ++note)
            {
                for (int osc = 0; osc < 3; ++osc)
                {
                    const float freq = baseFreqs[note] * std::pow(2.0f, detune[osc] / 1200.0f);
                    phases[note][osc] += 2.0f * juce::MathConstants<float>::pi * freq / static_cast<float>(currentSampleRate);

                    float saw = 0.0f;
                    for (int h = 1; h <= 6; ++h)
                    {
                        if (freq * static_cast<float>(h) > currentSampleRate * 0.45f) break;
                        saw += std::sin(phases[note][osc] * static_cast<float>(h)) / static_cast<float>(h);
                    }
                    saw *= 0.6f;

                    const float pan = 0.5f + (static_cast<float>(osc) - 1.0f) * 0.3f;
                    sampleL += saw * (1.0f - pan);
                    sampleR += saw * pan;
                }
            }

            filterStateL += (1.0f - filterCoeff) * (sampleL - filterStateL);
            filterStateR += (1.0f - filterCoeff) * (sampleR - filterStateR);

            dataL[i] = std::tanh(filterStateL * envelope * 0.3f);
            dataR[i] = std::tanh(filterStateR * envelope * 0.3f);
        }
    }

    // =========================================================================
    // 3. ELECTRIC GUITAR - Clean arpeggiated chord with realistic tone
    // =========================================================================
    void generateElectricGuitar()
    {
        const float duration = 2.5f;
        const int numSamples = static_cast<int>(currentSampleRate * duration);

        electricGuitarBuffer.setSize(2, numSamples);
        electricGuitarBuffer.clear();

        auto* dataL = electricGuitarBuffer.getWritePointer(0);
        auto* dataR = electricGuitarBuffer.getWritePointer(1);

        // Arpeggiated Am7 chord - realistic guitar voicing
        // Staggered timing like a fingerpicked pattern
        struct Note { float freq; float start; float dur; float pan; };
        const Note notes[] = {
            { 110.00f, 0.00f, 2.2f, 0.35f },  // A2 - bass note
            { 164.81f, 0.08f, 2.0f, 0.40f },  // E3
            { 220.00f, 0.16f, 1.8f, 0.50f },  // A3
            { 261.63f, 0.24f, 1.6f, 0.55f },  // C4
            { 329.63f, 0.32f, 1.4f, 0.60f },  // E4
            { 392.00f, 0.40f, 1.2f, 0.65f },  // G4
        };
        const int numNotes = 6;

        for (int n = 0; n < numNotes; ++n)
        {
            const int startSample = static_cast<int>(notes[n].start * currentSampleRate);
            const int noteSamples = static_cast<int>(notes[n].dur * currentSampleRate);
            const float freq = notes[n].freq;

            // Improved Karplus-Strong with better initialization
            const int periodSamples = static_cast<int>(currentSampleRate / freq);
            std::vector<float> delayLine(static_cast<size_t>(periodSamples), 0.0f);

            juce::Random random(static_cast<juce::int64>(n * 1000 + 777));

            // Initialize with shaped noise (pick position simulation)
            // Pick closer to bridge = more harmonics
            const float pickPos = 0.13f; // 13% from bridge
            for (int i = 0; i < periodSamples; ++i)
            {
                float pos = static_cast<float>(i) / static_cast<float>(periodSamples);
                // Create a pluck shape - triangular with pick position
                float pluck;
                if (pos < pickPos)
                    pluck = pos / pickPos;
                else
                    pluck = (1.0f - pos) / (1.0f - pickPos);

                // Add some noise for realism
                float noise = random.nextFloat() * 0.3f - 0.15f;
                delayLine[static_cast<size_t>(i)] = (pluck + noise) * 0.8f;
            }

            int readIdx = 0;
            float prevSample = 0.0f;
            float prevPrevSample = 0.0f;

            // Lowpass filter state for body resonance
            float bodyFilter = 0.0f;

            for (int i = 0; i < noteSamples && (startSample + i) < numSamples; ++i)
            {
                const float t = static_cast<float>(i) / static_cast<float>(currentSampleRate);

                // Read from delay line
                float current = delayLine[static_cast<size_t>(readIdx)];

                // Improved lowpass filter (smoother decay, more realistic)
                // Two-point average with adjustable damping
                const float damping = 0.996f - (freq / 20000.0f) * 0.01f; // Higher notes decay faster
                const float brightness = 0.5f + 0.3f * std::exp(-t * 2.0f); // Brightness decreases over time

                float filtered = brightness * current + (1.0f - brightness) * prevSample;
                filtered = filtered * damping;

                // Store back
                delayLine[static_cast<size_t>(readIdx)] = filtered;
                prevPrevSample = prevSample;
                prevSample = current;

                readIdx = (readIdx + 1) % periodSamples;

                // Envelope - guitar string decay
                float env = std::exp(-t * 1.8f);

                // Quick attack
                if (t < 0.002f)
                    env *= t / 0.002f;

                float sample = current * env;

                // Add body resonance (lowpassed version for warmth)
                bodyFilter += 0.05f * (sample - bodyFilter);
                sample = sample * 0.7f + bodyFilter * 0.3f;

                // Gentle tube-style saturation
                sample = std::tanh(sample * 1.5f) * 0.65f;

                // Stereo placement based on string
                const float pan = notes[n].pan;
                dataL[startSample + i] += sample * (1.0f - pan) * 0.7f;
                dataR[startSample + i] += sample * pan * 0.7f;
            }
        }

        // Add a subtle room reverb tail (simple comb filter)
        const int reverbDelay = static_cast<int>(currentSampleRate * 0.031f);
        float reverbL = 0.0f, reverbR = 0.0f;
        std::vector<float> reverbBufL(static_cast<size_t>(reverbDelay), 0.0f);
        std::vector<float> reverbBufR(static_cast<size_t>(reverbDelay), 0.0f);
        int reverbIdx = 0;

        for (int i = 0; i < numSamples; ++i)
        {
            float dryL = dataL[i];
            float dryR = dataR[i];

            float delayedL = reverbBufL[static_cast<size_t>(reverbIdx)];
            float delayedR = reverbBufR[static_cast<size_t>(reverbIdx)];

            reverbBufL[static_cast<size_t>(reverbIdx)] = dryL + delayedL * 0.3f;
            reverbBufR[static_cast<size_t>(reverbIdx)] = dryR + delayedR * 0.3f;

            dataL[i] = dryL + delayedL * 0.15f;
            dataR[i] = dryR + delayedR * 0.15f;

            reverbIdx = (reverbIdx + 1) % reverbDelay;
        }

        // Normalize
        const float maxLevel = std::max(
            electricGuitarBuffer.getMagnitude(0, 0, numSamples),
            electricGuitarBuffer.getMagnitude(1, 0, numSamples));
        if (maxLevel > 0.0f)
            electricGuitarBuffer.applyGain(0.75f / maxLevel);
    }

    // =========================================================================
    // 4. BASS GROOVE - Funky slap bass line
    // =========================================================================
    void generateBassGroove()
    {
        const float duration = 2.0f;
        const int numSamples = static_cast<int>(currentSampleRate * duration);

        bassGrooveBuffer.setSize(2, numSamples);
        bassGrooveBuffer.clear();

        auto* dataL = bassGrooveBuffer.getWritePointer(0);
        auto* dataR = bassGrooveBuffer.getWritePointer(1);

        // Funky bass pattern: E1, G1, A1, rest, E1, E1 (octave), G1
        struct Note { float freq; float start; float dur; bool slap; };
        const Note notes[] = {
            { 41.2f,  0.0f,   0.18f, true },
            { 49.0f,  0.25f,  0.15f, false },
            { 55.0f,  0.5f,   0.20f, false },
            { 41.2f,  0.85f,  0.12f, true },
            { 82.4f,  1.0f,   0.15f, false },
            { 49.0f,  1.25f,  0.25f, false },
            { 41.2f,  1.6f,   0.30f, true },
        };
        const int numNotes = 7;

        for (int n = 0; n < numNotes; ++n)
        {
            const int startSample = static_cast<int>(notes[n].start * currentSampleRate);
            const int noteSamples = static_cast<int>(notes[n].dur * currentSampleRate);

            float phase = 0.0f;
            const float freq = notes[n].freq;

            for (int i = 0; i < noteSamples && (startSample + i) < numSamples; ++i)
            {
                const float t = static_cast<float>(i) / static_cast<float>(currentSampleRate);

                // Envelope: fast attack, moderate decay
                float env;
                if (t < 0.005f) env = t / 0.005f;
                else env = std::exp(-t * 8.0f) * 0.7f + 0.3f * std::exp(-t * 2.0f);

                // Pitch envelope for slap
                float pitchEnv = 1.0f;
                if (notes[n].slap && t < 0.02f)
                    pitchEnv = 1.0f + (1.0f - t / 0.02f) * 0.5f;

                phase += 2.0f * juce::MathConstants<float>::pi * freq * pitchEnv / static_cast<float>(currentSampleRate);

                // Bass with harmonics
                float sample = std::sin(phase) * 0.6f;
                sample += std::sin(phase * 2.0f) * 0.25f * env;
                sample += std::sin(phase * 3.0f) * 0.1f * env;

                // Slap pop transient
                if (notes[n].slap && t < 0.01f)
                {
                    const float popEnv = 1.0f - t / 0.01f;
                    sample += std::sin(phase * 8.0f) * popEnv * popEnv * 0.4f;
                }

                sample *= env;
                sample = std::tanh(sample * 1.5f) * 0.7f;

                dataL[startSample + i] += sample;
                dataR[startSample + i] += sample;
            }
        }
    }

    // =========================================================================
    // 5. PIANO CHORD - Jazz voicing
    // =========================================================================
    void generatePianoChord()
    {
        const float duration = 2.5f;
        const int numSamples = static_cast<int>(currentSampleRate * duration);

        pianoChordBuffer.setSize(2, numSamples);
        pianoChordBuffer.clear();

        auto* dataL = pianoChordBuffer.getWritePointer(0);
        auto* dataR = pianoChordBuffer.getWritePointer(1);

        // Cmaj9 voicing: C3, E3, G3, B3, D4
        const float freqs[] = { 130.81f, 164.81f, 196.0f, 246.94f, 293.66f };
        const int numNotes = 5;

        for (int n = 0; n < numNotes; ++n)
        {
            float phase = 0.0f;
            const float freq = freqs[n];

            for (int i = 0; i < numSamples; ++i)
            {
                const float t = static_cast<float>(i) / static_cast<float>(currentSampleRate);

                // Piano-like envelope
                float env;
                if (t < 0.01f) env = t / 0.01f;
                else env = std::exp(-t * 1.5f);

                phase += 2.0f * juce::MathConstants<float>::pi * freq / static_cast<float>(currentSampleRate);

                // Piano harmonics (hammer strike characteristic)
                float sample = 0.0f;
                sample += std::sin(phase) * 0.5f;
                sample += std::sin(phase * 2.0f) * 0.25f * std::exp(-t * 3.0f);
                sample += std::sin(phase * 3.0f) * 0.15f * std::exp(-t * 4.0f);
                sample += std::sin(phase * 4.0f) * 0.08f * std::exp(-t * 5.0f);
                sample += std::sin(phase * 5.0f) * 0.04f * std::exp(-t * 6.0f);

                // Slight inharmonicity
                sample += std::sin(phase * 2.01f) * 0.02f * std::exp(-t * 3.0f);

                sample *= env * 0.15f;

                // Stereo placement based on pitch
                const float pan = 0.3f + static_cast<float>(n) * 0.1f;
                dataL[i] += sample * (1.0f - pan);
                dataR[i] += sample * pan;
            }
        }
    }

    // =========================================================================
    // 6. VOCAL PHRASE - Synth "ooh" sound
    // =========================================================================
    void generateVocalPhrase()
    {
        const float duration = 2.0f;
        const int numSamples = static_cast<int>(currentSampleRate * duration);

        vocalPhraseBuffer.setSize(2, numSamples);
        vocalPhraseBuffer.clear();

        auto* dataL = vocalPhraseBuffer.getWritePointer(0);
        auto* dataR = vocalPhraseBuffer.getWritePointer(1);

        const float baseFreq = 220.0f; // A3
        float phase = 0.0f;

        // Formant frequencies for "ooh" vowel
        const float formant1 = 300.0f;
        const float formant2 = 870.0f;
        const float formant3 = 2240.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(currentSampleRate);

            // Envelope with vibrato
            float env;
            if (t < 0.3f) env = t / 0.3f;
            else if (t < 1.5f) env = 1.0f;
            else env = std::max(0.0f, 1.0f - (t - 1.5f) / 0.5f);

            // Vibrato
            const float vibrato = 1.0f + 0.015f * std::sin(t * 25.0f) * std::min(1.0f, t / 0.5f);

            phase += 2.0f * juce::MathConstants<float>::pi * baseFreq * vibrato / static_cast<float>(currentSampleRate);

            // Generate harmonics
            float sample = 0.0f;
            for (int h = 1; h <= 20; ++h)
            {
                const float harmFreq = baseFreq * static_cast<float>(h);
                if (harmFreq > currentSampleRate * 0.4f) break;

                // Apply formant shaping
                float formantGain = 0.0f;
                formantGain += std::exp(-std::pow((harmFreq - formant1) / 80.0f, 2.0f));
                formantGain += std::exp(-std::pow((harmFreq - formant2) / 120.0f, 2.0f)) * 0.5f;
                formantGain += std::exp(-std::pow((harmFreq - formant3) / 200.0f, 2.0f)) * 0.3f;

                sample += std::sin(phase * static_cast<float>(h)) * formantGain / static_cast<float>(h);
            }

            sample *= env * 0.3f;

            dataL[i] = sample;
            dataR[i] = sample;
        }
    }

    // =========================================================================
    // 7. PERCUSSION - Conga pattern
    // =========================================================================
    void generatePercussion()
    {
        const float duration = 2.0f;
        const int numSamples = static_cast<int>(currentSampleRate * duration);

        percussionBuffer.setSize(2, numSamples);
        percussionBuffer.clear();

        auto* dataL = percussionBuffer.getWritePointer(0);
        auto* dataR = percussionBuffer.getWritePointer(1);

        // Conga pattern timings and types (0=low, 1=high, 2=slap)
        struct Hit { float time; int type; float vel; };
        const Hit hits[] = {
            { 0.0f,   0, 1.0f },
            { 0.25f,  1, 0.7f },
            { 0.5f,   2, 0.9f },
            { 0.75f,  1, 0.6f },
            { 1.0f,   0, 1.0f },
            { 1.2f,   1, 0.5f },
            { 1.35f,  1, 0.6f },
            { 1.5f,   2, 0.85f },
            { 1.75f,  0, 0.7f },
        };
        const int numHits = 9;

        for (int h = 0; h < numHits; ++h)
        {
            const int startSample = static_cast<int>(hits[h].time * currentSampleRate);
            const float baseFreq = (hits[h].type == 0) ? 200.0f : ((hits[h].type == 1) ? 280.0f : 350.0f);
            const int hitLength = static_cast<int>(currentSampleRate * 0.2);

            juce::Random random(startSample);

            for (int i = 0; i < hitLength && (startSample + i) < numSamples; ++i)
            {
                const float t = static_cast<float>(i) / static_cast<float>(currentSampleRate);

                // Pitch drop
                const float freq = baseFreq * (1.0f + 0.5f * std::exp(-t * 50.0f));
                const float env = std::exp(-t * 20.0f);

                float sample = std::sin(2.0f * juce::MathConstants<float>::pi * freq * t) * env;

                // Add click for slap
                if (hits[h].type == 2 && t < 0.003f)
                {
                    sample += (random.nextFloat() * 2.0f - 1.0f) * (1.0f - t / 0.003f) * 0.5f;
                }

                sample *= hits[h].vel * 0.6f;

                // Stereo placement
                const float pan = 0.4f + hits[h].type * 0.15f;
                dataL[startSample + i] += sample * (1.0f - pan);
                dataR[startSample + i] += sample * pan;
            }
        }
    }

    // =========================================================================
    // 8. AMBIENT TEXTURE - Evolving pad with noise
    // =========================================================================
    void generateAmbientTexture()
    {
        const float duration = 4.0f;
        const int numSamples = static_cast<int>(currentSampleRate * duration);

        ambientTextureBuffer.setSize(2, numSamples);
        ambientTextureBuffer.clear();

        auto* dataL = ambientTextureBuffer.getWritePointer(0);
        auto* dataR = ambientTextureBuffer.getWritePointer(1);

        juce::Random random(999);
        float filterL = 0.0f, filterR = 0.0f;
        float lfo1 = 0.0f, lfo2 = 0.0f;
        float phase1 = 0.0f, phase2 = 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(currentSampleRate);

            // Slow envelope
            float env;
            if (t < 1.5f) env = t / 1.5f;
            else if (t < 3.0f) env = 1.0f;
            else env = std::max(0.0f, 1.0f - (t - 3.0f) / 1.0f);

            // LFOs
            lfo1 += 2.0f * juce::MathConstants<float>::pi * 0.1f / static_cast<float>(currentSampleRate);
            lfo2 += 2.0f * juce::MathConstants<float>::pi * 0.07f / static_cast<float>(currentSampleRate);

            // Two drones
            const float freq1 = 110.0f + std::sin(lfo1) * 5.0f;
            const float freq2 = 165.0f + std::sin(lfo2) * 7.0f;

            phase1 += 2.0f * juce::MathConstants<float>::pi * freq1 / static_cast<float>(currentSampleRate);
            phase2 += 2.0f * juce::MathConstants<float>::pi * freq2 / static_cast<float>(currentSampleRate);

            float sampleL = std::sin(phase1) * 0.3f + std::sin(phase2) * 0.2f;
            float sampleR = std::sin(phase1 + 0.5f) * 0.3f + std::sin(phase2 + 0.3f) * 0.2f;

            // Add filtered noise
            const float noise = random.nextFloat() * 2.0f - 1.0f;
            const float cutoff = 500.0f + 300.0f * std::sin(lfo1 * 0.5f);
            const float coeff = std::exp(-2.0f * juce::MathConstants<float>::pi * cutoff / static_cast<float>(currentSampleRate));

            filterL += (1.0f - coeff) * (noise * 0.1f - filterL);
            filterR += (1.0f - coeff) * (noise * 0.1f - filterR);

            sampleL += filterL;
            sampleR += filterR;

            dataL[i] = sampleL * env * 0.5f;
            dataR[i] = sampleR * env * 0.5f;
        }
    }

    // =========================================================================
    // 9. NOISE BURST - White noise for testing transients
    // =========================================================================
    void generateNoiseBurst()
    {
        const float duration = 0.5f;
        const int numSamples = static_cast<int>(currentSampleRate * duration);

        noiseBurstBuffer.setSize(2, numSamples);
        noiseBurstBuffer.clear();

        auto* dataL = noiseBurstBuffer.getWritePointer(0);
        auto* dataR = noiseBurstBuffer.getWritePointer(1);

        juce::Random random(12345);

        for (int i = 0; i < numSamples; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(currentSampleRate);

            // Fast attack, exponential decay
            float env;
            if (t < 0.001f) env = t / 0.001f;
            else env = std::exp(-t * 10.0f);

            const float noiseL = random.nextFloat() * 2.0f - 1.0f;
            const float noiseR = random.nextFloat() * 2.0f - 1.0f;

            dataL[i] = noiseL * env * 0.7f;
            dataR[i] = noiseR * env * 0.7f;
        }
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TestToneGenerator)
};
