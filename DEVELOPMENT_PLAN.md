# Fuzz Delay VST Plugin Development Plan

A JUCE-based VST plugin combining fuzz and analog delay effects, with a web-based (HTML/CSS/JS) user interface.

## Project Overview

**Goal**: Create a VST3/AU plugin that combines fuzz and analog delay effects, allowing users to shape delay repeats with various fuzz characters.

**Tech Stack**:
- **Audio Framework**: JUCE 8
- **UI**: HTML/CSS/JavaScript via JUCE 8's WebView
- **Build System**: CMake with CPM for dependencies
- **Targets**: VST3, AU (macOS), Standalone

---

## Phase 1: Environment Setup & Hello World Plugin

### 1.1 Install Prerequisites

**macOS Requirements**:
- Xcode (with command line tools): `xcode-select --install`
- CMake 3.22+: `brew install cmake`
- Ninja build system: `brew install ninja`
- Node.js (for web UI development): `brew install node`

**Verify installations**:
```bash
cmake --version
ninja --version
node --version
npm --version
```

### 1.2 Project Structure Setup

```
fuzz-delay-plugin/
├── CMakeLists.txt              # Main CMake config
├── cmake/
│   └── CPM.cmake               # CPM package manager
├── src/
│   ├── PluginProcessor.h       # Audio processing
│   ├── PluginProcessor.cpp
│   ├── PluginEditor.h          # WebView container
│   └── PluginEditor.cpp
├── ui/                         # Web-based UI
│   ├── package.json
│   ├── index.html
│   ├── styles.css
│   └── main.js
├── resources/                  # Bundled assets
└── DEVELOPMENT_PLAN.md
```

### 1.3 Create Base CMake Configuration

The CMakeLists.txt should:
- Use CPM to fetch JUCE 8
- Configure WebView2 for Windows compatibility
- Set up the plugin with VST3/AU/Standalone formats
- Bundle web UI assets into binary data

### 1.4 Milestone Deliverable

- [ ] Empty plugin that opens in Ableton/Logic
- [ ] Plugin passes audio through unchanged
- [ ] Basic WebView loads showing "Hello Fuzz Delay"

---

## Phase 2: Basic Analog Delay (Audio Only)

### 2.1 Core DSP Components

**Circular Delay Buffer**:
```cpp
class DelayLine {
    std::vector<float> buffer;
    size_t writeIndex = 0;
    float sampleRate = 44100.0f;

    void setMaxDelayTime(float seconds);
    void write(float sample);
    float read(float delayTimeInSamples);  // With linear interpolation
};
```

**Key Parameters**:
| Parameter | Range | Default | Description |
|-----------|-------|---------|-------------|
| Delay Time | 1ms - 2000ms | 300ms | Time between repeats |
| Feedback | 0% - 95% | 40% | Amount fed back (capped for stability) |
| Mix | 0% - 100% | 50% | Dry/Wet balance |
| Tone | 200Hz - 12kHz | 4kHz | Lowpass filter cutoff |

### 2.2 Parameter Smoothing

Critical for avoiding clicks and zipper noise:
```cpp
// Use juce::SmoothedValue for all parameters
juce::SmoothedValue<float> delayTimeSmoothed;
delayTimeSmoothed.reset(sampleRate, 0.05);  // 50ms ramp time
```

### 2.3 Analog Character

To simulate analog warmth:
- **Lowpass filter in feedback loop** (progressively darker repeats)
- **Subtle saturation** on feedback path
- **Optional: slight modulation** (0.1-0.5 Hz) on delay time for tape wobble

### 2.4 Milestone Deliverable

- [ ] Delay effect audible with all 4 parameters working
- [ ] No clicks/pops when adjusting parameters
- [ ] Stable at all sample rates (44.1k, 48k, 88.2k, 96k)
- [ ] Feedback doesn't self-oscillate uncontrollably

---

## Phase 3: WebView UI Integration

### 3.1 WebView Setup in PluginEditor

```cpp
// PluginEditor.h
class FuzzDelayEditor : public juce::AudioProcessorEditor {
    juce::WebBrowserComponent browser;

    // Parameter relays for C++ <-> JS communication
    juce::WebSliderRelay delayTimeRelay { browser, "delayTime" };
    juce::WebSliderRelay feedbackRelay { browser, "feedback" };
    juce::WebSliderRelay mixRelay { browser, "mix" };
    juce::WebSliderRelay toneRelay { browser, "tone" };
};
```

### 3.2 C++ to JavaScript Communication

**Exposing Parameters**:
```cpp
browser.getOptions()
    .withNativeIntegrationEnabled()
    .withResourceProvider([this](auto& url) {
        return getResourceForUrl(url);
    })
    .withOptionsFrom(delayTimeRelay)
    .withOptionsFrom(feedbackRelay)
    // ... other relays
```

**JavaScript Side**:
```javascript
// Get parameter state
const delayTime = Juce.getSliderState("delayTime");

// Listen for changes from C++
delayTime.valueChangedEvent.addListener(() => {
    updateKnobPosition(delayTime.getNormalisedValue());
});

// Send changes to C++
knob.addEventListener("input", () => {
    delayTime.setNormalisedValue(knob.value / knob.max);
});
```

### 3.3 Development Workflow

