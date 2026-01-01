#pragma once

#include <juce_dsp/juce_dsp.h>
#include <vector>
#include <cmath>
#include <atomic>

/**
 * MicroLooper - MOOD MKII Inspired Micro-Looper
 *
 * An "always-listening" micro-looper that continuously records audio.
 * When activated, it plays back the captured moment with various manipulation modes.
 *
 * Key concepts from MOOD:
 * - Always-listening buffer: continuously records when not playing
 * - Clock control: sample rate affects both pitch and length
 * - Three modes: ENV (envelope), TAPE (speed/direction), STRETCH (time-stretch)
 * - Overdub: layer recordings on top of each other
 * - Freeze: lock the buffer and stop recording
 */
class MicroLooper
{
public:
    // Operating modes
    enum class Mode
    {
        ENV,     // Envelope-reactive mode - audio input gates/ducks the loop
        TAPE,    // Tape mode - manual speed and direction control
        STRETCH  // Time-stretch mode - length without pitch change
    };

    // Scale modes for pitch quantization
    enum class Scale
    {
        FREE,       // No quantization - continuous speed
        CHROMATIC,  // All 12 semitones
        MAJOR,      // Major scale intervals (1, 2, 3, 4, 5, 6, 7)
        MINOR,      // Natural minor scale
        PENTATONIC, // Pentatonic scale (1, 2, 3, 5, 6)
        OCTAVES     // Only octaves (1x, 0.5x, 2x)
    };

    MicroLooper() = default;

    void prepare(double sampleRate, int /*samplesPerBlock*/)
    {
        currentSampleRate = sampleRate;

        // Allocate buffer - 16 seconds at max sample rate
        // Clock control scales this: high clock = 0.5s, low clock = 16s
        maxBufferSize = static_cast<int>(sampleRate * 16.0);
        bufferL.resize(maxBufferSize, 0.0f);
        bufferR.resize(maxBufferSize, 0.0f);

        // Initialize state
        writePos = 0;
        readPos = 0.0f;
        bufferLength = maxBufferSize;
        capturedLoopStart = 0;
        capturedLength = 0;
        samplesRecorded = 0;
        isPlaying = false;

        // Smoothed values for click-free operation
        playbackSpeedSmooth.reset(sampleRate, 0.02);
        playbackSpeedSmooth.setCurrentAndTargetValue(1.0f);

        mixSmooth.reset(sampleRate, 0.02);
        mixSmooth.setCurrentAndTargetValue(1.0f);

        clockSmooth.reset(sampleRate, 0.05);  // Slower smoothing for clock
        clockSmooth.setCurrentAndTargetValue(0.5f);  // Default to middle

        lengthSmooth.reset(sampleRate, 0.02);
        lengthSmooth.setCurrentAndTargetValue(1.0f);

        modifySmooth.reset(sampleRate, 0.02);
        modifySmooth.setCurrentAndTargetValue(0.5f);

        bypassGainSmooth.reset(sampleRate, 0.02);
        bypassGainSmooth.setTargetValue(0.0f);

        // Crossfade for seamless loop points (15ms for smooth transitions)
        crossfadeLength = static_cast<int>(sampleRate * 0.015);

        // Envelope follower for ENV mode
        envelopeFollower = 0.0f;

        // Reset overdub state
        overdubLevel = 0.7f;

        // Stretch mode state
        stretchGrainPos = 0.0f;
        stretchGrainPhase = 0.0f;

        // Crossfade state
        crossfadePos = 0;
        inCrossfade = false;
    }

