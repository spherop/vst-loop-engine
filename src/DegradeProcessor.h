#pragma once

#include <juce_dsp/juce_dsp.h>
#include <vector>
#include <array>
#include <cmath>

class DegradeProcessor
{
public:
    DegradeProcessor() = default;

    void prepare(double sampleRate, int samplesPerBlock)
    {
        currentSampleRate = sampleRate;

        // Initialize smoothed values - use short smoothing for snappy response
        hpFreqSmooth.reset(sampleRate, 0.005);
        hpQSmooth.reset(sampleRate, 0.005);
        lpFreqSmooth.reset(sampleRate, 0.005);
        lpQSmooth.reset(sampleRate, 0.005);
        bitDepthSmooth.reset(sampleRate, 0.005);
        srReductionSmooth.reset(sampleRate, 0.005);
        wobbleAmountSmooth.reset(sampleRate, 0.01);
        mixSmooth.reset(sampleRate, 0.005);

        // Set initial values
        hpFreqSmooth.setCurrentAndTargetValue(20.0f);
        hpQSmooth.setCurrentAndTargetValue(0.707f);
        lpFreqSmooth.setCurrentAndTargetValue(20000.0f);
        lpQSmooth.setCurrentAndTargetValue(0.707f);
        bitDepthSmooth.setCurrentAndTargetValue(16.0f);
        srReductionSmooth.setCurrentAndTargetValue(static_cast<float>(sampleRate));
        wobbleAmountSmooth.setCurrentAndTargetValue(0.0f);
        mixSmooth.setCurrentAndTargetValue(1.0f);

        // Initialize bypass gain smoothers for click-free toggling
        // Use a very short ramp time (~3ms) for peppy/immediate response
        masterBypassGain.reset(sampleRate, 0.003);
        filterBypassGain.reset(sampleRate, 0.003);
        hpBypassGain.reset(sampleRate, 0.003);
        lpBypassGain.reset(sampleRate, 0.003);
        lofiBypassGain.reset(sampleRate, 0.003);
        textureBypassGain.reset(sampleRate, 0.003);

        // Set initial bypass states - most off by default for clean audio
        // EXCEPTION: filterBypassGain and HP/LP start ON because the UI expects them enabled
        // and there's no master toggle for filter section (only individual HP/LP toggles)
        masterBypassGain.setCurrentAndTargetValue(0.0f);
        filterBypassGain.setCurrentAndTargetValue(1.0f);  // Filter section ON by default
        hpBypassGain.setCurrentAndTargetValue(1.0f);      // HP filter ON by default
        lpBypassGain.setCurrentAndTargetValue(1.0f);      // LP filter ON by default
        lofiBypassGain.setCurrentAndTargetValue(0.0f);
        textureBypassGain.setCurrentAndTargetValue(0.0f);

        // Reset filter states
        resetFilters();

        // Reset sample rate reducer
        srHoldL = 0.0f;
        srHoldR = 0.0f;
        srCounter = 0.0f;

        // Initialize wobble delay buffer (for tape flutter effect)
        // Need ~50ms of buffer for pitch modulation at low frequencies
        int wobbleBufferSize = static_cast<int>(sampleRate * 0.1); // 100ms
        wobbleDelayBufferL.resize(wobbleBufferSize, 0.0f);
        wobbleDelayBufferR.resize(wobbleBufferSize, 0.0f);
        wobbleWritePos = 0;
        wobbleLfoPhase = 0.0f;
        wobbleDelaySmoothed = static_cast<float>(sampleRate) * 0.015f; // Start at base delay

        // Initialize granular texture engine
        initializeTexture(sampleRate);
    }

