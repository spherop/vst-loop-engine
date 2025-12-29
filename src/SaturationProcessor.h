#pragma once

#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <array>

/**
 * SaturationProcessor - Multi-algorithm saturation/distortion effect
 *
 * Four distinct saturation types:
 * - SOFT: Smooth tanh-based soft clipping (clean saturation)
 * - TAPE: Asymmetric tape saturation with bias and flutter
 * - TUBE: Vacuum tube emulation with harmonics and sag
 * - FUZZ: Extreme clipping with octave-up harmonics
 */
class SaturationProcessor
{
public:
    enum class Type { Soft = 0, Tape, Tube, Fuzz };

    SaturationProcessor() = default;

    void prepare(double sampleRate, int samplesPerBlock)
    {
        currentSampleRate = sampleRate;

        // Initialize bypass gain smoother for click-free toggling
        // 50ms ramp time for smooth enable/disable transitions
        bypassGain.reset(sampleRate, 0.050);
        bypassGain.setCurrentAndTargetValue(0.0f);

        // Master mix smoother
        mixSmooth.reset(sampleRate, 0.005);
        mixSmooth.setCurrentAndTargetValue(1.0f);

        // Initialize all type-specific parameter smoothers
        initSoftParams(sampleRate);
        initTapeParams(sampleRate);
        initTubeParams(sampleRate);
        initFuzzParams(sampleRate);

        // Reset filter states
        resetFilters();
    }

    void processBlock(juce::AudioBuffer<float>& buffer)
    {
        const int numSamples = buffer.getNumSamples();
        float* leftChannel = buffer.getWritePointer(0);
        float* rightChannel = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;

        for (int i = 0; i < numSamples; ++i)
        {
            const float bypass = bypassGain.getNextValue();

            // Early exit if fully bypassed
            if (bypass < 0.0001f)
            {
                consumeAllSmoothedValues();
                continue;
            }

            float dryL = leftChannel[i];
            float dryR = rightChannel ? rightChannel[i] : dryL;

            float wetL = dryL;
            float wetR = dryR;

            // Process based on current type
            switch (currentType.load())
            {
                case Type::Soft:
                    processSoft(wetL, wetR);
                    break;
                case Type::Tape:
                    processTape(wetL, wetR);
                    break;
                case Type::Tube:
                    processTube(wetL, wetR);
                    break;
                case Type::Fuzz:
                    processFuzz(wetL, wetR);
                    break;
            }

            // Consume unused type parameters to keep smoothers in sync
            consumeUnusedTypeParams();

            // Apply master mix
            const float mix = mixSmooth.getNextValue();
            float processedL = dryL * (1.0f - mix) + wetL * mix;
            float processedR = dryR * (1.0f - mix) + wetR * mix;

            // Apply bypass crossfade
            leftChannel[i] = dryL * (1.0f - bypass) + processedL * bypass;
            if (rightChannel)
                rightChannel[i] = dryR * (1.0f - bypass) + processedR * bypass;
        }
    }

    // ========================================
    // MASTER CONTROLS
    // ========================================

    void setEnabled(bool on)
    {
        enabled.store(on);
        bypassGain.setTargetValue(on ? 1.0f : 0.0f);
    }
    bool isEnabled() const { return enabled.load(); }

    void setType(int type)
    {
        currentType.store(static_cast<Type>(std::clamp(type, 0, 3)));
    }
    Type getType() const { return currentType.load(); }

    void setMix(float normalized)
    {
        mixSmooth.setTargetValue(std::clamp(normalized, 0.0f, 1.0f));
    }

    // ========================================
    // SOFT TYPE PARAMETERS
    // ========================================

    void setSoftDrive(float normalized)
    {
        softDriveSmooth.setTargetValue(std::clamp(normalized, 0.0f, 1.0f));
    }

    void setSoftTone(float normalized)
    {
        softToneSmooth.setTargetValue(std::clamp(normalized, 0.0f, 1.0f));
    }

    void setSoftCurve(float normalized)
    {
        softCurveSmooth.setTargetValue(std::clamp(normalized, 0.0f, 1.0f));
    }

    // ========================================
    // TAPE TYPE PARAMETERS
    // ========================================