    void processBlock(juce::AudioBuffer<float>& buffer)
    {
        const int numSamples = buffer.getNumSamples();
        float* leftChannel = buffer.getWritePointer(0);
        float* rightChannel = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;

        for (int i = 0; i < numSamples; ++i)
        {
            const float inputL = leftChannel[i];
            const float inputR = rightChannel ? rightChannel[i] : inputL;

            // Get smoothed values
            const float bypassGain = bypassGainSmooth.getNextValue();

            // Early exit if bypassed
            if (bypassGain < 0.0001f)
            {
                // Consume smoothed values to keep them updating
                playbackSpeedSmooth.getNextValue();
                mixSmooth.getNextValue();
                clockSmooth.getNextValue();
                lengthSmooth.getNextValue();
                modifySmooth.getNextValue();
                continue;
            }

            const float speed = playbackSpeedSmooth.getNextValue();
            const float mix = mixSmooth.getNextValue();
            const float clock = clockSmooth.getNextValue();
            const float length = lengthSmooth.getNextValue();
            const float modify = modifySmooth.getNextValue();

            // Calculate effective buffer length based on clock
            // Clock 1.0 = 0.5s (short), Clock 0.0 = 16s (long)
            float effectiveSeconds = 0.5f + (1.0f - clock) * 15.5f;
            int effectiveLength = static_cast<int>(effectiveSeconds * currentSampleRate);
            effectiveLength = std::clamp(effectiveLength, 2000, maxBufferSize);

            float outputL = 0.0f;
            float outputR = 0.0f;

            // Always-listening: record to buffer when not playing or in overdub mode
            if (!isPlaying || isOverdubbing.load())
            {
                if (!isFrozen.load())
                {
                    if (isOverdubbing.load() && isPlaying)
                    {
                        // In overdub mode during playback, mix with existing content
                        bufferL[writePos] = bufferL[writePos] * 0.6f + inputL * overdubLevel;
                        bufferR[writePos] = bufferR[writePos] * 0.6f + inputR * overdubLevel;
                    }
                    else if (!isPlaying)
                    {
                        // Normal recording when not playing
                        bufferL[writePos] = inputL;
                        bufferR[writePos] = inputR;
                    }

                    writePos = (writePos + 1) % effectiveLength;

                    // Track how much we've recorded (up to effective length)
                    if (samplesRecorded < effectiveLength)
                        samplesRecorded++;
                }
            }

            // Playback
            if (isPlaying && capturedLength > 0)
            {
                // Apply length control to get active portion of captured loop
                int activeLength = static_cast<int>(capturedLength * length);
                activeLength = std::max(activeLength, 200);  // Minimum ~5ms at 44.1kHz

                switch (currentMode)
                {
                    case Mode::ENV:
                        processEnvMode(inputL, inputR, outputL, outputR, activeLength, modify);
                        break;

                    case Mode::TAPE:
                        processTapeMode(outputL, outputR, activeLength, speed, modify);
                        break;

                    case Mode::STRETCH:
                        processStretchMode(outputL, outputR, activeLength, speed, modify);
                        break;
                }
            }

            // Mix dry and wet with proper gain staging
            float wetL = outputL * mix;
            float wetR = outputR * mix;
            float dryL = inputL * (1.0f - mix * 0.5f);  // Keep some dry signal
            float dryR = inputR * (1.0f - mix * 0.5f);

            // Apply bypass crossfade
            leftChannel[i] = inputL * (1.0f - bypassGain) + (dryL + wetL) * bypassGain;
            if (rightChannel)
                rightChannel[i] = inputR * (1.0f - bypassGain) + (dryR + wetR) * bypassGain;
        }
    }

    // === CONTROLS ===

    // Clock: 0-1 where 1 = short loop (0.5s), 0 = long loop (16s)
    void setClock(float value)
    {
        clockSmooth.setTargetValue(std::clamp(value, 0.0f, 1.0f));
    }

    // Length: 0-1 where 1 = full loop, 0 = tiny slice
    void setLength(float value)
    {
        lengthSmooth.setTargetValue(std::clamp(value, 0.05f, 1.0f));
    }

    // Modify: mode-specific behavior (0-1)
    void setModify(float value)
    {
        modifySmooth.setTargetValue(std::clamp(value, 0.0f, 1.0f));
    }

    // Playback speed multiplier (for TAPE/STRETCH modes)
    // Input: 0-1 normalized, where 0.5 = 1x speed
    void setSpeed(float value)
    {
        // Map 0-1 to speed: 0 = -2x, 0.25 = -0.5x, 0.5 = 1x, 0.75 = 1.5x, 1 = 2x
        float speed;
        if (value < 0.5f)
        {
            // Reverse speeds: 0 = -2x, 0.5 = 1x
            speed = -2.0f + value * 6.0f;  // 0->-2, 0.5->1
        }
        else
        {
            // Forward speeds: 0.5 = 1x, 1 = 2x
            speed = 1.0f + (value - 0.5f) * 2.0f;  // 0.5->1, 1->2
        }

        // Small dead zone around 1x for easy normal speed
        if (std::abs(speed - 1.0f) < 0.08f) speed = 1.0f;

        // Apply scale quantization if not FREE mode
        speed = quantizeToScale(speed);

        playbackSpeedSmooth.setTargetValue(speed);
    }