    void processBlock(juce::AudioBuffer<float>& buffer)
    {
        const int numSamples = buffer.getNumSamples();
        float* leftChannel = buffer.getWritePointer(0);
        float* rightChannel = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;

        for (int i = 0; i < numSamples; ++i)
        {
            float inputL = leftChannel[i];
            float inputR = rightChannel ? rightChannel[i] : inputL;

            // Store dry signal for final mix
            float dryL = inputL;
            float dryR = inputR;

            // Get smoothed bypass gains for click-free transitions
            const float masterGain = masterBypassGain.getNextValue();

            // Early exit if master is fully bypassed (gain near zero)
            if (masterGain < 0.0001f)
            {
                // Still consume the other smoothed values to keep them in sync
                filterBypassGain.getNextValue();
                hpBypassGain.getNextValue();
                lpBypassGain.getNextValue();
                lofiBypassGain.getNextValue();
                textureBypassGain.getNextValue();
                mixSmooth.getNextValue();
                hpFreqSmooth.getNextValue();
                hpQSmooth.getNextValue();
                lpFreqSmooth.getNextValue();
                lpQSmooth.getNextValue();
                bitDepthSmooth.getNextValue();
                srReductionSmooth.getNextValue();
                wobbleAmountSmooth.getNextValue();
                textureDensitySmooth.getNextValue();
                textureScatterSmooth.getNextValue();
                textureMotionSmooth.getNextValue();
                textureMixSmooth.getNextValue();
                // Output is unchanged (dry signal)
                continue;
            }

            const float filterGain = filterBypassGain.getNextValue();
            const float hpGain = hpBypassGain.getNextValue();
            const float lpGain = lpBypassGain.getNextValue();
            const float lofiGain = lofiBypassGain.getNextValue();
            const float textureGain = textureBypassGain.getNextValue();

            float wetL = inputL;
            float wetR = inputR;

            // Get smoothed parameter values
            const float hpFreq = hpFreqSmooth.getNextValue();
            const float hpQ = hpQSmooth.getNextValue();
            const float lpFreq = lpFreqSmooth.getNextValue();
            const float lpQ = lpQSmooth.getNextValue();
            const float bitDepth = bitDepthSmooth.getNextValue();
            const float srTarget = srReductionSmooth.getNextValue();
            const float wobbleAmt = wobbleAmountSmooth.getNextValue();
            const float mix = mixSmooth.getNextValue();

            // ==== FILTER SECTION ====
            // Always process filters to keep state continuous, then crossfade
            {
                float preFilterL = wetL;
                float preFilterR = wetR;

                // High-pass filter (biquad) - always process, crossfade based on gain
                updateHighPassCoeffs(hpFreq, hpQ);
                float hpOutL = processHighPass(wetL, true);
                float hpOutR = processHighPass(wetR, false);
                // Crossfade between unfiltered and filtered based on HP gain
                float effectiveHpGain = hpGain * filterGain;
                wetL = preFilterL * (1.0f - effectiveHpGain) + hpOutL * effectiveHpGain;
                wetR = preFilterR * (1.0f - effectiveHpGain) + hpOutR * effectiveHpGain;

                // Low-pass filter (biquad) - always process, crossfade based on gain
                preFilterL = wetL;
                preFilterR = wetR;
                updateLowPassCoeffs(lpFreq, lpQ);
                float lpOutL = processLowPass(wetL, true);
                float lpOutR = processLowPass(wetR, false);
                // Crossfade between unfiltered and filtered based on LP gain
                float effectiveLpGain = lpGain * filterGain;
                wetL = preFilterL * (1.0f - effectiveLpGain) + lpOutL * effectiveLpGain;
                wetR = preFilterR * (1.0f - effectiveLpGain) + lpOutR * effectiveLpGain;
            }

            // ==== LO-FI SECTION ====
            // Process and crossfade based on lofi gain
            if (lofiGain > 0.001f || lofiEnabled.load())
            {
                float preLofiL = wetL;
                float preLofiR = wetR;

                // Bit crusher (with per-channel noise shaping)
                float lofiL = processBitCrush(wetL, bitDepth, true);
                float lofiR = processBitCrush(wetR, bitDepth, false);

                // Sample rate reduction
                processSampleRateReduction(lofiL, lofiR, srTarget);

                // Wobble (tape flutter - pitch instability)
                if (wobbleAmt > 0.001f)
                {
                    processWobble(lofiL, lofiR, wobbleAmt);
                }

                // Crossfade between dry and lo-fi processed
                wetL = preLofiL * (1.0f - lofiGain) + lofiL * lofiGain;
                wetR = preLofiR * (1.0f - lofiGain) + lofiR * lofiGain;
            }

            // ==== TEXTURE SECTION ====
            // Multi-voice granular texture engine
            if (textureGain > 0.001f || textureEnabled.load())
            {
                const float density = textureDensitySmooth.getNextValue();
                const float scatter = textureScatterSmooth.getNextValue();
                const float motion = textureMotionSmooth.getNextValue();
                const float texMix = textureMixSmooth.getNextValue();

                if (texMix > 0.001f)
                {
                    float preTextureL = wetL;
                    float preTextureR = wetR;

                    processTexture(wetL, wetR, density, scatter, motion);

                    // Apply texture mix and bypass crossfade
                    float effectiveTexMix = texMix * textureGain;
                    wetL = preTextureL * (1.0f - effectiveTexMix) + wetL * effectiveTexMix;
                    wetR = preTextureR * (1.0f - effectiveTexMix) + wetR * effectiveTexMix;
                }
            }
            else
            {
                // Consume smoothed values to keep them in sync
                textureDensitySmooth.getNextValue();
                textureScatterSmooth.getNextValue();
                textureMotionSmooth.getNextValue();
                textureMixSmooth.getNextValue();
            }

            // Apply dry/wet mix, then master bypass crossfade
            float processedL = dryL * (1.0f - mix) + wetL * mix;
            float processedR = dryR * (1.0f - mix) + wetR * mix;

            // Master bypass crossfade
            leftChannel[i] = dryL * (1.0f - masterGain) + processedL * masterGain;
            if (rightChannel)
                rightChannel[i] = dryR * (1.0f - masterGain) + processedR * masterGain;
        }
    }

