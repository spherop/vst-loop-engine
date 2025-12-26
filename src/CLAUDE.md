# Loop Engine DSP Architecture

## Pitch Shifting System

### Block-Based Processing (v1.3.3+)
The pitch shifting system uses block-based processing for optimal CPU efficiency.

**Key Classes (PhaseVocoder.h):**

- `BlockPitchShifter` - Efficient mono block processor using Signalsmith Stretch
  - Uses `presetCheaper` for better performance (still high quality)
  - Processes entire audio blocks in single FFT pass
  - Includes latency compensation buffer

- `StereoBlockPitchShifter` - Stereo wrapper, processes L/R channels in blocks

- `SignalsmithPitchShifter` - Legacy sample-by-sample processor (kept for compatibility)
  - Higher CPU usage due to ring buffer overhead
  - Used during overdubbing where block processing isn't feasible

### LoopBuffer Integration

**State-Based Processing:**
- `Playing` state uses optimized `processPlayingBlock()` - reads entire block, pitch shifts in one call
- `Recording` and `Overdubbing` states use sample-by-sample (real-time input requirement)

**Pre-allocated Buffers:**
```cpp
std::vector<float> pitchInputL, pitchInputR;   // Raw loop audio
std::vector<float> pitchOutputL, pitchOutputR; // Pitch-shifted output
StereoBlockPitchShifter blockPitchShifter;     // Block processor
StereoPhaseVocoder phaseVocoder;               // Legacy (overdub monitoring)
```

### Performance Considerations

With 8 layers, the system could have up to 16 pitch shifter instances (2 per layer for stereo). Block processing dramatically reduces CPU overhead:

- **Before:** Per-sample ring buffer management + frequent small FFTs
- **After:** Single block-sized FFT per audio callback per layer

### Signalsmith Stretch Presets

The library offers different quality/CPU tradeoffs:
- `presetDefault()` - Highest quality, more CPU
- `presetCheaper()` - Good quality, lower CPU (currently used)

## Layer System

Each `LoopBuffer` is independent with its own:
- Audio buffers (bufferL, bufferR)
- Playhead position
- Fade multiplier
- Pitch shifter instance

The `LoopEngine` coordinates all layers, syncing playheads and managing state transitions.

## Diagnostic Metering

Added in v1.3.2 for debugging audio issues:
- Pre-clip peak levels (L/R)
- Loop output peak levels (L/R)
- Clip event counter
- Per-layer clip counts

Access via `getAudioDiagnostics` native function in UI.