**Hot Reload Setup**:
1. In Debug mode, point WebView to `localhost:3000`
2. Run `npm start` in the `ui/` directory
3. Edit HTML/CSS/JS and see changes instantly
4. For Release, bundle assets via JUCE's BinaryData

### 3.4 Basic UI Design

Initial layout for delay-only phase:
```
┌─────────────────────────────────────────┐
│           FUZZ DELAY                     │
├─────────────────────────────────────────┤
│                                         │
│   ┌─────┐  ┌─────┐  ┌─────┐  ┌─────┐   │
│   │TIME │  │FDBK │  │TONE │  │ MIX │   │
│   │     │  │     │  │     │  │     │   │
│   └─────┘  └─────┘  └─────┘  └─────┘   │
│    300ms    40%      4kHz     50%      │
│                                         │
│          [ TEMPO SYNC: OFF ]            │
│                                         │
└─────────────────────────────────────────┘
```

### 3.5 Milestone Deliverable

- [ ] WebView displays and resizes properly
- [ ] All 4 delay parameters controllable from web UI
- [ ] Parameters sync between UI and DAW automation
- [ ] Hot reload working in development
- [ ] Release build bundles UI assets correctly

---

## Phase 4: Fuzz Section

### 4.1 Fuzz DSP Components

**Fuzz Types to Implement**:
1. **Classic Fuzz** - Hard clipping (germanium-style)
2. **Smooth Overdrive** - Soft clipping (tube-style)
3. **Octave Fuzz** - Full-wave rectification for octave-up

**Basic Waveshaping**:
```cpp
// Hard clip
float hardClip(float x, float threshold) {
    return std::clamp(x, -threshold, threshold);
}

// Soft clip (tanh saturation)
float softClip(float x, float drive) {
    return std::tanh(x * drive);
}
```

### 4.2 Fuzz Parameters

| Parameter | Range | Description |
|-----------|-------|-------------|
| Fuzz Amount | 0% - 100% | Drive/gain into clipper |
| Fuzz Type | Enum | Classic, Smooth, Octave |
| Fuzz Tone | 200Hz - 8kHz | Post-fuzz filter |
| Fuzz Position | Pre/Post/Feedback | Where fuzz is applied |

### 4.3 Fuzz Routing Options

This is the **key differentiator** of your plugin:

1. **Pre-Delay**: Fuzz the input before delay (standard)
2. **Post-Delay**: Fuzz the entire wet signal
3. **Feedback Only**: Fuzz only in the feedback path (repeats get progressively more distorted)
4. **Parallel**: Clean delay mixed with fuzzed delay

### 4.4 Milestone Deliverable

- [ ] 3 fuzz types implemented and selectable
- [ ] Fuzz position routing working
- [ ] UI updated with fuzz controls
- [ ] No aliasing artifacts at high fuzz settings

---

## Phase 5: Advanced Features

### 5.1 Tempo Sync

- BPM sync options: 1/4, 1/8, 1/8T, 1/16, dotted variants
- Read host tempo via `getPlayHead()->getPosition()`
- Calculate delay time: `60.0 / bpm * multiplier * 1000.0` (ms)

### 5.2 Modulation

- LFO modulating delay time for chorus/vibrato effects
- Rate: 0.1 - 10 Hz
- Depth: 0 - 20ms

### 5.3 Preset System

- Save/load presets from UI
- Factory presets demonstrating different characters
- Use JUCE's `AudioProcessorValueTreeState` for state management

### 5.4 Visual Feedback

- Animated delay visualization showing repeats
- Input/output level meters
- Waveform display of fuzz character

---

## Phase 6: Polish & Release

### 6.1 Testing

- [ ] Test in Ableton Live, Logic Pro, FL Studio, Reaper
- [ ] Test at 44.1k, 48k, 88.2k, 96k sample rates
- [ ] CPU profiling and optimization
- [ ] Memory leak testing
- [ ] Automation testing

### 6.2 Platform Builds

- [ ] macOS Universal Binary (Intel + Apple Silicon)
- [ ] Windows x64
- [ ] Linux x64 (optional)

### 6.3 Installer Creation

- macOS: PKG installer or DMG
- Windows: Inno Setup or WiX

---

## Resources & References

### Official Documentation
- [JUCE 8 WebView UIs](https://juce.com/blog/juce-8-feature-overview-webview-uis/)
- [JUCE Delay Line Tutorial](https://docs.juce.com/master/tutorial_dsp_delay_line.html)

### Example Repositories
- [juce-webview-tutorial](https://github.com/JanWilczek/juce-webview-tutorial) - WebView plugin template
- [webview_juce_plugin_choc](https://github.com/TheAudioProgrammer/webview_juce_plugin_choc) - Choc-based alternative
- [FeedbackDelay](https://github.com/chadmckell/FeedbackDelay) - Simple delay implementation

### Learning Resources
- [The Audio Programmer YouTube](https://www.youtube.com/c/TheAudioProgrammer)
- [JUCE Forum](https://forum.juce.com/)

---

## Next Steps

To begin development, we should:

1. **Set up the project skeleton** with CMake and JUCE 8
2. **Create a minimal "pass-through" plugin** that loads in a DAW
3. **Add WebView with a basic HTML page** showing connection works
4. **Implement the delay buffer** and get audio flowing

Would you like to start with Phase 1 and get the project skeleton created?