    // Filter controls
    void setHighPassFreq(float hz)
    {
        hpFreqSmooth.setTargetValue(std::clamp(hz, 20.0f, 2000.0f));
    }

    void setHighPassQ(float q)
    {
        hpQSmooth.setTargetValue(std::clamp(q, 0.5f, 10.0f));
    }

    void setLowPassFreq(float hz)
    {
        lpFreqSmooth.setTargetValue(std::clamp(hz, 200.0f, 20000.0f));
    }

    void setLowPassQ(float q)
    {
        lpQSmooth.setTargetValue(std::clamp(q, 0.5f, 10.0f));
    }

    // Lo-fi controls
    void setBitDepth(float bits)
    {
        bitDepthSmooth.setTargetValue(std::clamp(bits, 1.0f, 16.0f));
    }

    void setSampleRateReduction(float hz)
    {
        srReductionSmooth.setTargetValue(std::clamp(hz, 1000.0f, static_cast<float>(currentSampleRate)));
    }

    void setWobble(float amount)
    {
        wobbleAmountSmooth.setTargetValue(std::clamp(amount, 0.0f, 1.0f));
    }

    // Texture controls (granular engine)
    // Density: 0-1 maps to grain size 200ms->5ms and spawn rate 200ms->1ms
    void setTextureDensity(float normalized)
    {
        textureDensitySmooth.setTargetValue(std::clamp(normalized, 0.0f, 1.0f));
    }

    // Scatter: 0-1 maps from sequential reading to fully random position jumping
    void setTextureScatter(float normalized)
    {
        textureScatterSmooth.setTargetValue(std::clamp(normalized, 0.0f, 1.0f));
    }

    // Motion: 0-1 controls per-grain variation (pitch drift, speed, reverse probability)
    void setTextureMotion(float normalized)
    {
        textureMotionSmooth.setTargetValue(std::clamp(normalized, 0.0f, 1.0f));
    }

    // Texture mix: dry/wet for the texture section
    void setTextureMix(float normalized)
    {
        textureMixSmooth.setTargetValue(std::clamp(normalized, 0.0f, 1.0f));
    }

    void setMix(float mix)
    {
        mixSmooth.setTargetValue(std::clamp(mix, 0.0f, 1.0f));
    }

    // Master bypass control
    void setEnabled(bool on)
    {
        masterEnabled.store(on);
        masterBypassGain.setTargetValue(on ? 1.0f : 0.0f);
    }
    bool isEnabled() const { return masterEnabled.load(); }

    // Section bypass controls
    void setFilterEnabled(bool on)
    {
        filterEnabled.store(on);
        filterBypassGain.setTargetValue(on ? 1.0f : 0.0f);
    }
    void setLofiEnabled(bool on)
    {
        lofiEnabled.store(on);
        lofiBypassGain.setTargetValue(on ? 1.0f : 0.0f);
    }
    void setTextureEnabled(bool on)
    {
        textureEnabled.store(on);
        textureBypassGain.setTargetValue(on ? 1.0f : 0.0f);
    }

    // Individual filter bypass controls
    void setHPEnabled(bool on)
    {
        hpEnabled.store(on);
        hpBypassGain.setTargetValue(on ? 1.0f : 0.0f);
    }
    void setLPEnabled(bool on)
    {
        lpEnabled.store(on);
        lpBypassGain.setTargetValue(on ? 1.0f : 0.0f);
    }

    bool getFilterEnabled() const { return filterEnabled.load(); }
    bool getLofiEnabled() const { return lofiEnabled.load(); }
    bool getTextureEnabled() const { return textureEnabled.load(); }
    bool getHPEnabled() const { return hpEnabled.load(); }
    bool getLPEnabled() const { return lpEnabled.load(); }

    // Get current filter frequencies for visualization
    float getCurrentHPFreq() const { return lastHpFreq; }
    float getCurrentLPFreq() const { return lastLpFreq; }
    float getCurrentHPQ() const { return lastHpQ; }
    float getCurrentLPQ() const { return lastLpQ; }

private:
    double currentSampleRate = 44100.0;

    // Master bypass
    std::atomic<bool> masterEnabled { false };

