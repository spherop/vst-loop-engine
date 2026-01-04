# Mix Bus & Per-Layer Effects Architecture Plan

## Overview

This document outlines the plan to extend Loop Engine's effects architecture from global-only processing to a flexible per-layer send system. The goal is to give users control over how much of each layer goes through various effect processors, similar to a traditional mixing console's aux send system.

---

## Current Architecture

### What We Have

**Per-Layer (already implemented):**
- Volume & Pan
- Mute/Solo
- 3-Band EQ (Low/Mid/High)
- Loop Start/End bounds
- Reverse toggle
- Pitch shifting via playback rate (Signalsmith Stretch, `presetCheaper`)

**Global Effects (single instance, processes mixed output):**
- **LOFI** (`DegradeProcessor.h` - note: code still uses "degrade" naming internally)
  - Bit Crusher
  - Sample Rate Reduction
  - Wobble (tape flutter)
  - Vinyl (hiss + crackle)
  - HP/LP Filters
- **TEXTURE** (granular engine inside DegradeProcessor)
  - 16-voice granular with density, size, pitch, spray, spread, reverse
  - Freeze mode
- **SATURATION** (`SaturationProcessor.h`)
  - Multiple algorithms (tube, tape, transistor, etc.)
- **SUB BASS** (`SubBassProcessor.h`)
  - Octave-down generator
- **REVERB** (`ReverbProcessor.h`)
  - JUCE built-in reverb
- **DELAY** (`DelayLine.h`)
  - Stereo delay with feedback

### Current Signal Flow

```
Layer 0 ─┐
Layer 1 ─┼──► Mix ──► DegradeProcessor ──► Saturation ──► SubBass ──► Reverb ──► Delay ──► Output
...      │         (LOFI + TEXTURE)
Layer 7 ─┘
```

---

## Phase 1: Per-Layer High-Quality Pitch Shifting

### Goal
Add a dedicated pitch shift control per layer in the existing layer settings panel (alongside EQ). This would be independent of playback speed, allowing users to transpose individual layers without affecting their timing.

### Current Pitch Shifting Situation

Each `LoopBuffer` (layer) already has:
- `StereoBlockPitchShifter blockPitchShifter` - Signalsmith Stretch instance
- Configured with `presetCheaper()` for CPU efficiency
- Currently tied to playback rate (speed changes = pitch changes)

### Implementation Options

**Option A: Upgrade existing pitch shifter quality**
- Change from `presetCheaper()` to `presetDefault()` or custom settings
- Add separate "pitch" parameter independent of playback rate
- Pro: Reuses existing infrastructure
- Con: Higher CPU per layer (8 layers × better quality = significant)

**Option B: Add second pitch shifter per layer for "effect" pitch**
- Keep `presetCheaper()` for playback rate
- Add new `StereoBlockPitchShifter` with `presetDefault()` for pitch effect
- Pro: Can have different quality for each use case
- Con: Doubles pitch shifter CPU load

**Option C: Single pitch shifter, smarter routing**
- When playback rate is 1.0x, use pitch shifter only for pitch effect (can be higher quality)
- When playback rate changes, combine both transformations
- Pro: More efficient when not using speed changes
- Con: Complex state management

### Recommended Approach: Option A with Quality Toggle

Add per-layer UI controls:
1. **PITCH knob**: -24 to +24 semitones (independent of speed)
2. **HQ toggle**: Switch between `presetCheaper()` and `presetDefault()`

### UI Changes

In the layer settings panel (where EQ lives), add:

```
┌─────────────────────────────────────────────┐
│  EQ                    PITCH                │
│  ┌───┐ ┌───┐ ┌───┐    ┌───────┐            │
│  │LOW│ │MID│ │HI │    │ PITCH │  [HQ]      │
│  └───┘ └───┘ └───┘    └───────┘            │
│  0dB   0dB   0dB       0 st                 │
└─────────────────────────────────────────────┘
```

### DSP Changes

In `LoopBuffer.h`:
- Add `pitchSemitones` parameter (float, -24 to +24)
- Add `highQualityPitch` flag
- Modify `processBlock()` to apply pitch shift independently of playback rate
- Combined pitch ratio = `speedPitchRatio * effectPitchRatio`

### CPU Impact Assessment

| Preset | Relative CPU | Quality |
|--------|-------------|---------|
| `presetCheaper()` | 1.0x | Good |
| `presetDefault()` | ~2.0x | Better |
| `presetBetter()` | ~3.5x | Best |

With 8 layers at `presetDefault()`: approximately 2x current pitch shifting CPU.

---

## Phase 2: Send Architecture Foundation

### Goal
Add per-layer send levels that control how much of each layer's signal goes to shared effect buses.

### Concept

```
Layer 0 ──┬──► Direct Out ────────────────────┐
          ├──► LOFI Send ──┐                  │
          ├──► TEXTURE Send ──┐               │
          └──► REVERB Send ──┐│               │
                             ││               │
Layer 1 ──┬──► Direct Out ───│┼───────────────┤
          ├──► LOFI Send ────┤│               │
          ├──► TEXTURE Send ─┼┤               │
          └──► REVERB Send ──┼┼┐              │
                             │││              │
...                          │││              │
                             ↓↓↓              │
                      ┌──────────────┐        │
                      │ Effect Buses │        │
                      │  • LOFI      │        │
                      │  • TEXTURE   │        │
                      │  • REVERB    │        │
                      └──────┬───────┘        │
                             │                │
                             ↓                │
                      Effect Returns ─────────┤
                                              │
                                              ↓
                                         Final Mix
```

### Per-Layer Send Parameters