    // Scale mode selection
    void setScale(Scale scale)
    {
        currentScale = scale;
    }

    void setScale(int scaleIndex)
    {
        switch (scaleIndex)
        {
            case 0: currentScale = Scale::FREE; break;
            case 1: currentScale = Scale::CHROMATIC; break;
            case 2: currentScale = Scale::MAJOR; break;
            case 3: currentScale = Scale::MINOR; break;
            case 4: currentScale = Scale::PENTATONIC; break;
            case 5: currentScale = Scale::OCTAVES; break;
            default: currentScale = Scale::FREE; break;
        }
    }

    Scale getScale() const { return currentScale; }
    int getScaleIndex() const { return static_cast<int>(currentScale); }

    // Reverse toggle
    void setReverse(bool reverse)
    {
        isReversed.store(reverse);
    }

    // Mode selection
    void setMode(Mode mode)
    {
        currentMode = mode;
    }

    void setMode(int modeIndex)
    {
        switch (modeIndex)
        {
            case 0: currentMode = Mode::ENV; break;
            case 1: currentMode = Mode::TAPE; break;
            case 2: currentMode = Mode::STRETCH; break;
            default: currentMode = Mode::TAPE; break;
        }
    }

    // Mix: 0-1 dry/wet
    void setMix(float value)
    {
        mixSmooth.setTargetValue(std::clamp(value, 0.0f, 1.0f));
    }

    // Transport controls
    void play()
    {
        if (!isPlaying)
        {
            // Capture the current moment
            // The loop starts from where we are now, going back by the effective length
            int effectiveLength = std::min(samplesRecorded, maxBufferSize);
            if (effectiveLength > 0)
            {
                capturedLoopStart = (writePos - effectiveLength + maxBufferSize) % maxBufferSize;
                capturedLength = effectiveLength;
                readPos = 0.0f;
                stretchGrainPos = 0.0f;
                stretchGrainPhase = 0.0f;
                isPlaying = true;
                DBG("MicroLooper: PLAY - captured " + juce::String(capturedLength) +
                    " samples starting at " + juce::String(capturedLoopStart));
            }
        }
    }

    void stop()
    {
        isPlaying = false;
        isOverdubbing.store(false);
        readPos = 0.0f;
        DBG("MicroLooper: STOP");
    }

    void togglePlay()
    {
        if (isPlaying)
            stop();
        else
            play();
    }

    // Overdub: record on top of existing loop
    void setOverdub(bool on)
    {
        isOverdubbing.store(on);
        DBG("MicroLooper: OVERDUB " + juce::String(on ? "ON" : "OFF"));
    }

    void toggleOverdub()
    {
        setOverdub(!isOverdubbing.load());
    }

    // Freeze: stop recording, keep playing
    void setFreeze(bool on)
    {
        isFrozen.store(on);
        DBG("MicroLooper: FREEZE " + juce::String(on ? "ON" : "OFF"));
    }

    void toggleFreeze()
    {
        setFreeze(!isFrozen.load());
    }

    // Clear: reset buffer
    void clear()
    {
        std::fill(bufferL.begin(), bufferL.end(), 0.0f);
        std::fill(bufferR.begin(), bufferR.end(), 0.0f);
        writePos = 0;
        readPos = 0.0f;
        samplesRecorded = 0;
        capturedLoopStart = 0;
        capturedLength = 0;
        isPlaying = false;
        isOverdubbing.store(false);
        isFrozen.store(false);
        stretchGrainPos = 0.0f;
        stretchGrainPhase = 0.0f;
        DBG("MicroLooper: CLEAR");
    }

    // Master enable/bypass
    void setEnabled(bool on)
    {
        enabled.store(on);
        bypassGainSmooth.setTargetValue(on ? 1.0f : 0.0f);
    }

    bool isEnabled() const { return enabled.load(); }
    bool getIsPlaying() const { return isPlaying; }
    bool getIsOverdubbing() const { return isOverdubbing.load(); }
    bool getIsFrozen() const { return isFrozen.load(); }
    int getCurrentMode() const { return static_cast<int>(currentMode); }

    // Get playhead position for visualization (0-1)
    float getPlayheadPosition() const
    {
        if (!isPlaying || capturedLength == 0)
            return 0.0f;
        return std::clamp(static_cast<float>(readPos) / static_cast<float>(capturedLength), 0.0f, 1.0f);
    }