    // Section bypass states - default to OFF so audio isn't affected until user enables
    // EXCEPTION: filterEnabled defaults to ON because there's no UI toggle for filter section
    // (HP and LP have individual toggles that control the actual filter bypass)
    std::atomic<bool> filterEnabled { true };
    std::atomic<bool> lofiEnabled { false };
    std::atomic<bool> textureEnabled { false };

    // Individual filter bypass states
    // Default to ON so filters work immediately when user adjusts HP/LP knobs
    std::atomic<bool> hpEnabled { true };
    std::atomic<bool> lpEnabled { true };

    // Smoothed bypass gains for click-free toggling (0 = bypassed, 1 = active)
    juce::SmoothedValue<float> masterBypassGain;
    juce::SmoothedValue<float> filterBypassGain;
    juce::SmoothedValue<float> hpBypassGain;
    juce::SmoothedValue<float> lpBypassGain;
    juce::SmoothedValue<float> lofiBypassGain;
    juce::SmoothedValue<float> textureBypassGain;

    // Biquad HP filter coefficients and state
    float hpB0 = 1.0f, hpB1 = 0.0f, hpB2 = 0.0f, hpA1 = 0.0f, hpA2 = 0.0f;
    float hpX1L = 0.0f, hpX2L = 0.0f, hpY1L = 0.0f, hpY2L = 0.0f;
    float hpX1R = 0.0f, hpX2R = 0.0f, hpY1R = 0.0f, hpY2R = 0.0f;
    float lastHpFreq = 20.0f, lastHpQ = 0.707f;
    juce::SmoothedValue<float> hpFreqSmooth, hpQSmooth;

    // Biquad LP filter coefficients and state
    float lpB0 = 1.0f, lpB1 = 0.0f, lpB2 = 0.0f, lpA1 = 0.0f, lpA2 = 0.0f;
    float lpX1L = 0.0f, lpX2L = 0.0f, lpY1L = 0.0f, lpY2L = 0.0f;
    float lpX1R = 0.0f, lpX2R = 0.0f, lpY1R = 0.0f, lpY2R = 0.0f;
    float lastLpFreq = 20000.0f, lastLpQ = 0.707f;
    juce::SmoothedValue<float> lpFreqSmooth, lpQSmooth;

    // Bit crusher with dithering
    juce::SmoothedValue<float> bitDepthSmooth;
    juce::Random ditherRandom;
    float noiseShapeErrorL = 0.0f;  // For first-order noise shaping (left)
    float noiseShapeErrorR = 0.0f;  // For first-order noise shaping (right)

    // Sample rate reducer with anti-aliasing
    float srHoldL = 0.0f, srHoldR = 0.0f;
    float srPrevHoldL = 0.0f, srPrevHoldR = 0.0f;  // Previous held values for interpolation
    float srCounter = 0.0f;
    juce::SmoothedValue<float> srReductionSmooth;

    // Anti-alias filter state for sample rate reduction
    float srAAB0 = 1.0f, srAAB1 = 0.0f, srAAB2 = 0.0f, srAAA1 = 0.0f, srAAA2 = 0.0f;
    float srAAX1L = 0.0f, srAAX2L = 0.0f, srAAY1L = 0.0f, srAAY2L = 0.0f;
    float srAAX1R = 0.0f, srAAX2R = 0.0f, srAAY1R = 0.0f, srAAY2R = 0.0f;
    float lastSRAAFreq = 0.0f;

    // Wobble (tape flutter effect using modulated delay)
    std::vector<float> wobbleDelayBufferL;
    std::vector<float> wobbleDelayBufferR;
    int wobbleWritePos = 0;
    float wobbleLfoPhase = 0.0f;
    float wobbleDelaySmoothed = 0.0f;  // Smoothed delay time to prevent crackling
    juce::SmoothedValue<float> wobbleAmountSmooth;

    // ========================================
    // GRANULAR TEXTURE ENGINE
    // ========================================
    // Multi-voice granular synthesizer for creating rich, evolving textures
    // Inspired by Chase Bliss Mood MK2's granular approach

    // Grain structure - each voice has independent state
    struct Grain
    {
        bool active = false;
        float readPosL = 0.0f;       // Read position in left buffer
        float readPosR = 0.0f;       // Read position in right buffer
        float grainLength = 0.0f;    // Total grain length in samples
        float progress = 0.0f;       // 0.0 to 1.0 within grain lifetime
        float playbackRate = 1.0f;   // Per-grain speed variation (affects pitch)
        bool reverse = false;        // Per-grain reverse flag
    };

    static constexpr int NUM_TEXTURE_VOICES = 6;
    static constexpr int TEXTURE_BUFFER_SIZE = 262144;  // ~5.4s at 48kHz