    void setTapeDrive(float normalized)
    {
        tapeDriveSmooth.setTargetValue(std::clamp(normalized, 0.0f, 1.0f));
    }

    void setTapeBias(float normalized)
    {
        tapeBiasSmooth.setTargetValue(std::clamp(normalized, 0.0f, 1.0f));
    }

    void setTapeFlutter(float normalized)
    {
        tapeFlutterSmooth.setTargetValue(std::clamp(normalized, 0.0f, 1.0f));
    }

    void setTapeTone(float normalized)
    {
        tapeToneSmooth.setTargetValue(std::clamp(normalized, 0.0f, 1.0f));
    }

    // ========================================
    // TUBE TYPE PARAMETERS
    // ========================================

    void setTubeDrive(float normalized)
    {
        tubeDriveSmooth.setTargetValue(std::clamp(normalized, 0.0f, 1.0f));
    }

    void setTubeBias(float normalized)
    {
        tubeBiasSmooth.setTargetValue(std::clamp(normalized, 0.0f, 1.0f));
    }

    void setTubeWarmth(float normalized)
    {
        tubeWarmthSmooth.setTargetValue(std::clamp(normalized, 0.0f, 1.0f));
    }

    void setTubeSag(float normalized)
    {
        tubeSagSmooth.setTargetValue(std::clamp(normalized, 0.0f, 1.0f));
    }

    // ========================================
    // FUZZ TYPE PARAMETERS
    // ========================================

    void setFuzzDrive(float normalized)
    {
        fuzzDriveSmooth.setTargetValue(std::clamp(normalized, 0.0f, 1.0f));
    }

    void setFuzzGate(float normalized)
    {
        fuzzGateSmooth.setTargetValue(std::clamp(normalized, 0.0f, 1.0f));
    }

    void setFuzzOctave(float normalized)
    {
        fuzzOctaveSmooth.setTargetValue(std::clamp(normalized, 0.0f, 1.0f));
    }

    void setFuzzTone(float normalized)
    {
        fuzzToneSmooth.setTargetValue(std::clamp(normalized, 0.0f, 1.0f));
    }

private:
    double currentSampleRate = 44100.0;

    // Master state
    std::atomic<bool> enabled { false };
    std::atomic<Type> currentType { Type::Soft };

    juce::SmoothedValue<float> bypassGain;
    juce::SmoothedValue<float> mixSmooth;

    // ========================================
    // SOFT TYPE STATE
    // ========================================
    juce::SmoothedValue<float> softDriveSmooth;
    juce::SmoothedValue<float> softToneSmooth;
    juce::SmoothedValue<float> softCurveSmooth;

    // Soft tone filter state (1-pole LP)
    float softToneStateL = 0.0f;
    float softToneStateR = 0.0f;

    void initSoftParams(double sampleRate)
    {
        softDriveSmooth.reset(sampleRate, 0.005);
        softToneSmooth.reset(sampleRate, 0.005);
        softCurveSmooth.reset(sampleRate, 0.005);

        softDriveSmooth.setCurrentAndTargetValue(0.0f);
        softToneSmooth.setCurrentAndTargetValue(0.5f);
        softCurveSmooth.setCurrentAndTargetValue(0.5f);

        softToneStateL = 0.0f;
        softToneStateR = 0.0f;
    }

    void processSoft(float& left, float& right)
    {
        const float drive = softDriveSmooth.getNextValue();
        const float tone = softToneSmooth.getNextValue();
        const float curve = softCurveSmooth.getNextValue();

        // SOFT: Clean, transparent saturation - minimal coloration
        // Drive: gentle 1x-8x range for subtle saturation
        const float gain = 1.0f + drive * 7.0f;

        float satL = left * gain;
        float satR = right * gain;

        // Curve controls soft-to-hard knee (subtle effect)
        const float curveExp = 1.0f + curve * 2.0f;  // 1 to 3 (gentler range)

        // Clean tanh saturation
        satL = applySoftCurve(satL, curveExp);
        satR = applySoftCurve(satR, curveExp);

        // Output compensation - more aggressive to maintain unity gain
        // Saturated signals sound louder, so compensate more
        const float compensation = 1.0f / std::max(1.0f, gain * 0.7f);
        satL *= compensation;
        satR *= compensation;

        // Tone: gentle high shelf
        const float toneFreq = 2000.0f + tone * 10000.0f;  // 2kHz to 12kHz
        const float toneCoeff = std::exp(-2.0f * juce::MathConstants<float>::pi * toneFreq / static_cast<float>(currentSampleRate));

        softToneStateL = softToneStateL * toneCoeff + satL * (1.0f - toneCoeff);
        softToneStateR = softToneStateR * toneCoeff + satR * (1.0f - toneCoeff);

        left = satL * tone + softToneStateL * (1.0f - tone);
        right = satR * tone + softToneStateR * (1.0f - tone);
    }

