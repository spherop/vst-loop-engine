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

        // Reset wobble
        wobblePhase = 0.0f;
        wobbleLfoPhase = 0.0f;

        // Initialize scrambler buffer (max 2 seconds at 48kHz for segments)
        int maxScrambleSize = static_cast<int>(sampleRate * 2.0);
        scrambleBufferL.resize(maxScrambleSize, 0.0f);
        scrambleBufferR.resize(maxScrambleSize, 0.0f);
        scrambleReadPos = 0;
        scrambleWritePos = 0;
        segmentPosition = 0;
        currentSegment = 0;

        // Initialize segment order
        for (int i = 0; i < MAX_SCRAMBLE_SEGMENTS; ++i)
            segmentOrder[i] = i;
    }

    void processBlock(juce::AudioBuffer<float>& buffer)
    {
        if (!enabled.load())
            return;

        const int numSamples = buffer.getNumSamples();
        float* leftChannel = buffer.getWritePointer(0);
        float* rightChannel = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;

        for (int i = 0; i < numSamples; ++i)
        {
            float inputL = leftChannel[i];
            float inputR = rightChannel ? rightChannel[i] : inputL;

            // Store dry signal
            float dryL = inputL;
            float dryR = inputR;

            // Get smoothed values
            const float hpFreq = hpFreqSmooth.getNextValue();
            const float hpQ = hpQSmooth.getNextValue();
            const float lpFreq = lpFreqSmooth.getNextValue();
            const float lpQ = lpQSmooth.getNextValue();
            const float bitDepth = bitDepthSmooth.getNextValue();
            const float srTarget = srReductionSmooth.getNextValue();
            const float wobbleAmt = wobbleAmountSmooth.getNextValue();
            const float mix = mixSmooth.getNextValue();

            // Update filter coefficients if needed
            updateHighPassCoeffs(hpFreq, hpQ);
            updateLowPassCoeffs(lpFreq, lpQ);

            // 1. High-pass filter (biquad)
            float wetL = processHighPass(inputL, true);
            float wetR = processHighPass(inputR, false);

            // 2. Low-pass filter (biquad)
            wetL = processLowPass(wetL, true);
            wetR = processLowPass(wetR, false);

            // 3. Bit crusher
            wetL = processBitCrush(wetL, bitDepth);
            wetR = processBitCrush(wetR, bitDepth);

            // 4. Sample rate reduction
            processSampleRateReduction(wetL, wetR, srTarget);

            // 5. Wobble (pitch/speed instability)
            if (wobbleAmt > 0.001f)
            {
                processWobble(wetL, wetR, wobbleAmt);
            }

            // 6. Scrambler
            if (scramblerSubdiv > 0)
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
        currentBpm = std::clamp(bpm, 20.0f, 300.0f);
        if (scramblerSubdiv > 0)
            calculateSegmentSize();
    }

    void setLoopLengthSamples(int len)
    {
        loopLengthSamples = len;
        if (scramblerSubdiv > 0)
            calculateSegmentSize();
    }

    void setMix(float mix)
    {
        mixSmooth.setTargetValue(std::clamp(mix, 0.0f, 1.0f));
    }

    void setEnabled(bool on)
    {
        enabled.store(on);
    }

private:
    double currentSampleRate = 44100.0;

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

    // Wobble (LFO-modulated pitch)
    float wobblePhase = 0.0f;
    float wobbleLfoPhase = 0.0f;
    static constexpr int WOBBLE_BUFFER_SIZE = 4096;
    std::array<float, WOBBLE_BUFFER_SIZE> wobbleBufferL{};
    std::array<float, WOBBLE_BUFFER_SIZE> wobbleBufferR{};
    int wobbleWritePos = 0;
    juce::SmoothedValue<float> wobbleAmountSmooth;

    // Scrambler
    static constexpr int MAX_SCRAMBLE_SEGMENTS = 32;
    std::vector<float> scrambleBufferL, scrambleBufferR;
    std::array<int, MAX_SCRAMBLE_SEGMENTS> segmentOrder{};
    int scrambleReadPos = 0;
    int scrambleWritePos = 0;
    int currentSegment = 0;
    int segmentSamples = 0;
    int segmentPosition = 0;
    int scramblerSubdiv = 0;
    float currentBpm = 120.0f;
    int loopLengthSamples = 0;
    juce::SmoothedValue<float> scramblerAmountSmooth;
    juce::Random scrambleRandom;

    // Mix
    juce::SmoothedValue<float> mixSmooth;
    std::atomic<bool> enabled { true };

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
        // Write to wobble buffer
        wobbleBufferL[wobbleWritePos] = left;
        wobbleBufferR[wobbleWritePos] = right;

        // LFO for random pitch/speed variation
        wobbleLfoPhase += (0.5f + scrambleRandom.nextFloat() * 2.0f) / static_cast<float>(currentSampleRate);
        if (wobbleLfoPhase >= 1.0f)
            wobbleLfoPhase -= 1.0f;

        // Generate wobble offset using sine + noise
        const float lfo = std::sin(wobbleLfoPhase * 2.0f * juce::MathConstants<float>::pi);
        const float noise = scrambleRandom.nextFloat() * 2.0f - 1.0f;

        // Wobble amount controls delay variation (0-50 samples)
        const float maxWobble = 50.0f * amount;
        const float wobbleOffset = (lfo * 0.7f + noise * 0.3f) * maxWobble;

        // Read position with wobble
        float readPos = static_cast<float>(wobbleWritePos) - 100.0f - wobbleOffset;
        while (readPos < 0.0f)
            readPos += WOBBLE_BUFFER_SIZE;

        // Linear interpolation
        const int idx0 = static_cast<int>(readPos) % WOBBLE_BUFFER_SIZE;
        const int idx1 = (idx0 + 1) % WOBBLE_BUFFER_SIZE;
        const float frac = readPos - std::floor(readPos);

        left = wobbleBufferL[idx0] * (1.0f - frac) + wobbleBufferL[idx1] * frac;
        right = wobbleBufferR[idx0] * (1.0f - frac) + wobbleBufferR[idx1] * frac;

        wobbleWritePos = (wobbleWritePos + 1) % WOBBLE_BUFFER_SIZE;
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
        const int subdivisions = 1 << (scramblerSubdiv + 1);  // 4, 8, 16, 32

        if (loopLengthSamples > 0)
        {
            // Free-time mode: divide loop into segments
            segmentSamples = loopLengthSamples / subdivisions;
        }
        else
        {
            // Tempo-synced mode
            const float beatsPerSecond = currentBpm / 60.0f;
            const float samplesPerBeat = static_cast<float>(currentSampleRate) / beatsPerSecond;
            const float samplesPerBar = samplesPerBeat * 4.0f;  // Assuming 4/4
            segmentSamples = static_cast<int>(samplesPerBar / static_cast<float>(subdivisions));
        }

        // Minimum segment size
        segmentSamples = std::max(segmentSamples, 256);

        // Regenerate segment order
        updateSegmentOrder();
    }

    void updateSegmentOrder()
    {
        const float amount = scramblerAmountSmooth.getCurrentValue();

        // Start with sequential order
        for (int i = 0; i < MAX_SCRAMBLE_SEGMENTS; ++i)
            segmentOrder[i] = i;

        // Shuffle based on amount (0 = no shuffle, 1 = full random)
        if (amount > 0.01f)
        {
            const int numSwaps = static_cast<int>(amount * MAX_SCRAMBLE_SEGMENTS);
            for (int i = 0; i < numSwaps; ++i)
            {
                const int a = scrambleRandom.nextInt(MAX_SCRAMBLE_SEGMENTS);
                const int b = scrambleRandom.nextInt(MAX_SCRAMBLE_SEGMENTS);
                std::swap(segmentOrder[a], segmentOrder[b]);
            }
        }
    }

    void processScrambler(float& left, float& right)
    {
        if (segmentSamples <= 0)
            return;

        // Write to scramble buffer
        const int writeIdx = scrambleWritePos % static_cast<int>(scrambleBufferL.size());
        scrambleBufferL[writeIdx] = left;
        scrambleBufferR[writeIdx] = right;
        scrambleWritePos++;

        // Calculate read position based on current segment
        const int numSegments = std::min(MAX_SCRAMBLE_SEGMENTS,
            static_cast<int>(scrambleBufferL.size()) / segmentSamples);

        if (numSegments <= 1)
        {
            // Not enough buffer for scrambling, pass through
            return;
        }

        // Map current segment to scrambled segment
        const int mappedSegment = segmentOrder[currentSegment % numSegments];

        // Calculate read position
        const int segmentStart = mappedSegment * segmentSamples;
        int readIdx = (segmentStart + segmentPosition) % static_cast<int>(scrambleBufferL.size());

        // Ensure we have valid data (don't read ahead of write)
        const int bufferDelay = numSegments * segmentSamples;
        if (scrambleWritePos < bufferDelay)
        {
            // Still filling buffer, pass through
            return;
        }

        // Read from scrambled position (with delay)
        readIdx = (scrambleWritePos - bufferDelay + readIdx) % static_cast<int>(scrambleBufferL.size());
        if (readIdx < 0)
            readIdx += static_cast<int>(scrambleBufferL.size());

        left = scrambleBufferL[readIdx];
        right = scrambleBufferR[readIdx];

        // Advance position within segment
        segmentPosition++;
        if (segmentPosition >= segmentSamples)
        {
            segmentPosition = 0;
            currentSegment++;

            // Periodically re-shuffle
            if (currentSegment % numSegments == 0)
            {
                updateSegmentOrder();
            }
        }
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DegradeProcessor)
};
