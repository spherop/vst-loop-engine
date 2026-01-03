# Loop Engine - Claude Code Guidelines

## Project Overview
Loop Engine is a VST3/AU audio plugin built with JUCE 8 and a WebView-based UI. It's a Blooper-inspired looper with pitch shifting, time manipulation, and lo-fi degradation effects.

## Build Commands
```bash
# Configure (REQUIRED when UI files change - see note below)
cmake -B build -G Ninja

# Build
cmake --build build

# Clean rebuild
rm -rf build && cmake -B build -G Ninja && cmake --build build
```

### UI File Changes Require Reconfigure (CRITICAL)
**When modifying files in `ui/` (HTML, CSS, JS), you MUST run `cmake -B build` before `cmake --build build`.**

The UI files are embedded into BinaryData via CMake's `file(GLOB_RECURSE ...)`, which only evaluates at **configure time**. Running just `cmake --build build` will NOT pick up UI file changes - the old versions will be embedded in the plugin.

**Safe pattern for UI changes:**
```bash
cmake -B build -G Ninja && cmake --build build
```

This ensures the file glob is re-evaluated and new UI files are included.

## Crucial Practices

### Version Incrementing (CRITICAL)
**ALWAYS increment the version ticker after each significant change or build.**

Location: `ui/index.html` line ~540
```html
<span class="font-mono text-[9px] text-fd-text-dim" id="version-ticker">v0.2.0</span>
```

Version format: `vMAJOR.MINOR.PATCH`
- MAJOR: Breaking changes or major feature additions
- MINOR: New features or significant improvements
- PATCH: Bug fixes, tweaks, small changes

After modifying code and rebuilding, update the version before committing.

### Code Organization
- **src/**: C++ plugin code (DSP, JUCE integration)
- **ui/**: HTML/CSS/JS for WebView interface
- **lib/**: External libraries (Signalsmith Stretch, etc.)

### Key Files
- `PluginProcessor.cpp/h`: Audio processing, parameters
- `PluginEditor.cpp/h`: WebView hosting, native function bridge
- `LoopBuffer.h`: Core loop recording/playback DSP
- `LoopEngine.h`: Multi-layer management
- `PhaseVocoder.h`: Pitch shifting (Signalsmith Stretch wrapper)
- `DegradeProcessor.h`: Lo-fi effects (filter, bitcrush, wobble, scrambler)

### Native Function Bridge
JS communicates with C++ via native functions defined in PluginEditor.cpp:
```cpp
.withNativeFunction("functionName", [this](const juce::Array<juce::var>& args, auto complete) {
    // Handle call
    complete({});
})
```

### Parameter System
Uses JUCE AudioProcessorValueTreeState (APVTS) for automatable parameters.
WebSliderParameterAttachment connects UI knobs to parameters.

## Testing
After building, the plugin is automatically copied to:
- VST3: `~/Library/Audio/Plug-Ins/VST3/Loop Engine.vst3`
- AU: `~/Library/Audio/Plug-Ins/Components/Loop Engine.component`

Reload in DAW to test changes.

## Libraries
- **Signalsmith Stretch** (MIT): High-quality pitch shifting
  - Location: `lib/signalsmith-stretch/`
  - Dependency: `lib/signalsmith-linear/` (FFT)
- **JUCE 8**: Audio plugin framework (fetched via CPM)