    float applySoftCurve(float x, float curveExp)
    {
        // Variable softness: lower curveExp = softer, higher = harder
        // Uses a modified tanh with variable knee
        if (curveExp <= 1.5f)
        {
            // Soft tanh
            return std::tanh(x);
        }
        else
        {
            // Blend towards hard clip as curve increases
            float soft = std::tanh(x);
            float hard = std::clamp(x, -1.0f, 1.0f);
            float blend = (curveExp - 1.5f) / 3.5f;  // 0 at curve=1.5, 1 at curve=5
            return soft * (1.0f - blend) + hard * blend;
        }
    }

    // ========================================
    // TAPE TYPE STATE
    // ========================================
    juce::SmoothedValue<float> tapeDriveSmooth;
    juce::SmoothedValue<float> tapeBiasSmooth;
    juce::SmoothedValue<float> tapeFlutterSmooth;
    juce::SmoothedValue<float> tapeToneSmooth;

    // Tape flutter LFO
    float tapeFlutterPhase = 0.0f;

    // Tape head bump filter (low shelf boost)
    float tapeHeadBumpStateL = 0.0f;
    float tapeHeadBumpStateR = 0.0f;

    // Tape high cut filter
    float tapeHighCutStateL = 0.0f;
    float tapeHighCutStateR = 0.0f;

    void initTapeParams(double sampleRate)
    {
        tapeDriveSmooth.reset(sampleRate, 0.005);
        tapeBiasSmooth.reset(sampleRate, 0.005);
        tapeFlutterSmooth.reset(sampleRate, 0.010);
        tapeToneSmooth.reset(sampleRate, 0.005);

        tapeDriveSmooth.setCurrentAndTargetValue(0.3f);
        tapeBiasSmooth.setCurrentAndTargetValue(0.5f);
        tapeFlutterSmooth.setCurrentAndTargetValue(0.0f);
        tapeToneSmooth.setCurrentAndTargetValue(0.5f);

        tapeFlutterPhase = 0.0f;
        tapeHeadBumpStateL = tapeHeadBumpStateR = 0.0f;
        tapeHighCutStateL = tapeHighCutStateR = 0.0f;
    }