    std::array<Grain, NUM_TEXTURE_VOICES> textureGrains;
    std::vector<float> textureBufferL, textureBufferR;
    int textureWritePos = 0;
    float textureSpawnTimer = 0.0f;
    int textureBufferFilled = 0;  // Samples written to buffer
    juce::Random textureRandom;

    // Smoothed texture parameters
    juce::SmoothedValue<float> textureDensitySmooth;
    juce::SmoothedValue<float> textureScatterSmooth;
    juce::SmoothedValue<float> textureMotionSmooth;
    juce::SmoothedValue<float> textureMixSmooth;

    // Mix
    juce::SmoothedValue<float> mixSmooth;

    void resetFilters()
    {
        hpX1L = hpX2L = hpY1L = hpY2L = 0.0f;
        hpX1R = hpX2R = hpY1R = hpY2R = 0.0f;
        lpX1L = lpX2L = lpY1L = lpY2L = 0.0f;
        lpX1R = lpX2R = lpY1R = lpY2R = 0.0f;
    }

    void updateHighPassCoeffs(float freq, float q)
    {
        if (std::abs(freq - lastHpFreq) < 0.1f && std::abs(q - lastHpQ) < 0.01f)
            return;

        lastHpFreq = freq;
        lastHpQ = q;

        // Biquad high-pass coefficients
        const float omega = 2.0f * juce::MathConstants<float>::pi * freq / static_cast<float>(currentSampleRate);
        const float sinOmega = std::sin(omega);
        const float cosOmega = std::cos(omega);
        const float alpha = sinOmega / (2.0f * q);

        const float a0 = 1.0f + alpha;
        hpB0 = ((1.0f + cosOmega) / 2.0f) / a0;
        hpB1 = (-(1.0f + cosOmega)) / a0;
        hpB2 = ((1.0f + cosOmega) / 2.0f) / a0;
        hpA1 = (-2.0f * cosOmega) / a0;
        hpA2 = (1.0f - alpha) / a0;
    }

    void updateLowPassCoeffs(float freq, float q)
    {
        if (std::abs(freq - lastLpFreq) < 0.1f && std::abs(q - lastLpQ) < 0.01f)
            return;

        lastLpFreq = freq;
        lastLpQ = q;

        // Biquad low-pass coefficients
        const float omega = 2.0f * juce::MathConstants<float>::pi * freq / static_cast<float>(currentSampleRate);
        const float sinOmega = std::sin(omega);
        const float cosOmega = std::cos(omega);
        const float alpha = sinOmega / (2.0f * q);

        const float a0 = 1.0f + alpha;
        lpB0 = ((1.0f - cosOmega) / 2.0f) / a0;
        lpB1 = (1.0f - cosOmega) / a0;
        lpB2 = ((1.0f - cosOmega) / 2.0f) / a0;
        lpA1 = (-2.0f * cosOmega) / a0;
        lpA2 = (1.0f - alpha) / a0;
    }

    float processHighPass(float input, bool isLeft)
    {
        float& x1 = isLeft ? hpX1L : hpX1R;
        float& x2 = isLeft ? hpX2L : hpX2R;
        float& y1 = isLeft ? hpY1L : hpY1R;
        float& y2 = isLeft ? hpY2L : hpY2R;

        float output = hpB0 * input + hpB1 * x1 + hpB2 * x2 - hpA1 * y1 - hpA2 * y2;

        x2 = x1;
        x1 = input;
        y2 = y1;
        y1 = output;

        return output;
    }

    float processLowPass(float input, bool isLeft)
    {
        float& x1 = isLeft ? lpX1L : lpX1R;
        float& x2 = isLeft ? lpX2L : lpX2R;
        float& y1 = isLeft ? lpY1L : lpY1R;
        float& y2 = isLeft ? lpY2L : lpY2R;

        float output = lpB0 * input + lpB1 * x1 + lpB2 * x2 - lpA1 * y1 - lpA2 * y2;

        x2 = x1;
        x1 = input;
        y2 = y1;
        y1 = output;

        return output;
    }

