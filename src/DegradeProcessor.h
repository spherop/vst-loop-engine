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

        // Initialize smoothed values
        hpFreqSmooth.reset(sampleRate, 0.02);
        hpQSmooth.reset(sampleRate, 0.02);
        lpFreqSmooth.reset(sampleRate, 0.02);
        lpQSmooth.reset(sampleRate, 0.02);
        bitDepthSmooth.reset(sampleRate, 0.02);
        srReductionSmooth.reset(sampleRate, 0.02);
        wobbleAmountSmooth.reset(sampleRate, 0.05);
        scramblerAmountSmooth.reset(sampleRate, 0.02);
        mixSmooth.reset(sampleRate, 0.02);

        // Set initial values
        hpFreqSmooth.setCurrentAndTargetValue(20.0f);
        hpQSmooth.setCurrentAndTargetValue(0.707f);
        lpFreqSmooth.setCurrentAndTargetValue(20000.0f);
        lpQSmooth.setCurrentAndTargetValue(0.707f);
        bitDepthSmooth.setCurrentAndTargetValue(16.0f);
        srReductionSmooth.setCurrentAndTargetValue(static_cast<float>(sampleRate));
        wobbleAmountSmooth.setCurrentAndTargetValue(0.0f);
        scramblerAmountSmooth.setCurrentAndTargetValue(0.5f);
        mixSmooth.setCurrentAndTargetValue(1.0f);

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

        // Initialize scrambler buffer (store up to 4 bars worth at 60bpm = 16 seconds max)
        int maxScrambleSize = static_cast<int>(sampleRate * 8.0);
        scrambleBufferL.resize(maxScrambleSize, 0.0f);
        scrambleBufferR.resize(maxScrambleSize, 0.0f);
        scrambleWritePos = 0;
        scrambleReadPos = 0;
        segmentPosition = 0;
        currentSegment = 0;
        scrambleBufferFilled = false;

        // Initialize segment order
        for (int i = 0; i < MAX_SCRAMBLE_SEGMENTS; ++i)
            segmentOrder[i] = i;
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

            float wetL = inputL;
            float wetR = inputR;

            // Get smoothed values
            const float hpFreq = hpFreqSmooth.getNextValue();
            const float hpQ = hpQSmooth.getNextValue();
            const float lpFreq = lpFreqSmooth.getNextValue();
            const float lpQ = lpQSmooth.getNextValue();
            const float bitDepth = bitDepthSmooth.getNextValue();
            const float srTarget = srReductionSmooth.getNextValue();
            const float wobbleAmt = wobbleAmountSmooth.getNextValue();
            const float mix = mixSmooth.getNextValue();

            // ==== FILTER SECTION ====
            if (filterEnabled.load())
            {
                // Update filter coefficients if needed
                updateHighPassCoeffs(hpFreq, hpQ);
                updateLowPassCoeffs(lpFreq, lpQ);

                // High-pass filter (biquad)
                wetL = processHighPass(wetL, true);
                wetR = processHighPass(wetR, false);

                // Low-pass filter (biquad)
                wetL = processLowPass(wetL, true);
                wetR = processLowPass(wetR, false);
            }

            // ==== LO-FI SECTION ====
            if (lofiEnabled.load())
            {
                // Bit crusher
                wetL = processBitCrush(wetL, bitDepth);
                wetR = processBitCrush(wetR, bitDepth);

                // Sample rate reduction
                processSampleRateReduction(wetL, wetR, srTarget);

                // Wobble (tape flutter - pitch instability)
                if (wobbleAmt > 0.001f)
                {
                    processWobble(wetL, wetR, wobbleAmt);
                }
            }

            // ==== SCRAMBLER SECTION ====
            if (scramblerEnabled.load() && scramblerSubdiv > 0)
            {
                processScrambler(wetL, wetR);
            }

            // Apply dry/wet mix
            leftChannel[i] = dryL * (1.0f - mix) + wetL * mix;
            if (rightChannel)
                rightChannel[i] = dryR * (1.0f - mix) + wetR * mix;
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

    // Scrambler controls
    void setScramblerSubdiv(int subdiv)
    {
        // 0=off, 1=1/4, 2=1/8, 3=1/16, 4=1/32
        scramblerSubdiv = std::clamp(subdiv, 0, 4);
        if (subdiv > 0)
            calculateSegmentSize();
    }

    void setScramblerAmount(float amt)
    {
        scramblerAmountSmooth.setTargetValue(std::clamp(amt, 0.0f, 1.0f));
    }

    void setTempo(float bpm)
    {
        if (std::abs(bpm - currentBpm) > 0.1f)
        {
            currentBpm = std::clamp(bpm, 20.0f, 300.0f);
            if (scramblerSubdiv > 0)
                calculateSegmentSize();
        }
    }

    void setLoopLengthSamples(int len)
    {
        if (len != loopLengthSamples)
        {
            loopLengthSamples = len;
            if (scramblerSubdiv > 0)
                calculateSegmentSize();
        }
    }

    void setMix(float mix)
    {
        mixSmooth.setTargetValue(std::clamp(mix, 0.0f, 1.0f));
    }

    // Section bypass controls
    void setFilterEnabled(bool on) { filterEnabled.store(on); }
    void setLofiEnabled(bool on) { lofiEnabled.store(on); }
    void setScramblerEnabled(bool on) { scramblerEnabled.store(on); }

    bool getFilterEnabled() const { return filterEnabled.load(); }
    bool getLofiEnabled() const { return lofiEnabled.load(); }
    bool getScramblerEnabled() const { return scramblerEnabled.load(); }

    // Get current filter frequencies for visualization
    float getCurrentHPFreq() const { return lastHpFreq; }
    float getCurrentLPFreq() const { return lastLpFreq; }
    float getCurrentHPQ() const { return lastHpQ; }
    float getCurrentLPQ() const { return lastLpQ; }