    void processTape(float& left, float& right)
    {
        const float drive = tapeDriveSmooth.getNextValue();
        const float bias = tapeBiasSmooth.getNextValue();
        const float flutter = tapeFlutterSmooth.getNextValue();
        const float tone = tapeToneSmooth.getNextValue();

        // TAPE: Warm, compressed, dark - characteristic tape sound

        // Apply flutter (pitch wobble) - more pronounced
        if (flutter > 0.001f)
        {
            const float wowRate = 0.4f;    // Slow wow
            const float flutterRate = 6.0f; // Faster flutter

            tapeFlutterPhase += 1.0f / static_cast<float>(currentSampleRate);
            if (tapeFlutterPhase >= 1.0f) tapeFlutterPhase -= 1.0f;

            float wow = std::sin(tapeFlutterPhase * wowRate * 2.0f * juce::MathConstants<float>::pi);
            float flut = std::sin(tapeFlutterPhase * flutterRate * 2.0f * juce::MathConstants<float>::pi);

            // More audible flutter effect
            float modulation = (wow * 0.6f + flut * 0.4f) * flutter * 0.008f;
            float pitchMod = 1.0f + modulation;
            left *= pitchMod;
            right *= pitchMod;
        }

        // Drive: more aggressive compression (1x to 15x)
        const float gain = 1.0f + drive * 14.0f;
        float satL = left * gain;
        float satR = right * gain;

        // Bias: stronger asymmetry for more even harmonics
        const float biasOffset = (bias - 0.5f) * 0.5f;
        satL += biasOffset;
        satR += biasOffset;

        // Tape saturation with compression
        satL = processTapeSaturation(satL);
        satR = processTapeSaturation(satR);

        // Remove DC
        satL -= biasOffset * 0.4f;
        satR -= biasOffset * 0.4f;

        // Output compensation - aggressive to maintain perceived unity
        const float compensation = 1.0f / std::max(1.0f, gain * 0.75f);
        satL *= compensation;
        satR *= compensation;

        // Strong head bump: bass boost around 80-100Hz
        const float bumpCoeff = 0.997f;  // Lower = more bass
        tapeHeadBumpStateL = tapeHeadBumpStateL * bumpCoeff + satL * (1.0f - bumpCoeff);
        tapeHeadBumpStateR = tapeHeadBumpStateR * bumpCoeff + satR * (1.0f - bumpCoeff);

        // More pronounced low bump
        satL += tapeHeadBumpStateL * 0.4f;
        satR += tapeHeadBumpStateR * 0.4f;

        // Aggressive high cut - tape is DARK
        // tone 0 = very dark (~1.5kHz), tone 1 = warmer (~6kHz)
        const float highCutFreq = 1500.0f + tone * 4500.0f;
        const float highCutCoeff = std::exp(-2.0f * juce::MathConstants<float>::pi * highCutFreq / static_cast<float>(currentSampleRate));

        tapeHighCutStateL = tapeHighCutStateL * highCutCoeff + satL * (1.0f - highCutCoeff);
        tapeHighCutStateR = tapeHighCutStateR * highCutCoeff + satR * (1.0f - highCutCoeff);

        left = tapeHighCutStateL;
        right = tapeHighCutStateR;
    }

    float processTapeSaturation(float x)
    {
        // Asymmetric soft clip (like real tape)
        if (x > 0.0f)
            return 1.0f - std::exp(-x);
        else
            return -1.0f + std::exp(x);
    }

    // ========================================
    // TUBE TYPE STATE
    // ========================================
    juce::SmoothedValue<float> tubeDriveSmooth;
    juce::SmoothedValue<float> tubeBiasSmooth;
    juce::SmoothedValue<float> tubeWarmthSmooth;
    juce::SmoothedValue<float> tubeSagSmooth;

    // Tube sag envelope follower
    float tubeSagEnvL = 0.0f;
    float tubeSagEnvR = 0.0f;

    // Tube warmth filter (adds even harmonics via asymmetric processing)
    float tubeWarmthStateL = 0.0f;
    float tubeWarmthStateR = 0.0f;

    void initTubeParams(double sampleRate)
    {
        tubeDriveSmooth.reset(sampleRate, 0.005);
        tubeBiasSmooth.reset(sampleRate, 0.005);
        tubeWarmthSmooth.reset(sampleRate, 0.005);
        tubeSagSmooth.reset(sampleRate, 0.010);

        tubeDriveSmooth.setCurrentAndTargetValue(0.3f);
        tubeBiasSmooth.setCurrentAndTargetValue(0.5f);
        tubeWarmthSmooth.setCurrentAndTargetValue(0.5f);
        tubeSagSmooth.setCurrentAndTargetValue(0.0f);

        tubeSagEnvL = tubeSagEnvR = 0.0f;
        tubeWarmthStateL = tubeWarmthStateR = 0.0f;
    }