    float processBitCrush(float input, float bits, bool isLeft = true)
    {
        if (bits >= 15.9f)
            return input;  // No crushing

        // Number of quantization levels
        const float levels = std::pow(2.0f, bits);
        const float stepSize = 2.0f / levels;  // -1 to 1 range

        // Triangular probability density function (TPDF) dithering
        // Sum of two uniform random numbers creates triangular distribution
        // This eliminates quantization distortion and replaces it with
        // less objectionable white noise
        // Scale dithering for low bit depths to avoid overwhelming the signal
        const float ditherScale = bits < 8.0f ? 0.5f : 1.0f;
        const float dither1 = ditherRandom.nextFloat() - 0.5f;
        const float dither2 = ditherRandom.nextFloat() - 0.5f;
        const float dither = (dither1 + dither2) * stepSize * ditherScale;

        // Apply dither before quantization
        float dithered = input + dither;

        // Quantize
        float quantized = std::round(dithered * levels) / levels;

        // Simple first-order noise shaping (error feedback)
        // Pushes quantization noise to higher frequencies
        // Use reduced feedback for low bit depths to prevent instability
        float& noiseShapeError = isLeft ? noiseShapeErrorL : noiseShapeErrorR;
        float error = input - quantized;
        float noiseShapeFactor = bits < 8.0f ? 0.25f : 0.5f;
        float shaped = quantized + noiseShapeError * noiseShapeFactor;
        noiseShapeError = error;

        // Soft clamp to avoid overflow from dither
        return std::clamp(shaped, -1.0f, 1.0f);
    }

    void processSampleRateReduction(float& left, float& right, float targetRate)
    {
        if (targetRate >= currentSampleRate - 100.0f)
            return;  // No reduction needed

        // Anti-aliasing: Apply low-pass filter before downsampling
        // This prevents aliasing artifacts that make the effect sound harsh
        // Use a simple but effective 2-pole butterworth at the target Nyquist
        const float nyquist = targetRate * 0.45f;  // Slightly below Nyquist for safety
        updateSRAntiAliasCoeffs(nyquist);

        // Apply anti-alias filter
        left = processSRAntiAlias(left, true);
        right = processSRAntiAlias(right, false);

        // Calculate step size for sample-and-hold
        const float step = static_cast<float>(currentSampleRate) / targetRate;

        srCounter += 1.0f;
        if (srCounter >= step)
        {
            srCounter -= step;
            // Store previous value before capturing new one
            srPrevHoldL = srHoldL;
            srPrevHoldR = srHoldR;
            // Capture new sample
            srHoldL = left;
            srHoldR = right;
        }

        // Linear interpolation between held samples for smoother output
        // This acts as a simple reconstruction filter
        const float t = srCounter / step;  // 0-1 interpolation factor
        left = srPrevHoldL * (1.0f - t) + srHoldL * t;
        right = srPrevHoldR * (1.0f - t) + srHoldR * t;
    }

    void updateSRAntiAliasCoeffs(float freq)
    {
        // Only update if frequency changed significantly
        if (std::abs(freq - lastSRAAFreq) < 10.0f)
            return;

        lastSRAAFreq = freq;

        // 2-pole Butterworth low-pass for anti-aliasing
        const float omega = 2.0f * juce::MathConstants<float>::pi * freq / static_cast<float>(currentSampleRate);
        const float sinOmega = std::sin(omega);
        const float cosOmega = std::cos(omega);
        const float alpha = sinOmega / (2.0f * 0.707f);  // Q = 0.707 for Butterworth

        const float a0 = 1.0f + alpha;
        srAAB0 = ((1.0f - cosOmega) / 2.0f) / a0;
        srAAB1 = (1.0f - cosOmega) / a0;
        srAAB2 = ((1.0f - cosOmega) / 2.0f) / a0;
        srAAA1 = (-2.0f * cosOmega) / a0;
        srAAA2 = (1.0f - alpha) / a0;
    }

    float processSRAntiAlias(float input, bool isLeft)
    {
        float& x1 = isLeft ? srAAX1L : srAAX1R;
        float& x2 = isLeft ? srAAX2L : srAAX2R;
        float& y1 = isLeft ? srAAY1L : srAAY1R;
        float& y2 = isLeft ? srAAY2L : srAAY2R;

        float output = srAAB0 * input + srAAB1 * x1 + srAAB2 * x2 - srAAA1 * y1 - srAAA2 * y2;

        x2 = x1;
        x1 = input;
        y2 = y1;
        y1 = output;

        return output;
    }