private:
    double currentSampleRate = 44100.0;

    // Section bypass states
    std::atomic<bool> filterEnabled { true };
    std::atomic<bool> lofiEnabled { true };
    std::atomic<bool> scramblerEnabled { true };

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

    // Bit crusher
    juce::SmoothedValue<float> bitDepthSmooth;

    // Sample rate reducer
    float srHoldL = 0.0f, srHoldR = 0.0f;
    float srCounter = 0.0f;
    juce::SmoothedValue<float> srReductionSmooth;

    // Wobble (tape flutter effect using modulated delay)
    std::vector<float> wobbleDelayBufferL;
    std::vector<float> wobbleDelayBufferR;
    int wobbleWritePos = 0;
    float wobbleLfoPhase = 0.0f;
    juce::SmoothedValue<float> wobbleAmountSmooth;

    // Scrambler
    static constexpr int MAX_SCRAMBLE_SEGMENTS = 32;
    std::vector<float> scrambleBufferL, scrambleBufferR;
    std::array<int, MAX_SCRAMBLE_SEGMENTS> segmentOrder{};
    int scrambleWritePos = 0;
    int scrambleReadPos = 0;
    int currentSegment = 0;
    int segmentSamples = 0;
    int segmentPosition = 0;
    int scramblerSubdiv = 0;
    float currentBpm = 120.0f;
    int loopLengthSamples = 0;
    bool scrambleBufferFilled = false;
    juce::SmoothedValue<float> scramblerAmountSmooth;
    juce::Random scrambleRandom;

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

    float processBitCrush(float input, float bits)
    {
        if (bits >= 15.9f)
            return input;  // No crushing

        // Quantize to fewer bits
        const float levels = std::pow(2.0f, bits);
        return std::round(input * levels) / levels;
    }

    void processSampleRateReduction(float& left, float& right, float targetRate)
    {
        if (targetRate >= currentSampleRate - 100.0f)
            return;  // No reduction needed

        // Calculate step size for sample-and-hold
        const float step = static_cast<float>(currentSampleRate) / targetRate;

        srCounter += 1.0f;
        if (srCounter >= step)
        {
            srCounter -= step;
            srHoldL = left;
            srHoldR = right;
        }

        left = srHoldL;
        right = srHoldR;
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

        // LFO for flutter effect
        // Use multiple LFOs at different rates for more organic sound
        // Primary: ~4Hz (wow), Secondary: ~0.5Hz (flutter)
        const float primaryRate = 4.0f / static_cast<float>(currentSampleRate);
        const float secondaryRate = 0.5f / static_cast<float>(currentSampleRate);

        wobbleLfoPhase += primaryRate;
        if (wobbleLfoPhase >= 1.0f)
            wobbleLfoPhase -= 1.0f;

        // Combine two sine waves for more complex motion
        const float primaryLfo = std::sin(wobbleLfoPhase * 2.0f * juce::MathConstants<float>::pi);
        const float secondaryLfo = std::sin(wobbleLfoPhase * 0.125f * 2.0f * juce::MathConstants<float>::pi);

        // Combined modulation (primary contributes more)
        const float lfoValue = primaryLfo * 0.7f + secondaryLfo * 0.3f;

        // Base delay is ~10ms, modulation adds +/- 5ms based on amount
        // At 48kHz: 10ms = 480 samples, 5ms = 240 samples
        const float baseDelaySamples = static_cast<float>(currentSampleRate) * 0.010f; // 10ms
        const float modulationDepth = static_cast<float>(currentSampleRate) * 0.005f * amount; // 0-5ms

        const float delaySamples = baseDelaySamples + lfoValue * modulationDepth;

        // Calculate read position with fractional delay
        float readPos = static_cast<float>(wobbleWritePos) - delaySamples;
        while (readPos < 0.0f)
            readPos += static_cast<float>(bufferSize);

        // Linear interpolation for smooth delay changes
        const int idx0 = static_cast<int>(readPos) % bufferSize;
        const int idx1 = (idx0 + 1) % bufferSize;
        const float frac = readPos - std::floor(readPos);

        left = wobbleDelayBufferL[idx0] * (1.0f - frac) + wobbleDelayBufferL[idx1] * frac;
        right = wobbleDelayBufferR[idx0] * (1.0f - frac) + wobbleDelayBufferR[idx1] * frac;

        // Advance write position
        wobbleWritePos = (wobbleWritePos + 1) % bufferSize;
    }

    void calculateSegmentSize()
    {
        if (scramblerSubdiv == 0)
        {
            segmentSamples = 0;
            return;
        }

        // Calculate segment size based on subdivision
        // subdiv: 1=1/4, 2=1/8, 3=1/16, 4=1/32
        int subdivisions;
        switch (scramblerSubdiv)
        {
            case 1: subdivisions = 4; break;   // Quarter notes
            case 2: subdivisions = 8; break;   // Eighth notes
            case 3: subdivisions = 16; break;  // Sixteenth notes
            case 4: subdivisions = 32; break;  // Thirty-second notes
            default: subdivisions = 4;
        }

        if (loopLengthSamples > 0)
        {
            // Use loop length to determine segment size
            segmentSamples = loopLengthSamples / subdivisions;
        }
        else
        {
            // Fall back to tempo-based calculation (assume 1 bar)
            const float beatsPerSecond = currentBpm / 60.0f;
            const float samplesPerBeat = static_cast<float>(currentSampleRate) / beatsPerSecond;
            const float samplesPerBar = samplesPerBeat * 4.0f;  // Assuming 4/4
            segmentSamples = static_cast<int>(samplesPerBar * 4.0f / static_cast<float>(subdivisions));
        }

        // Ensure minimum segment size (avoid tiny glitchy segments)
        segmentSamples = std::max(segmentSamples, static_cast<int>(currentSampleRate * 0.05)); // Min 50ms

        // Reset scrambler state
        scrambleBufferFilled = false;
        scrambleWritePos = 0;
        scrambleReadPos = 0;
        segmentPosition = 0;
        currentSegment = 0;

        // Generate initial segment order
        updateSegmentOrder();
    }

    void updateSegmentOrder()
    {
        const float amount = scramblerAmountSmooth.getCurrentValue();

        // Number of segments we'll work with
        const int numSegments = std::min(MAX_SCRAMBLE_SEGMENTS,
            segmentSamples > 0 ? static_cast<int>(scrambleBufferL.size()) / segmentSamples : 1);

        // Start with sequential order
        for (int i = 0; i < numSegments; ++i)
            segmentOrder[i] = i;

        // Shuffle based on amount (0 = no shuffle, 1 = full random)
        if (amount > 0.01f && numSegments > 1)
        {
            // Fisher-Yates shuffle with probability based on amount
            for (int i = numSegments - 1; i > 0; --i)
            {
                if (scrambleRandom.nextFloat() < amount)
                {
                    const int j = scrambleRandom.nextInt(i + 1);
                    std::swap(segmentOrder[i], segmentOrder[j]);
                }
            }
        }
    }

    void processScrambler(float& left, float& right)
    {
        if (segmentSamples <= 0 || scrambleBufferL.empty())
            return;

        const int bufferSize = static_cast<int>(scrambleBufferL.size());
        const int numSegments = std::min(MAX_SCRAMBLE_SEGMENTS, bufferSize / segmentSamples);

        if (numSegments < 2)
            return;

        // Write incoming audio to buffer
        scrambleBufferL[scrambleWritePos] = left;
        scrambleBufferR[scrambleWritePos] = right;

        // Need to fill at least one full set of segments before outputting scrambled audio
        const int requiredSamples = numSegments * segmentSamples;

        if (!scrambleBufferFilled)
        {
            scrambleWritePos++;
            if (scrambleWritePos >= requiredSamples)
            {
                scrambleBufferFilled = true;
                scrambleWritePos = 0;
                scrambleReadPos = 0;
                segmentPosition = 0;
                currentSegment = 0;
                updateSegmentOrder();
            }
            // Pass through while filling buffer
            return;
        }

        // Map current segment position to scrambled segment
        const int mappedSegment = segmentOrder[currentSegment % numSegments];
        const int readOffset = mappedSegment * segmentSamples + segmentPosition;

        // Calculate actual read position in circular buffer
        int readIdx = readOffset % bufferSize;

        // Crossfade at segment boundaries to avoid clicks
        float crossfade = 1.0f;
        const int crossfadeSamples = std::min(64, segmentSamples / 8);

        if (segmentPosition < crossfadeSamples)
        {
            crossfade = static_cast<float>(segmentPosition) / static_cast<float>(crossfadeSamples);
        }
        else if (segmentPosition >= segmentSamples - crossfadeSamples)
        {
            crossfade = static_cast<float>(segmentSamples - segmentPosition) / static_cast<float>(crossfadeSamples);
        }

        // Read from scrambled position
        float scrambledL = scrambleBufferL[readIdx];
        float scrambledR = scrambleBufferR[readIdx];

        // Apply crossfade between original and scrambled
        left = left * (1.0f - crossfade) + scrambledL * crossfade;
        right = right * (1.0f - crossfade) + scrambledR * crossfade;

        // Advance positions
        scrambleWritePos = (scrambleWritePos + 1) % bufferSize;
        segmentPosition++;

        if (segmentPosition >= segmentSamples)
        {
            segmentPosition = 0;
            currentSegment++;

            // Re-shuffle periodically for variation
            if (currentSegment % numSegments == 0)
            {
                updateSegmentOrder();
            }
        }
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DegradeProcessor)
};