    void processTube(float& left, float& right)
    {
        const float drive = tubeDriveSmooth.getNextValue();
        const float bias = tubeBiasSmooth.getNextValue();
        const float warmth = tubeWarmthSmooth.getNextValue();
        const float sag = tubeSagSmooth.getNextValue();

        // TUBE: Rich harmonics, sag compression, warm midrange boost

        // Sag: power supply compression - more dramatic effect
        float sagAmount = 0.0f;
        if (sag > 0.001f)
        {
            const float sagAttack = 0.0005f;  // Slightly slower attack
            const float sagRelease = 0.9985f; // Slower release for "bloom"

            float inputLevel = std::max(std::abs(left), std::abs(right));

            if (inputLevel > tubeSagEnvL)
                tubeSagEnvL = tubeSagEnvL * (1.0f - sagAttack) + inputLevel * sagAttack;
            else
                tubeSagEnvL = tubeSagEnvL * sagRelease;

            // More pronounced sag effect
            sagAmount = tubeSagEnvL * sag * 0.7f;
        }

        // Drive with sag compression (1x to 20x)
        const float effectiveDrive = drive * (1.0f - sagAmount * 0.5f);
        const float gain = 1.0f + effectiveDrive * 19.0f;

        float satL = left * gain;
        float satR = right * gain;

        // Bias: stronger effect on harmonic content
        const float biasOffset = (bias - 0.5f) * 0.4f;
        satL += biasOffset;
        satR += biasOffset;

        // Tube saturation with harmonics
        satL = processTubeSaturation(satL, warmth);
        satR = processTubeSaturation(satR, warmth);

        // Remove DC
        satL -= biasOffset * 0.25f;
        satR -= biasOffset * 0.25f;

        // Output compensation - aggressive for unity gain perception
        const float compensation = 1.0f / std::max(1.0f, gain * 0.7f);
        satL *= compensation;
        satR *= compensation;

        // Warmth filter: mid-frequency presence boost
        const float warmthCoeff = 0.95f;  // ~500Hz emphasis
        tubeWarmthStateL = tubeWarmthStateL * warmthCoeff + satL * (1.0f - warmthCoeff);
        tubeWarmthStateR = tubeWarmthStateR * warmthCoeff + satR * (1.0f - warmthCoeff);

        // Strong warmth blend for "round" tube sound
        left = satL * (1.0f - warmth * 0.3f) + tubeWarmthStateL * warmth * 0.5f;
        right = satR * (1.0f - warmth * 0.3f) + tubeWarmthStateR * warmth * 0.5f;
    }

    float processTubeSaturation(float x, float warmth)
    {
        // Tube-like transfer function with rich harmonics
        // More aggressive harmonic generation

        // Odd harmonics (symmetric) - the base
        float odd = std::tanh(x * 1.2f);

        // Even harmonics (asymmetric) - the warmth/character
        // x^2 creates 2nd harmonic, x^4 creates 4th
        float even2 = x * std::abs(x) * 0.4f;  // Strong 2nd harmonic
        float even4 = x * x * x * std::abs(x) * 0.1f;  // Subtle 4th harmonic

        // Blend based on warmth (more warmth = more even harmonics)
        float even = (even2 + even4) * std::tanh(x);  // Soft limit the harmonics
        float result = odd + even * warmth;

        // Soft limit
        return std::clamp(result, -1.2f, 1.2f);
    }

    // ========================================
    // FUZZ TYPE STATE
    // ========================================
    juce::SmoothedValue<float> fuzzDriveSmooth;
    juce::SmoothedValue<float> fuzzGateSmooth;
    juce::SmoothedValue<float> fuzzOctaveSmooth;
    juce::SmoothedValue<float> fuzzToneSmooth;

    // Fuzz tone filter
    float fuzzToneStateL = 0.0f;
    float fuzzToneStateR = 0.0f;

    // Fuzz octave full-wave rectifier state
    float fuzzOctavePrevL = 0.0f;
    float fuzzOctavePrevR = 0.0f;

    // Fuzz gate envelope
    float fuzzGateEnvL = 0.0f;
    float fuzzGateEnvR = 0.0f;

    void initFuzzParams(double sampleRate)
    {
        fuzzDriveSmooth.reset(sampleRate, 0.005);
        fuzzGateSmooth.reset(sampleRate, 0.005);
        fuzzOctaveSmooth.reset(sampleRate, 0.005);
        fuzzToneSmooth.reset(sampleRate, 0.005);

        fuzzDriveSmooth.setCurrentAndTargetValue(0.5f);
        fuzzGateSmooth.setCurrentAndTargetValue(0.0f);
        fuzzOctaveSmooth.setCurrentAndTargetValue(0.0f);
        fuzzToneSmooth.setCurrentAndTargetValue(0.5f);

        fuzzToneStateL = fuzzToneStateR = 0.0f;
        fuzzOctavePrevL = fuzzOctavePrevR = 0.0f;
        fuzzGateEnvL = fuzzGateEnvR = 0.0f;
    }

