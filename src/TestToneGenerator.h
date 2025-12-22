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
        Click,       // Crisp rimshot/sidestick
        DrumLoop,    // Funky breakbeat-style pattern
        SynthChord,  // Lush analog pad
        GuitarChord  // Clean electric chord with chorus
    };

    TestToneGenerator() = default;

    void prepare(double sampleRate, int samplesPerBlock)
    {
        currentSampleRate = sampleRate;
        maxBlockSize = samplesPerBlock;

        // Pre-generate all test sounds
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
    juce::AudioBuffer<float> synthChordBuffer;
    juce::AudioBuffer<float> guitarChordBuffer;

    std::atomic<int> currentSound { 0 };
    std::atomic<int> playbackPosition { 0 };
    std::atomic<bool> isPlaying { false };
    std::atomic<bool> loopEnabled { false };

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

    // =========================================================================
    // CLICK - Professional rimshot/sidestick with body and crack
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

            // Initial transient crack (first 2ms)
            if (t < 0.002f)
            {
                const float crackEnv = 1.0f - (t / 0.002f);
                sample += (random.nextFloat() * 2.0f - 1.0f) * crackEnv * crackEnv * 0.9f;
            }

            // Stick body resonance (2-3kHz range)
            const float bodyEnv = std::exp(-t * 60.0f);
            sample += std::sin(2.0f * juce::MathConstants<float>::pi * 2500.0f * t) * bodyEnv * 0.4f;
            sample += std::sin(2.0f * juce::MathConstants<float>::pi * 3200.0f * t) * bodyEnv * 0.25f;

            // Rim resonance (higher pitched ring)
            const float rimEnv = std::exp(-t * 35.0f);
            sample += std::sin(2.0f * juce::MathConstants<float>::pi * 4800.0f * t) * rimEnv * 0.2f;

            // Low body thump
            const float thumpEnv = std::exp(-t * 80.0f);
            sample += std::sin(2.0f * juce::MathConstants<float>::pi * 180.0f * t) * thumpEnv * 0.35f;

            // Slight stereo width
            dataL[i] = sample * 0.7f;
            dataR[i] = sample * 0.7f;

            // Add subtle stereo spread with slight delay
            if (i > 5)
            {
                dataR[i] += dataL[i - 5] * 0.1f;
            }
        }
    }

    // =========================================================================
    // DRUM LOOP - Funky breakbeat with ghost notes and swing
    // =========================================================================
    void generateDrumLoop()
    {
        const float bpm = 95.0f; // Funky tempo
        const float beatsPerLoop = 8.0f;
        const float loopDuration = (60.0f / bpm) * beatsPerLoop;
        const int numSamples = static_cast<int>(currentSampleRate * loopDuration);

        drumLoopBuffer.setSize(2, numSamples);
        drumLoopBuffer.clear();

        auto* dataL = drumLoopBuffer.getWritePointer(0);
        auto* dataR = drumLoopBuffer.getWritePointer(1);

        const float samplesPerBeat = static_cast<float>(currentSampleRate) * 60.0f / bpm;
        const float swing = 0.12f; // Swing amount (0-0.5)

        // Funky pattern inspired by classic breaks
        // Beat positions: K=Kick, S=Snare, H=HiHat, G=Ghost snare

        // Kicks: 1, 1.5+, 3, 4.5
        addPunchyKick(dataL, dataR, 0, numSamples);
        addPunchyKick(dataL, dataR, static_cast<int>(1.5f * samplesPerBeat), numSamples);
        addPunchyKick(dataL, dataR, static_cast<int>(4.0f * samplesPerBeat), numSamples);
        addPunchyKick(dataL, dataR, static_cast<int>(5.5f * samplesPerBeat), numSamples);

        // Main snares on 2 and 4 (backbeat)
        addFatSnare(dataL, dataR, static_cast<int>(2.0f * samplesPerBeat), numSamples, 1.0f);
        addFatSnare(dataL, dataR, static_cast<int>(6.0f * samplesPerBeat), numSamples, 1.0f);

        // Ghost notes for groove
        addFatSnare(dataL, dataR, static_cast<int>(1.75f * samplesPerBeat), numSamples, 0.25f);
        addFatSnare(dataL, dataR, static_cast<int>(3.5f * samplesPerBeat), numSamples, 0.3f);
        addFatSnare(dataL, dataR, static_cast<int>(5.75f * samplesPerBeat), numSamples, 0.25f);
        addFatSnare(dataL, dataR, static_cast<int>(7.25f * samplesPerBeat), numSamples, 0.35f);

        // Hi-hats with swing
        for (int i = 0; i < 16; ++i)
        {
            float beatPos = static_cast<float>(i) * 0.5f;
            // Add swing to off-beats
            if (i % 2 == 1)
                beatPos += swing;

            const int samplePos = static_cast<int>(beatPos * samplesPerBeat);
            const float velocity = (i % 2 == 0) ? 0.6f : 0.35f; // Accent downbeats
            addCrispHiHat(dataL, dataR, samplePos, numSamples, velocity, i % 4 == 3);
        }

        // Normalize
        const float maxLevel = std::max(
            drumLoopBuffer.getMagnitude(0, 0, numSamples),
            drumLoopBuffer.getMagnitude(1, 0, numSamples));
        if (maxLevel > 0.0f)
            drumLoopBuffer.applyGain(0.85f / maxLevel);
    }

    void addPunchyKick(float* dataL, float* dataR, int startSample, int bufferLength)
    {
        const int kickLength = static_cast<int>(currentSampleRate * 0.25);

        // Layered kick: sub + punch + click
        float phase = 0.0f;
        float clickPhase = 0.0f;

        for (int i = 0; i < kickLength && (startSample + i) < bufferLength; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(currentSampleRate);

            // Sub bass layer (pitch drop from 80Hz to 45Hz)
            const float subFreq = 80.0f * std::exp(-t * 8.0f) + 45.0f;
            const float subEnv = std::exp(-t * 12.0f);
            phase += 2.0f * juce::MathConstants<float>::pi * subFreq / static_cast<float>(currentSampleRate);
            float sample = std::sin(phase) * subEnv * 0.8f;

            // Punch layer (100-200Hz with faster decay)
            const float punchFreq = 180.0f * std::exp(-t * 15.0f) + 80.0f;
            const float punchEnv = std::exp(-t * 25.0f);
            sample += std::sin(phase * (punchFreq / subFreq)) * punchEnv * 0.4f;

            // Click transient (first 5ms)
            if (t < 0.005f)
            {
                clickPhase += 2.0f * juce::MathConstants<float>::pi * 3500.0f / static_cast<float>(currentSampleRate);
                const float clickEnv = 1.0f - (t / 0.005f);
                sample += std::sin(clickPhase) * clickEnv * clickEnv * 0.3f;
            }

            // Soft saturation
            sample = std::tanh(sample * 1.5f);

            dataL[startSample + i] += sample * 0.75f;
            dataR[startSample + i] += sample * 0.75f;
        }
    }

    void addFatSnare(float* dataL, float* dataR, int startSample, int bufferLength, float velocity)
    {
        const int snareLength = static_cast<int>(currentSampleRate * 0.18);
        juce::Random random(static_cast<juce::int64>(startSample) + 42);

        // Simple biquad state for noise filtering
        float noiseZ1 = 0.0f, noiseZ2 = 0.0f;

        for (int i = 0; i < snareLength && (startSample + i) < bufferLength; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(currentSampleRate);
            float sample = 0.0f;

            // Body tone (180-220Hz with pitch bend)
            const float bodyFreq = 200.0f + 40.0f * std::exp(-t * 50.0f);
            const float bodyEnv = std::exp(-t * 18.0f);
            sample += std::sin(2.0f * juce::MathConstants<float>::pi * bodyFreq * t) * bodyEnv * 0.5f;

            // Snare wire noise (bandpass filtered)
            float noise = random.nextFloat() * 2.0f - 1.0f;
            // Simple highpass for snare wire character
            const float hp = noise - noiseZ1 * 0.7f;
            noiseZ1 = noise;
            const float wireEnv = std::exp(-t * 12.0f) * (1.0f - std::exp(-t * 200.0f));
            sample += hp * wireEnv * 0.55f;

            // Snare rattle (longer tail)
            const float rattleEnv = std::exp(-t * 8.0f) * (1.0f - std::exp(-t * 100.0f));
            sample += (random.nextFloat() * 2.0f - 1.0f) * rattleEnv * 0.2f;

            // Initial transient pop
            if (t < 0.003f)
            {
                const float popEnv = 1.0f - (t / 0.003f);
                sample += (random.nextFloat() * 2.0f - 1.0f) * popEnv * popEnv * 0.6f;
            }

            sample *= velocity;

            // Slight stereo spread
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

            // High-frequency metallic noise
            float noise = random.nextFloat() * 2.0f - 1.0f;

            // Resonant highpass filter for metallic character
            const float cutoff = 6000.0f + 2000.0f * std::sin(t * 15000.0f);
            const float w = 2.0f * juce::MathConstants<float>::pi * cutoff / static_cast<float>(currentSampleRate);
            const float hp = noise - 1.8f * z1 + 0.85f * z2;
            z2 = z1;
            z1 = noise;

            // Envelope
            const float decayRate = open ? 15.0f : 80.0f;
            const float env = std::exp(-t * decayRate) * (1.0f - std::exp(-t * 500.0f));

            float sample = hp * env * velocity * 0.35f;

            // Pan slightly for stereo interest
            const float pan = 0.55f;
            dataL[startSample + i] += sample * (1.0f - pan);
            dataR[startSample + i] += sample * pan;
        }
    }

    // =========================================================================
    // SYNTH CHORD - Lush analog-style pad with detuning and filter movement
    // =========================================================================
    void generateSynthChord()
    {
        const float duration = 3.0f;
        const int numSamples = static_cast<int>(currentSampleRate * duration);

        synthChordBuffer.setSize(2, numSamples);
        synthChordBuffer.clear();

        auto* dataL = synthChordBuffer.getWritePointer(0);
        auto* dataR = synthChordBuffer.getWritePointer(1);

        // Cm7 chord for moody vibe: C3, Eb3, G3, Bb3
        const float baseFreqs[] = { 130.81f, 155.56f, 196.00f, 233.08f };
        const int numNotes = 4;
        const int numOscs = 3; // 3 oscillators per note for thickness

        // Oscillator phases and detuning
        float phases[4][3] = {};
        const float detune[3] = { -0.08f, 0.0f, 0.07f }; // Cents of detuning

        // Filter state (simple one-pole lowpass)
        float filterStateL = 0.0f, filterStateR = 0.0f;

        // LFO for subtle movement
        float lfoPhase = 0.0f;
        const float lfoRate = 0.3f;

        for (int i = 0; i < numSamples; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(currentSampleRate);
            const float tNorm = static_cast<float>(i) / static_cast<float>(numSamples);

            // ADSR envelope: 0.5s attack, 0.3s decay, 0.7 sustain, 1.5s release
            float envelope;
            if (t < 0.5f)
                envelope = t / 0.5f; // Attack
            else if (t < 0.8f)
                envelope = 1.0f - 0.3f * ((t - 0.5f) / 0.3f); // Decay to 0.7
            else if (t < 2.0f)
                envelope = 0.7f; // Sustain
            else
                envelope = 0.7f * (1.0f - (t - 2.0f) / 1.0f); // Release

            envelope = std::max(0.0f, envelope);

            // LFO
            lfoPhase += 2.0f * juce::MathConstants<float>::pi * lfoRate / static_cast<float>(currentSampleRate);
            const float lfo = std::sin(lfoPhase);

            // Filter cutoff with envelope and LFO modulation
            const float baseCutoff = 800.0f + 2000.0f * envelope + 300.0f * lfo;
            const float filterCoeff = std::exp(-2.0f * juce::MathConstants<float>::pi * baseCutoff / static_cast<float>(currentSampleRate));

            float sampleL = 0.0f, sampleR = 0.0f;

            for (int note = 0; note < numNotes; ++note)
            {
                for (int osc = 0; osc < numOscs; ++osc)
                {
                    const float freq = baseFreqs[note] * std::pow(2.0f, detune[osc] / 1200.0f);
                    phases[note][osc] += 2.0f * juce::MathConstants<float>::pi * freq / static_cast<float>(currentSampleRate);

                    // Saw wave approximation (first 6 harmonics)
                    float saw = 0.0f;
                    for (int h = 1; h <= 6; ++h)
                    {
                        const float harmFreq = freq * static_cast<float>(h);
                        if (harmFreq > currentSampleRate * 0.45f) break; // Anti-alias
                        saw += std::sin(phases[note][osc] * static_cast<float>(h)) / static_cast<float>(h);
                    }
                    saw *= 0.6f;

                    // Stereo spread based on oscillator
                    const float pan = 0.5f + (static_cast<float>(osc) - 1.0f) * 0.3f;
                    sampleL += saw * (1.0f - pan);
                    sampleR += saw * pan;
                }
            }

            // Apply lowpass filter
            filterStateL += (1.0f - filterCoeff) * (sampleL - filterStateL);
            filterStateR += (1.0f - filterCoeff) * (sampleR - filterStateR);

            // Apply envelope and soft saturation
            float outL = filterStateL * envelope * 0.15f;
            float outR = filterStateR * envelope * 0.15f;

            outL = std::tanh(outL * 2.0f);
            outR = std::tanh(outR * 2.0f);

            dataL[i] = outL;
            dataR[i] = outR;
        }
    }

    // =========================================================================
    // GUITAR CHORD - Clean electric with subtle chorus and room ambience
    // =========================================================================
    void generateGuitarChord()
    {
        const float duration = 2.5f;
        const int numSamples = static_cast<int>(currentSampleRate * duration);

        guitarChordBuffer.setSize(2, numSamples);
        guitarChordBuffer.clear();

        auto* dataL = guitarChordBuffer.getWritePointer(0);
        auto* dataR = guitarChordBuffer.getWritePointer(1);

        // Am7 chord (open position): A2, E3, G3, C4, E4
        const float stringFreqs[] = { 110.0f, 164.81f, 196.0f, 261.63f, 329.63f };
        const int numStrings = 5;

        // Karplus-Strong delay lines for each string
        std::vector<std::vector<float>> delayLines(numStrings);
        std::vector<int> delayIndices(numStrings, 0);
        std::vector<float> lastSamples(numStrings, 0.0f);

        juce::Random random(42);

        // Initialize delay lines with noise bursts (pluck excitation)
        for (int s = 0; s < numStrings; ++s)
        {
            const int delayLength = static_cast<int>(currentSampleRate / stringFreqs[s]);
            delayLines[s].resize(static_cast<size_t>(delayLength), 0.0f);

            // Noise burst with slight filtering for pluck character
            float prev = 0.0f;
            for (int i = 0; i < delayLength; ++i)
            {
                float noise = random.nextFloat() * 2.0f - 1.0f;
                // Simple lowpass to soften the pluck
                noise = noise * 0.6f + prev * 0.4f;
                prev = noise;
                delayLines[s][static_cast<size_t>(i)] = noise;
            }
        }

        // Strum timing (slight delay between strings)
        const float strumTime = 0.025f;
        std::vector<int> strumOffsets(numStrings);
        for (int s = 0; s < numStrings; ++s)
            strumOffsets[s] = static_cast<int>(s * strumTime * currentSampleRate);

        // Chorus LFO
        float chorusPhaseL = 0.0f, chorusPhaseR = juce::MathConstants<float>::pi * 0.5f;
        const float chorusRate = 1.2f;
        const float chorusDepth = 0.002f; // 2ms max modulation

        // Simple reverb (comb filter approximation)
        const int reverbLength = static_cast<int>(currentSampleRate * 0.05);
        std::vector<float> reverbL(static_cast<size_t>(reverbLength), 0.0f);
        std::vector<float> reverbR(static_cast<size_t>(reverbLength), 0.0f);
        int reverbIndex = 0;
        const float reverbDecay = 0.35f;

        for (int i = 0; i < numSamples; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(currentSampleRate);

            // Guitar body resonance envelope
            float envelope;
            if (t < 0.01f)
                envelope = t / 0.01f;
            else
                envelope = std::exp(-(t - 0.01f) * 1.8f);

            float sampleL = 0.0f, sampleR = 0.0f;

            // Process each string
            for (int s = 0; s < numStrings; ++s)
            {
                const int effectivePos = i - strumOffsets[s];
                if (effectivePos < 0) continue;

                const int delayLen = static_cast<int>(delayLines[s].size());
                const int idx = delayIndices[s];

                // Karplus-Strong: read, filter, write back
                const float current = delayLines[s][static_cast<size_t>(idx)];
                const float filtered = 0.5f * (current + lastSamples[s]);
                lastSamples[s] = current;

                // Decay factor (higher strings decay faster)
                const float decay = 0.996f - static_cast<float>(s) * 0.0003f;
                delayLines[s][static_cast<size_t>(idx)] = filtered * decay;

                delayIndices[s] = (idx + 1) % delayLen;

                // Pan strings across stereo field
                const float pan = 0.3f + static_cast<float>(s) * 0.1f;
                sampleL += current * (1.0f - pan);
                sampleR += current * pan;
            }

            sampleL *= envelope;
            sampleR *= envelope;

            // Chorus effect
            chorusPhaseL += 2.0f * juce::MathConstants<float>::pi * chorusRate / static_cast<float>(currentSampleRate);
            chorusPhaseR += 2.0f * juce::MathConstants<float>::pi * chorusRate / static_cast<float>(currentSampleRate);

            const float chorusModL = std::sin(chorusPhaseL) * chorusDepth * static_cast<float>(currentSampleRate);
            const float chorusModR = std::sin(chorusPhaseR) * chorusDepth * static_cast<float>(currentSampleRate);

            // Simple chorus using short delay modulation
            if (i > static_cast<int>(chorusDepth * currentSampleRate * 2.0f))
            {
                const int chorusDelayL = static_cast<int>(chorusDepth * currentSampleRate + chorusModL);
                const int chorusDelayR = static_cast<int>(chorusDepth * currentSampleRate + chorusModR);
                sampleL = sampleL * 0.7f + dataL[i - chorusDelayL] * 0.3f;
                sampleR = sampleR * 0.7f + dataR[i - chorusDelayR] * 0.3f;
            }

            // Room ambience (simple comb reverb)
            const float reverbIn = (sampleL + sampleR) * 0.5f;
            const float reverbOut = reverbL[static_cast<size_t>(reverbIndex)] * reverbDecay;
            reverbL[static_cast<size_t>(reverbIndex)] = reverbIn + reverbOut * 0.3f;
            reverbR[static_cast<size_t>((reverbIndex + reverbLength / 3) % reverbLength)] = reverbIn + reverbOut * 0.25f;
            reverbIndex = (reverbIndex + 1) % reverbLength;

            sampleL += reverbOut * 0.15f;
            sampleR += reverbR[static_cast<size_t>((reverbIndex + reverbLength * 2 / 3) % reverbLength)] * reverbDecay * 0.15f;

            // Gentle tube-like warmth
            sampleL = std::tanh(sampleL * 1.2f) * 0.6f;
            sampleR = std::tanh(sampleR * 1.2f) * 0.6f;

            dataL[i] = sampleL;
            dataR[i] = sampleR;
        }
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TestToneGenerator)
};