For each layer (0-7), add:
- `lofiSend`: 0-100% send to LOFI bus
- `textureSend`: 0-100% send to TEXTURE bus
- `reverbSend`: 0-100% send to REVERB bus
- `delaySend`: 0-100% send to DELAY bus
- `saturationSend`: 0-100% send to SATURATION bus

### Global Effect Return Levels

Each effect bus has a return level controlling how much processed signal mixes back in:
- `lofiReturn`: 0-100%
- `textureReturn`: 0-100%
- `reverbReturn`: 0-100%
- etc.

### Implementation Steps

1. **Create Effect Bus Buffers**
   - Add `effectBusBuffer` for each effect type in `PluginProcessor`
   - Size: stereo, samplesPerBlock

2. **Modify LoopEngine Processing**
   - After each layer processes, split signal:
     - Direct out → goes to main mix
     - Send amounts → accumulate into effect bus buffers

3. **Process Effect Buses**
   - Each effect processor receives its bus buffer (sum of all layer sends)
   - Process in place
   - Mix return into final output

4. **Add Parameters**
   - Per-layer send levels (8 layers × N effects = many params)
   - Could use non-automatable state initially to reduce param count

### UI Concept for Send Levels

In layer settings panel, add send knobs:

```
┌─────────────────────────────────────────────────────┐
│  EQ                 PITCH           SENDS           │
│  ┌───┐ ┌───┐ ┌───┐  ┌───┐    ┌───┐ ┌───┐ ┌───┐    │
│  │LOW│ │MID│ │HI │  │ P │    │LOF│ │TEX│ │REV│    │
│  └───┘ └───┘ └───┘  └───┘    └───┘ └───┘ └───┘    │
│  0dB   0dB   0dB    0st      50%   0%    25%      │
└─────────────────────────────────────────────────────┘
```

Or in mixer view, add send section per channel strip.

---

## Phase 3: Selective Per-Layer Effects

### Goal
For certain lightweight effects, offer true per-layer instances rather than sends.

### Candidates for Per-Layer Instances

| Effect | Per-Layer Viable? | Notes |
|--------|------------------|-------|
| HP/LP Filter | Yes | Biquad is cheap (~100 bytes state) |
| Bit Crusher | Yes | Stateless, trivial CPU |
| Sample Rate Reduction | Yes | Minimal state |
| Saturation | Maybe | Depends on algorithm |
| Reverb | No | Too CPU heavy |
| Texture/Granular | No | ~4MB buffer per layer |
| Delay | No | Large buffers needed |

### Lightweight Per-Layer Effects

Could move HP/LP filters and bit crusher to per-layer processing:

```
Layer 0 ──► [HP/LP] ──► [BitCrush] ──► Per-layer mix ──► Sends
```

This would allow per-layer frequency shaping without the full LOFI effect stack.

---

## Phase 4: Advanced Send Routing

### Pre/Post Fader Sends

- **Pre-fader**: Send level independent of layer volume (for reverb tails that persist during fadeouts)
- **Post-fader**: Send follows layer volume (more common for most effects)

### Effect Bus Ordering

Allow users to change effect bus processing order:
```
LOFI → TEXTURE → SATURATION → REVERB → DELAY
vs
REVERB → LOFI → DELAY → TEXTURE → SATURATION
```

### Parallel vs Series Sends

- **Parallel** (current plan): Each send is independent
- **Series**: Output of one effect feeds into next

---

## Code Naming Cleanup (Housekeeping)

The internal code uses "degrade" terminology while UI shows "LOFI":

| Current Code | UI Label | Suggested Rename |
|--------------|----------|------------------|
| `DegradeProcessor` | LOFI | `LofiProcessor` |
| `degradeProcessor` | - | `lofiProcessor` |
| `degradeHP`, `degradeLP` | HP/LP | `lofiHP`, `lofiLP` |
| `degradeMasterBypass` | - | `lofiMasterBypass` |

This is low priority but would improve code clarity.

---

## Implementation Priority

1. **Phase 1**: Per-layer pitch shifting with quality option
   - Highest user value
   - Contained scope (LoopBuffer + UI changes)
   - Can ship independently

2. **Phase 2**: Basic send architecture
   - Foundation for all future per-layer effect control
   - Significant but manageable refactor
   - Enables creative possibilities

3. **Phase 3**: Selective per-layer effects
   - Nice-to-have polish
   - Low CPU overhead
   - Depends on Phase 2

4. **Phase 4**: Advanced routing
   - Power user features
   - Can be deferred indefinitely

---

## CPU Budget Considerations

Current rough estimates (relative to baseline):

| Configuration | CPU Multiplier |
|---------------|---------------|
| Current (8 layers, cheap pitch) | 1.0x |
| Phase 1 (HQ pitch all layers) | ~1.8x |
| Phase 2 (send architecture) | ~1.05x (just routing) |
| Phase 3 (per-layer HP/LP/Bit) | ~1.2x |

Total with all phases: approximately 2.0-2.5x current CPU load, which should remain acceptable on modern systems.

---

## Open Questions

1. Should per-layer pitch be automatable (APVTS param) or just UI state?
2. How many send buses are needed? Start with 3 (LOFI, TEXTURE, REVERB)?
3. Should sends be visible in mixer view, layer panel, or both?
4. Pre-fader vs post-fader: default or user choice?

---

## Next Steps

- [ ] Review this plan and confirm priorities
- [ ] Prototype Phase 1 pitch shifting in LoopBuffer
- [ ] Design UI mockup for layer settings with pitch control
- [ ] Estimate total parameter count for Phase 2 sends