    void processFuzz(float& left, float& right)
    {
        const float drive = fuzzDriveSmooth.getNextValue();
        const float gate = fuzzGateSmooth.getNextValue();
        const float octave = fuzzOctaveSmooth.getNextValue();
        const float tone = fuzzToneSmooth.getNextValue();

        // Noise gate: mutes signal below threshold
        float gateGainL = 1.0f;
        float gateGainR = 1.0f;
        if (gate > 0.001f)
        {
            const float threshold = gate * 0.1f;  // Gate threshold
            const float attack = 0.001f;
            const float release = 0.995f;

            // Envelope followers
            if (std::abs(left) > fuzzGateEnvL)
                fuzzGateEnvL = fuzzGateEnvL * (1.0f - attack) + std::abs(left) * attack;
            else
                fuzzGateEnvL *= release;

            if (std::abs(right) > fuzzGateEnvR)
                fuzzGateEnvR = fuzzGateEnvR * (1.0f - attack) + std::abs(right) * attack;
            else
                fuzzGateEnvR *= release;

            // Gate gain
            gateGainL = (fuzzGateEnvL > threshold) ? 1.0f : fuzzGateEnvL / threshold;
            gateGainR = (fuzzGateEnvR > threshold) ? 1.0f : fuzzGateEnvR / threshold;
        }

        // Extreme drive for fuzz (1x to 100x)
        const float gain = 1.0f + drive * 99.0f;
        float satL = left * gain * gateGainL;
        float satR = right * gain * gateGainR;

        // Hard clip for that gnarly fuzz sound
        satL = processFuzzClip(satL);
        satR = processFuzzClip(satR);

        // Octave-up effect: full-wave rectification creates octave harmonic
        if (octave > 0.001f)
        {
            // Full-wave rectification (absolute value creates octave)
            float octL = std::abs(satL) * 2.0f - 1.0f;
            float octR = std::abs(satR) * 2.0f - 1.0f;

            // Smooth the octave signal slightly
            octL = octL * 0.7f + fuzzOctavePrevL * 0.3f;
            octR = octR * 0.7f + fuzzOctavePrevR * 0.3f;
            fuzzOctavePrevL = octL;
            fuzzOctavePrevR = octR;

            // Blend octave
            satL = satL * (1.0f - octave) + octL * octave;
            satR = satR * (1.0f - octave) + octR * octave;
        }

        // Output compensation - scale with drive for more consistent volume
        // Hard clipping sounds louder at high drive, so reduce more
        const float compensation = 0.4f / std::max(1.0f, 1.0f + drive * 0.5f);
        satL *= compensation;
        satR *= compensation;

        // Fuzz tone: typically a simple tone stack
        // tone 0 = dark/wooly, tone 1 = bright/sizzly
        const float toneFreq = 800.0f + tone * 8000.0f;
        const float toneCoeff = std::exp(-2.0f * juce::MathConstants<float>::pi * toneFreq / static_cast<float>(currentSampleRate));

        fuzzToneStateL = fuzzToneStateL * toneCoeff + satL * (1.0f - toneCoeff);
        fuzzToneStateR = fuzzToneStateR * toneCoeff + satR * (1.0f - toneCoeff);

        // Crossfade between dark and bright
        left = fuzzToneStateL * (1.0f - tone) + satL * tone;
        right = fuzzToneStateR * (1.0f - tone) + satR * tone;
    }

    float processFuzzClip(float x)
    {
        // Asymmetric hard clip with some rounding
        if (x > 0.8f)
            return 0.8f + 0.2f * std::tanh((x - 0.8f) * 5.0f);
        else if (x < -0.6f)
            return -0.6f + 0.4f * std::tanh((x + 0.6f) * 3.0f);
        else
            return x;
    }