    // Get recording position for visualization (0-1)
    float getRecordPosition() const
    {
        if (samplesRecorded == 0)
            return 0.0f;
        return static_cast<float>(writePos) / static_cast<float>(std::max(samplesRecorded, 1));
    }

    // Get buffer fill level (0-1)
    float getBufferFill() const
    {
        if (maxBufferSize == 0) return 0.0f;
        return static_cast<float>(samplesRecorded) / static_cast<float>(maxBufferSize);
    }

    // Get waveform data for visualization (returns peak values)
    std::vector<float> getWaveformData(int numPoints) const
    {
        std::vector<float> waveform(numPoints, 0.0f);

        if (samplesRecorded == 0 || numPoints == 0)
            return waveform;

        int samplesPerPoint = std::max(1, samplesRecorded / numPoints);

        for (int i = 0; i < numPoints; ++i)
        {
            float maxVal = 0.0f;
            int startIdx = (i * samplesRecorded) / numPoints;
            int endIdx = std::min(startIdx + samplesPerPoint, samplesRecorded);

            for (int j = startIdx; j < endIdx; ++j)
            {
                int bufIdx = j % maxBufferSize;
                float val = std::abs(bufferL[bufIdx]) + std::abs(bufferR[bufIdx]);
                maxVal = std::max(maxVal, val * 0.5f);
            }
            waveform[i] = maxVal;
        }

        return waveform;
    }

private:
    double currentSampleRate = 44100.0;
    int maxBufferSize = 0;

    // Audio buffers
    std::vector<float> bufferL, bufferR;
    int writePos = 0;
    float readPos = 0.0f;  // Floating point for sub-sample interpolation
    int bufferLength = 0;
    int capturedLoopStart = 0;  // Where the captured loop starts in the buffer
    int capturedLength = 0;     // Length of the captured loop
    int samplesRecorded = 0;

    // State
    bool isPlaying = false;
    std::atomic<bool> isOverdubbing { false };
    std::atomic<bool> isFrozen { false };
    std::atomic<bool> isReversed { false };
    std::atomic<bool> enabled { false };

    float overdubLevel = 0.7f;
    Mode currentMode = Mode::TAPE;
    Scale currentScale = Scale::FREE;  // Default to no quantization

    // Smoothed parameters
    juce::SmoothedValue<float> playbackSpeedSmooth;
    juce::SmoothedValue<float> mixSmooth;
    juce::SmoothedValue<float> clockSmooth;
    juce::SmoothedValue<float> lengthSmooth;
    juce::SmoothedValue<float> modifySmooth;
    juce::SmoothedValue<float> bypassGainSmooth;

    // Crossfade
    int crossfadeLength = 0;
    int crossfadePos = 0;
    bool inCrossfade = false;

    // ENV mode state
    float envelopeFollower = 0.0f;

    // STRETCH mode state
    float stretchGrainPos = 0.0f;
    float stretchGrainPhase = 0.0f;

    // Quantize speed to musical intervals based on current scale
    // Speed maps to pitch: 1.0 = unison, 2.0 = octave up, 0.5 = octave down
    float quantizeToScale(float speed) const
    {
        if (currentScale == Scale::FREE)
            return speed;

        // Handle negative speeds (reverse playback)
        const bool isNegative = speed < 0.0f;
        float absSpeed = std::abs(speed);

        // Clamp to reasonable range
        absSpeed = std::clamp(absSpeed, 0.25f, 4.0f);

        // Convert speed to semitones relative to 1.0x
        // speed = 2^(semitones/12), so semitones = 12 * log2(speed)
        float semitones = 12.0f * std::log2(absSpeed);

        // Define scale intervals in semitones relative to root
        // Each scale spans 2 octaves down to 2 octaves up (-24 to +24 semitones)
        std::vector<float> scaleIntervals;

        switch (currentScale)
        {
            case Scale::CHROMATIC:
                // All 12 semitones
                for (int i = -24; i <= 24; ++i)
                    scaleIntervals.push_back(static_cast<float>(i));
                break;

            case Scale::MAJOR:
                // Major scale: 0, 2, 4, 5, 7, 9, 11 (and octaves)
                {
                    const int majorPattern[] = { 0, 2, 4, 5, 7, 9, 11 };
                    for (int octave = -2; octave <= 2; ++octave)
                    {
                        for (int interval : majorPattern)
                            scaleIntervals.push_back(static_cast<float>(octave * 12 + interval));
                    }
                }
                break;

            case Scale::MINOR:
                // Natural minor: 0, 2, 3, 5, 7, 8, 10 (and octaves)
                {
                    const int minorPattern[] = { 0, 2, 3, 5, 7, 8, 10 };
                    for (int octave = -2; octave <= 2; ++octave)
                    {
                        for (int interval : minorPattern)
                            scaleIntervals.push_back(static_cast<float>(octave * 12 + interval));
                    }
                }
                break;

            case Scale::PENTATONIC:
                // Major pentatonic: 0, 2, 4, 7, 9 (and octaves)
                {
                    const int pentaPattern[] = { 0, 2, 4, 7, 9 };
                    for (int octave = -2; octave <= 2; ++octave)
                    {
                        for (int interval : pentaPattern)
                            scaleIntervals.push_back(static_cast<float>(octave * 12 + interval));
                    }
                }
                break;

            case Scale::OCTAVES:
                // Only octaves: -24, -12, 0, 12, 24
                scaleIntervals = { -24.0f, -12.0f, 0.0f, 12.0f, 24.0f };
                break;

            default:
                return speed;
        }

        // Find closest scale interval
        float closestInterval = 0.0f;
        float minDistance = 1000.0f;

        for (float interval : scaleIntervals)
        {
            float distance = std::abs(semitones - interval);
            if (distance < minDistance)
            {
                minDistance = distance;
                closestInterval = interval;
            }
        }

        // Convert back to speed: speed = 2^(semitones/12)
        float quantizedSpeed = std::pow(2.0f, closestInterval / 12.0f);

        // Restore sign for reverse playback
        return isNegative ? -quantizedSpeed : quantizedSpeed;
    }