    void processWobble(float& left, float& right, float amount)
    {
        // Tape flutter effect: use a slow LFO to modulate delay time
        // This creates pitch variations similar to tape wow/flutter

        if (wobbleDelayBufferL.empty())
            return;

        const int bufferSize = static_cast<int>(wobbleDelayBufferL.size());

        // Write current sample to delay buffer
        wobbleDelayBufferL[wobbleWritePos] = left;
        wobbleDelayBufferR[wobbleWritePos] = right;

        // LFO for flutter effect - slower rates for smoother tape-like wobble
        // Use multiple LFOs at different rates for more organic sound
        // Primary: ~2Hz (slow wow), Secondary: ~0.3Hz (very slow drift)
        const float primaryRate = 2.0f / static_cast<float>(currentSampleRate);

        wobbleLfoPhase += primaryRate;
        if (wobbleLfoPhase >= 1.0f)
            wobbleLfoPhase -= 1.0f;

        // Combine two sine waves for more complex motion
        const float primaryLfo = std::sin(wobbleLfoPhase * 2.0f * juce::MathConstants<float>::pi);
        const float secondaryLfo = std::sin(wobbleLfoPhase * 0.15f * 2.0f * juce::MathConstants<float>::pi);

        // Combined modulation (primary contributes more)
        const float lfoValue = primaryLfo * 0.7f + secondaryLfo * 0.3f;

        // Base delay is ~15ms for more latency but smoother result
        // Modulation adds +/- up to 3ms based on amount (reduced for less crackling)
        const float baseDelaySamples = static_cast<float>(currentSampleRate) * 0.015f; // 15ms
        const float modulationDepth = static_cast<float>(currentSampleRate) * 0.003f * amount; // 0-3ms

        // Smooth the delay time to avoid sudden jumps that cause crackling
        const float targetDelay = baseDelaySamples + lfoValue * modulationDepth;
        wobbleDelaySmoothed = wobbleDelaySmoothed * 0.999f + targetDelay * 0.001f;

        // Calculate read position with fractional delay
        float readPos = static_cast<float>(wobbleWritePos) - wobbleDelaySmoothed;
        while (readPos < 0.0f)
            readPos += static_cast<float>(bufferSize);

        // Hermite interpolation for smoother delay changes (reduces crackling)
        const int idx0 = static_cast<int>(readPos) % bufferSize;
        const int idx1 = (idx0 + 1) % bufferSize;
        const int idxM1 = (idx0 - 1 + bufferSize) % bufferSize;
        const int idx2 = (idx0 + 2) % bufferSize;
        const float frac = readPos - std::floor(readPos);

        // Hermite interpolation for left channel
        left = hermiteInterpolate(
            wobbleDelayBufferL[idxM1], wobbleDelayBufferL[idx0],
            wobbleDelayBufferL[idx1], wobbleDelayBufferL[idx2], frac);
        // Hermite interpolation for right channel
        right = hermiteInterpolate(
            wobbleDelayBufferR[idxM1], wobbleDelayBufferR[idx0],
            wobbleDelayBufferR[idx1], wobbleDelayBufferR[idx2], frac);

        // Advance write position
        wobbleWritePos = (wobbleWritePos + 1) % bufferSize;
    }