    // ========================================
    // UTILITY METHODS
    // ========================================

    void resetFilters()
    {
        softToneStateL = softToneStateR = 0.0f;
        tapeHeadBumpStateL = tapeHeadBumpStateR = 0.0f;
        tapeHighCutStateL = tapeHighCutStateR = 0.0f;
        tubeWarmthStateL = tubeWarmthStateR = 0.0f;
        tubeSagEnvL = tubeSagEnvR = 0.0f;
        fuzzToneStateL = fuzzToneStateR = 0.0f;
        fuzzGateEnvL = fuzzGateEnvR = 0.0f;
        fuzzOctavePrevL = fuzzOctavePrevR = 0.0f;
    }

    void consumeAllSmoothedValues()
    {
        mixSmooth.getNextValue();

        softDriveSmooth.getNextValue();
        softToneSmooth.getNextValue();
        softCurveSmooth.getNextValue();

        tapeDriveSmooth.getNextValue();
        tapeBiasSmooth.getNextValue();
        tapeFlutterSmooth.getNextValue();
        tapeToneSmooth.getNextValue();

        tubeDriveSmooth.getNextValue();
        tubeBiasSmooth.getNextValue();
        tubeWarmthSmooth.getNextValue();
        tubeSagSmooth.getNextValue();

        fuzzDriveSmooth.getNextValue();
        fuzzGateSmooth.getNextValue();
        fuzzOctaveSmooth.getNextValue();
        fuzzToneSmooth.getNextValue();
    }

    void consumeUnusedTypeParams()
    {
        // Consume smoothed values for types we're not currently processing
        // to keep all smoothers in sync
        switch (currentType.load())
        {
            case Type::Soft:
                // Consume tape, tube, fuzz
                tapeDriveSmooth.getNextValue();
                tapeBiasSmooth.getNextValue();
                tapeFlutterSmooth.getNextValue();
                tapeToneSmooth.getNextValue();
                tubeDriveSmooth.getNextValue();
                tubeBiasSmooth.getNextValue();
                tubeWarmthSmooth.getNextValue();
                tubeSagSmooth.getNextValue();
                fuzzDriveSmooth.getNextValue();
                fuzzGateSmooth.getNextValue();
                fuzzOctaveSmooth.getNextValue();
                fuzzToneSmooth.getNextValue();
                break;

            case Type::Tape:
                // Consume soft, tube, fuzz
                softDriveSmooth.getNextValue();
                softToneSmooth.getNextValue();
                softCurveSmooth.getNextValue();
                tubeDriveSmooth.getNextValue();
                tubeBiasSmooth.getNextValue();
                tubeWarmthSmooth.getNextValue();
                tubeSagSmooth.getNextValue();
                fuzzDriveSmooth.getNextValue();
                fuzzGateSmooth.getNextValue();
                fuzzOctaveSmooth.getNextValue();
                fuzzToneSmooth.getNextValue();
                break;

            case Type::Tube:
                // Consume soft, tape, fuzz
                softDriveSmooth.getNextValue();
                softToneSmooth.getNextValue();
                softCurveSmooth.getNextValue();
                tapeDriveSmooth.getNextValue();
                tapeBiasSmooth.getNextValue();
                tapeFlutterSmooth.getNextValue();
                tapeToneSmooth.getNextValue();
                fuzzDriveSmooth.getNextValue();
                fuzzGateSmooth.getNextValue();
                fuzzOctaveSmooth.getNextValue();
                fuzzToneSmooth.getNextValue();
                break;

            case Type::Fuzz:
                // Consume soft, tape, tube
                softDriveSmooth.getNextValue();
                softToneSmooth.getNextValue();
                softCurveSmooth.getNextValue();
                tapeDriveSmooth.getNextValue();
                tapeBiasSmooth.getNextValue();
                tapeFlutterSmooth.getNextValue();
                tapeToneSmooth.getNextValue();
                tubeDriveSmooth.getNextValue();
                tubeBiasSmooth.getNextValue();
                tubeWarmthSmooth.getNextValue();
                tubeSagSmooth.getNextValue();
                break;
        }
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SaturationProcessor)
};