    // Read from buffer with Hermite interpolation for smooth playback
    float readBufferHermite(const std::vector<float>& buffer, float pos, int loopStart, int loopLength) const
    {
        // Wrap position within loop
        while (pos < 0.0f)
            pos += static_cast<float>(loopLength);
        while (pos >= static_cast<float>(loopLength))
            pos -= static_cast<float>(loopLength);

        // Get actual buffer indices
        auto getIdx = [&](int offset) {
            int idx = loopStart + static_cast<int>(pos) + offset;
            // Wrap within the loop region
            while (idx < loopStart) idx += loopLength;
            while (idx >= loopStart + loopLength) idx -= loopLength;
            // Wrap within buffer bounds
            return idx % maxBufferSize;
        };

        const int idx0 = getIdx(0);
        const int idxM1 = getIdx(-1);
        const int idx1 = getIdx(1);
        const int idx2 = getIdx(2);
        const float frac = pos - std::floor(pos);

        const float y0 = buffer[idxM1];
        const float y1 = buffer[idx0];
        const float y2 = buffer[idx1];
        const float y3 = buffer[idx2];

        // Hermite interpolation coefficients
        const float c0 = y1;
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);

        return ((c3 * frac + c2) * frac + c1) * frac + c0;
    }

    // Calculate crossfade gain for loop boundaries
    float getCrossfadeGain(float pos, int loopLength) const
    {
        if (crossfadeLength <= 0 || loopLength <= crossfadeLength * 2)
            return 1.0f;

        // Fade in at start
        if (pos < crossfadeLength)
        {
            return pos / static_cast<float>(crossfadeLength);
        }
        // Fade out at end
        else if (pos > loopLength - crossfadeLength)
        {
            return (loopLength - pos) / static_cast<float>(crossfadeLength);
        }
        return 1.0f;
    }

    // ENV mode: input envelope gates/modulates the loop playback
    void processEnvMode(float inputL, float inputR, float& outputL, float& outputR,
                        int activeLength, float modify)
    {
        // Calculate input envelope
        float inputLevel = (std::abs(inputL) + std::abs(inputR)) * 0.5f;

        // Envelope follower with attack/release
        float attackCoeff = 0.005f;   // ~5ms attack
        float releaseCoeff = 0.0005f; // ~50ms release

        if (inputLevel > envelopeFollower)
            envelopeFollower += (inputLevel - envelopeFollower) * attackCoeff;
        else
            envelopeFollower += (inputLevel - envelopeFollower) * releaseCoeff;

        // Modify controls envelope sensitivity
        // Low modify = loop always plays; high modify = loop ducks when input is loud
        float threshold = 0.1f * (1.0f - modify);
        float loopGain = 1.0f;

        if (modify > 0.05f)
        {
            // Calculate ducking: louder input = quieter loop
            float duckAmount = std::clamp((envelopeFollower - threshold) * modify * 5.0f, 0.0f, 1.0f);
            loopGain = 1.0f - duckAmount;
        }

        // Read from buffer with crossfade
        float xfadeGain = getCrossfadeGain(readPos, activeLength);
        outputL = readBufferHermite(bufferL, readPos, capturedLoopStart, activeLength) * loopGain * xfadeGain;
        outputR = readBufferHermite(bufferR, readPos, capturedLoopStart, activeLength) * loopGain * xfadeGain;

        // Advance playhead (always forward in ENV mode at 1x speed)
        readPos += 1.0f;
        if (readPos >= static_cast<float>(activeLength))
            readPos -= static_cast<float>(activeLength);
    }

    // TAPE mode: speed and direction control like a tape reel
    void processTapeMode(float& outputL, float& outputR,
                         int activeLength, float speed, float modify)
    {
        // Apply reverse if toggled
        float effectiveSpeed = isReversed.load() ? -speed : speed;

        // Read from buffer with crossfade
        float xfadeGain = getCrossfadeGain(readPos, activeLength);
        outputL = readBufferHermite(bufferL, readPos, capturedLoopStart, activeLength) * xfadeGain;
        outputR = readBufferHermite(bufferR, readPos, capturedLoopStart, activeLength) * xfadeGain;

        // Modify in TAPE mode controls a subtle pitch wobble (like tape wow)
        float wobbleAmount = modify * 0.002f;
        float wobble = std::sin(readPos * 0.01f) * wobbleAmount;

        // Advance playhead with speed and wobble
        readPos += effectiveSpeed + wobble;

        // Wrap around loop
        while (readPos < 0.0f)
            readPos += static_cast<float>(activeLength);
        while (readPos >= static_cast<float>(activeLength))
            readPos -= static_cast<float>(activeLength);
    }

    // STRETCH mode: time-stretch using granular technique (change speed without pitch)
    void processStretchMode(float& outputL, float& outputR,
                            int activeLength, float speed, float modify)
    {
        // Modify controls grain size: 0 = tiny grains (10ms), 1 = large grains (150ms)
        float grainSizeMs = 10.0f + modify * 140.0f;
        float grainSizeSamples = grainSizeMs * static_cast<float>(currentSampleRate) / 1000.0f;
        grainSizeSamples = std::min(grainSizeSamples, static_cast<float>(activeLength) * 0.5f);

        // Two overlapping grains for smooth output (50% overlap)
        float windowPhase1 = stretchGrainPhase;
        float windowPhase2 = std::fmod(stretchGrainPhase + 0.5f, 1.0f);

        // Hann windows for each grain
        float window1 = 0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * windowPhase1));
        float window2 = 0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * windowPhase2));

        // Read positions for each grain (both read at normal speed through the grain)
        float grainOffset1 = windowPhase1 * grainSizeSamples;
        float grainOffset2 = windowPhase2 * grainSizeSamples;

        float pos1 = stretchGrainPos + grainOffset1;
        float pos2 = stretchGrainPos - grainSizeSamples * 0.5f + grainOffset2;

        // Read and window both grains
        float sample1L = readBufferHermite(bufferL, pos1, capturedLoopStart, activeLength) * window1;
        float sample1R = readBufferHermite(bufferR, pos1, capturedLoopStart, activeLength) * window1;
        float sample2L = readBufferHermite(bufferL, pos2, capturedLoopStart, activeLength) * window2;
        float sample2R = readBufferHermite(bufferR, pos2, capturedLoopStart, activeLength) * window2;

        // Sum grains
        outputL = sample1L + sample2L;
        outputR = sample1R + sample2R;

        // Apply reverse if toggled
        float effectiveSpeed = isReversed.load() ? -speed : speed;

        // Advance grain phase at normal rate (grains always play at original pitch)
        stretchGrainPhase += 1.0f / grainSizeSamples;
        if (stretchGrainPhase >= 1.0f)
            stretchGrainPhase -= 1.0f;

        // Advance grain position at the speed rate (controls time-stretch)
        stretchGrainPos += effectiveSpeed;

        // Also update readPos for playhead visualization
        readPos = stretchGrainPos;

        // Wrap around loop
        while (stretchGrainPos < 0.0f)
            stretchGrainPos += static_cast<float>(activeLength);
        while (stretchGrainPos >= static_cast<float>(activeLength))
            stretchGrainPos -= static_cast<float>(activeLength);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MicroLooper)
};