    // Hermite (cubic) interpolation for smooth delay line reading
    float hermiteInterpolate(float y0, float y1, float y2, float y3, float frac)
    {
        const float c0 = y1;
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * frac + c2) * frac + c1) * frac + c0;
    }

    // ========================================
    // TEXTURE ENGINE METHODS
    // ========================================

    void initializeTexture(double sampleRate)
    {
        // Allocate texture buffer (~5.4 seconds at 48kHz)
        textureBufferL.resize(TEXTURE_BUFFER_SIZE, 0.0f);
        textureBufferR.resize(TEXTURE_BUFFER_SIZE, 0.0f);
        textureWritePos = 0;
        textureSpawnTimer = 0.0f;
        textureBufferFilled = 0;

        // Reset all grain voices
        for (auto& grain : textureGrains)
        {
            grain.active = false;
            grain.progress = 0.0f;
        }

        // Initialize smoothed parameters
        textureDensitySmooth.reset(sampleRate, 0.01);
        textureScatterSmooth.reset(sampleRate, 0.01);
        textureMotionSmooth.reset(sampleRate, 0.01);
        textureMixSmooth.reset(sampleRate, 0.01);

        textureDensitySmooth.setCurrentAndTargetValue(0.5f);
        textureScatterSmooth.setCurrentAndTargetValue(0.0f);
        textureMotionSmooth.setCurrentAndTargetValue(0.0f);
        textureMixSmooth.setCurrentAndTargetValue(0.5f);
    }

    // Linear interpolation helper
    static float lerp(float a, float b, float t)
    {
        return a + t * (b - a);
    }

    // Wrap buffer position to valid range
    float wrapBufferPos(float pos) const
    {
        while (pos < 0.0f)
            pos += static_cast<float>(TEXTURE_BUFFER_SIZE);
        while (pos >= static_cast<float>(TEXTURE_BUFFER_SIZE))
            pos -= static_cast<float>(TEXTURE_BUFFER_SIZE);
        return pos;
    }

    // Read from buffer with linear interpolation
    float readBufferInterpolated(const std::vector<float>& buffer, float pos) const
    {
        pos = wrapBufferPos(pos);
        const int idx0 = static_cast<int>(pos) % TEXTURE_BUFFER_SIZE;
        const int idx1 = (idx0 + 1) % TEXTURE_BUFFER_SIZE;
        const float frac = pos - std::floor(pos);
        return buffer[idx0] * (1.0f - frac) + buffer[idx1] * frac;
    }

    // Spawn a new grain voice
    void spawnGrain(float density, float scatter, float motion)
    {
        // Find an inactive grain slot, or steal the oldest one
        int slotToUse = -1;
        float oldestProgress = -1.0f;
        int oldestSlot = 0;

        for (int i = 0; i < NUM_TEXTURE_VOICES; ++i)
        {
            if (!textureGrains[i].active)
            {
                slotToUse = i;
                break;
            }
            if (textureGrains[i].progress > oldestProgress)
            {
                oldestProgress = textureGrains[i].progress;
                oldestSlot = i;
            }
        }

        // If no inactive slot, steal the oldest
        if (slotToUse < 0)
            slotToUse = oldestSlot;

        Grain& grain = textureGrains[slotToUse];

        // Calculate grain size based on density
        // density 0 = 200ms grains, density 1 = 5ms grains
        float grainSizeMs = lerp(200.0f, 5.0f, density);
        grain.grainLength = grainSizeMs * static_cast<float>(currentSampleRate) / 1000.0f;
        grain.grainLength = std::max(grain.grainLength, 48.0f);  // Minimum ~1ms at 48kHz

        // Calculate read position based on scatter
        // scatter 0 = read from recent audio (near write position)
        // scatter 1 = read from anywhere in the buffer
        float basePos = static_cast<float>(textureWritePos) - grain.grainLength;
        float scatterRange = static_cast<float>(std::min(textureBufferFilled, TEXTURE_BUFFER_SIZE)) * 0.8f * scatter;
        float randomOffset = (textureRandom.nextFloat() * 2.0f - 1.0f) * scatterRange;
        grain.readPosL = wrapBufferPos(basePos + randomOffset);
        grain.readPosR = grain.readPosL;  // Same position for stereo coherence

        // Per-grain variation based on motion
        // Pitch/speed variation: ±20% at max motion
        float speedVar = motion * 0.2f * (textureRandom.nextFloat() * 2.0f - 1.0f);
        grain.playbackRate = 1.0f + speedVar;

        // Reverse probability: up to 30% at max motion
        grain.reverse = textureRandom.nextFloat() < (motion * 0.3f);

        grain.progress = 0.0f;
        grain.active = true;
    }

    // Main texture processing method
    void processTexture(float& left, float& right, float density, float scatter, float motion)
    {
        if (textureBufferL.empty())
            return;

        // Write input to circular buffer
        textureBufferL[textureWritePos] = left;
        textureBufferR[textureWritePos] = right;

        // Track how much buffer is filled (for scatter range calculation)
        if (textureBufferFilled < TEXTURE_BUFFER_SIZE)
            textureBufferFilled++;

        // Check if we should spawn a new grain
        textureSpawnTimer -= 1.0f;
        if (textureSpawnTimer <= 0.0f)
        {
            spawnGrain(density, scatter, motion);

            // Calculate next spawn interval based on density
            // density 0 = spawn every 200ms, density 1 = spawn every 1ms
            float intervalMs = lerp(200.0f, 1.0f, density);
            textureSpawnTimer = intervalMs * static_cast<float>(currentSampleRate) / 1000.0f;
        }

        // Process all active grains
        float outputL = 0.0f;
        float outputR = 0.0f;
        int activeCount = 0;

        for (auto& grain : textureGrains)
        {
            if (!grain.active)
                continue;

            activeCount++;

            // Calculate Hann window envelope
            float window = 0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * grain.progress));

            // Read from buffer with interpolation
            float sampleL = readBufferInterpolated(textureBufferL, grain.readPosL);
            float sampleR = readBufferInterpolated(textureBufferR, grain.readPosR);

            // Apply window and accumulate
            outputL += sampleL * window;
            outputR += sampleR * window;

            // Advance grain read position
            float increment = grain.playbackRate;
            if (grain.reverse)
                increment = -increment;

            grain.readPosL = wrapBufferPos(grain.readPosL + increment);
            grain.readPosR = wrapBufferPos(grain.readPosR + increment);

            // Advance grain progress
            grain.progress += 1.0f / grain.grainLength;

            // Deactivate grain when finished
            if (grain.progress >= 1.0f)
                grain.active = false;
        }

        // Normalize output by sqrt of active count to maintain consistent volume
        if (activeCount > 1)
        {
            float normFactor = 1.0f / std::sqrt(static_cast<float>(activeCount));
            outputL *= normFactor;
            outputR *= normFactor;
        }

        // Output the granular texture
        left = outputL;
        right = outputR;

        // Advance write position
        textureWritePos = (textureWritePos + 1) % TEXTURE_BUFFER_SIZE;
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DegradeProcessor)
};
