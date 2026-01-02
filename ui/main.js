// Loop Engine Plugin UI Controller
// Implements JUCE WebView bindings without npm dependencies

// Loop Engine - Version is set in index.html

// Promise handler for native function calls
class PromiseHandler {
    constructor() {
        this.lastPromiseId = 0;
        this.promises = new Map();

        // Listen for completion events from C++
        if (typeof window.__JUCE__ !== 'undefined' && window.__JUCE__.backend) {
            window.__JUCE__.backend.addEventListener("__juce__complete", ({ promiseId, result }) => {
                if (this.promises.has(promiseId)) {
                    this.promises.get(promiseId).resolve(result);
                    this.promises.delete(promiseId);
                }
            });
        }
    }

    createPromise() {
        const promiseId = this.lastPromiseId++;
        const result = new Promise((resolve, reject) => {
            this.promises.set(promiseId, { resolve, reject });
        });
        return [promiseId, result];
    }
}

const promiseHandler = new PromiseHandler();

// Helper to get native functions
function getNativeFunction(name) {
    return function() {
        const [promiseId, result] = promiseHandler.createPromise();

        if (window.__JUCE__ && window.__JUCE__.backend) {
            console.log(`[NATIVE] Calling ${name} with args:`, Array.prototype.slice.call(arguments));
            window.__JUCE__.backend.emitEvent("__juce__invoke", {
                name: name,
                params: Array.prototype.slice.call(arguments),
                resultId: promiseId
            });
        } else {
            console.error(`[NATIVE] JUCE backend not available for ${name}!`);
        }

        return result;
    };
}

// Slider state class for parameter binding
class SliderState {
    constructor(name) {
        this.name = name;
        this.identifier = "__juce__slider" + this.name;
        this.scaledValue = 0;
        this.properties = {
            start: 0,
            end: 1,
            skew: 1,
            name: "",
            label: "",
            numSteps: 100,
            interval: 0,
            parameterIndex: -1
        };
        this.valueChangedEvent = new ListenerList();
        this.propertiesChangedEvent = new ListenerList();

        if (window.__JUCE__ && window.__JUCE__.backend) {
            window.__JUCE__.backend.addEventListener(this.identifier, (event) => this.handleEvent(event));
            window.__JUCE__.backend.emitEvent(this.identifier, { eventType: "requestInitialUpdate" });
        }
    }

    setNormalisedValue(newValue) {
        this.scaledValue = this.normalisedToScaledValue(newValue);

        if (window.__JUCE__ && window.__JUCE__.backend) {
            window.__JUCE__.backend.emitEvent(this.identifier, {
                eventType: "valueChanged",
                value: this.scaledValue
            });
        }
    }

    getNormalisedValue() {
        return Math.pow(
            (this.scaledValue - this.properties.start) / (this.properties.end - this.properties.start),
            this.properties.skew
        );
    }

    normalisedToScaledValue(normalisedValue) {
        return Math.pow(normalisedValue, 1 / this.properties.skew) *
            (this.properties.end - this.properties.start) + this.properties.start;
    }

    handleEvent(event) {
        if (event.eventType === "valueChanged") {
            this.scaledValue = event.value;
            this.valueChangedEvent.callListeners();
        }
        if (event.eventType === "propertiesChanged") {
            const { eventType, ...rest } = event;
            this.properties = rest;
            this.propertiesChangedEvent.callListeners();
        }
    }

    sliderDragStarted() {
        if (window.__JUCE__ && window.__JUCE__.backend) {
            window.__JUCE__.backend.emitEvent(this.identifier, { eventType: "sliderDragStarted" });
        }
    }

    sliderDragEnded() {
        if (window.__JUCE__ && window.__JUCE__.backend) {
            window.__JUCE__.backend.emitEvent(this.identifier, { eventType: "sliderDragEnded" });
        }
    }
}

// Simple listener list
class ListenerList {
    constructor() {
        this.listeners = new Map();
        this.listenerId = 0;
    }

    addListener(fn) {
        const newId = this.listenerId++;
        this.listeners.set(newId, fn);
        return newId;
    }

    removeListener(id) {
        this.listeners.delete(id);
    }

    callListeners() {
        for (const [, fn] of this.listeners) {
            fn();
        }
    }
}

// Slider state cache
const sliderStates = new Map();

function getSliderState(name) {
    if (!sliderStates.has(name)) {
        sliderStates.set(name, new SliderState(name));
    }
    return sliderStates.get(name);
}

// Knob UI Controller
class KnobController {
    constructor(elementId, paramName, options = {}) {
        this.element = document.getElementById(elementId);
        if (!this.element) {
            console.error(`Knob element not found: ${elementId}`);
            return;
        }

        // Support both .knob-indicator (line) and .knob-dot (dot) styles
        this.indicator = this.element.querySelector('.knob-indicator') || this.element.querySelector('.knob-dot');
        // Allow custom value display element ID via options
        const valueElementId = options.valueElementId || `${paramName}-value`;
        this.valueDisplay = document.getElementById(valueElementId);
        this.paramName = paramName;

        this.minAngle = -135;
        this.maxAngle = 135;
        this.value = 0.5;

        this.formatValue = options.formatValue || ((v) => `${Math.round(v * 100)}%`);
        this.onValueChange = options.onValueChange || null;

        // Default value to use when resetting (optional)
        this.defaultValue = options.defaultValue !== undefined ? options.defaultValue : null;

        // Auto-enable callback - called when knob is turned (for card knobs to auto-enable effect)
        this.onTurnCallback = options.onTurn || null;

        // Chromatic/stepped mode options (for pitch knob)
        // When shiftStep is set, Shift+drag will snap to discrete steps
        this.shiftStep = options.shiftStep || null;  // e.g., 1/24 for semitones
        this.stepRange = options.stepRange || { start: 0, end: 1 };  // Actual value range

        // Flag to ignore JUCE updates temporarily (used during reset)
        this.ignoreJuceUpdates = false;

        this.isDragging = false;
        this.isShiftDrag = false;  // Track if Shift is held during drag
        this.lastY = 0;
        this.justEndedDrag = false;  // Prevent click after drag

        this.setupEvents();
        this.setupJuceBinding();
    }

    setupEvents() {
        this.element.addEventListener('mousedown', (e) => {
            // Stop propagation to prevent parent (card) from receiving click
            e.stopPropagation();
            // Cmd+click (Mac) or Ctrl+click (Windows) to reset to default
            if (e.metaKey || e.ctrlKey) {
                e.preventDefault();
                this.resetToDefault();
                return;
            }
            this.startDrag(e);
        });
        document.addEventListener('mousemove', (e) => this.drag(e));
        document.addEventListener('mouseup', () => this.endDrag());

        this.element.addEventListener('touchstart', (e) => {
            e.preventDefault();
            this.startDrag(e.touches[0]);
        });
        document.addEventListener('touchmove', (e) => {
            if (this.isDragging) {
                e.preventDefault();
                this.drag(e.touches[0]);
            }
        }, { passive: false });
        document.addEventListener('touchend', () => this.endDrag());

        this.element.addEventListener('wheel', (e) => {
            e.preventDefault();
            const delta = e.deltaY > 0 ? -0.02 : 0.02;
            this.setValue(Math.max(0, Math.min(1, this.value + delta)));
            this.sendToJuce();
        }, { passive: false });

        // Double-click to reset to default
        this.element.addEventListener('dblclick', (e) => {
            e.preventDefault();
            this.resetToDefault();
        });

        // Prevent click from propagating to parent (e.g., pedal card) after drag
        this.element.addEventListener('click', (e) => {
            if (this.justEndedDrag) {
                e.stopPropagation();
            }
        });
    }

    setupJuceBinding() {
        if (typeof window.__JUCE__ !== 'undefined') {
            this.sliderState = getSliderState(this.paramName);

            this.sliderState.valueChangedEvent.addListener(() => {
                // Skip updates if we're in a reset phase
                if (this.ignoreJuceUpdates) {
                    console.log(`[KNOB] ${this.paramName}: ignoring JUCE update during reset`);
                    return;
                }
                this.setValue(this.sliderState.getNormalisedValue());
            });

            // For loop params, use defaults instead of saved values
            // This ensures fresh session starts with default loop settings
            setTimeout(() => {
                if (this.defaultValue !== null && !this.ignoreJuceUpdates) {
                    console.log(`[KNOB] ${this.paramName}: using default value ${this.defaultValue}`);
                    this.setValue(this.defaultValue);
                    this.sendToJuce();
                } else {
                    this.setValue(this.sliderState.getNormalisedValue());
                }
            }, 100);
        }
    }

    // Reset to default value and push to JUCE
    resetToDefault() {
        if (this.defaultValue !== null) {
            console.log(`[KNOB] ${this.paramName}: resetting to default ${this.defaultValue}`);
            this.ignoreJuceUpdates = true;
            this.setValue(this.defaultValue);
            this.sendToJuce();
            // Re-enable JUCE updates after a delay
            setTimeout(() => {
                this.ignoreJuceUpdates = false;
            }, 300);
        }
    }

    startDrag(e) {
        this.isDragging = true;
        this.isShiftDrag = e.shiftKey;  // Track if Shift was held at start
        this.lastY = e.clientY;
        this.element.classList.add('active');
        this.element.classList.add('adjusting');
        if (this.sliderState) {
            this.sliderState.sliderDragStarted();
        }
    }

    drag(e) {
        if (!this.isDragging) return;

        // Update shift state in case it changed mid-drag
        this.isShiftDrag = e.shiftKey;

        const deltaY = this.lastY - e.clientY;
        const sensitivity = 0.005;
        let newValue = Math.max(0, Math.min(1, this.value + deltaY * sensitivity));

        // Chromatic (stepped) mode when Shift is held and shiftStep is configured
        if (this.isShiftDrag && this.shiftStep) {
            // Snap to discrete steps
            // Convert normalized value to step count, round, then back to normalized
            const range = this.stepRange.end - this.stepRange.start;
            const stepSize = this.shiftStep / range;  // Size of one step in normalized space
            newValue = Math.round(newValue / stepSize) * stepSize;
            newValue = Math.max(0, Math.min(1, newValue));
        }

        this.setValue(newValue);
        this.sendToJuce();

        // Call onTurn callback (e.g., to auto-enable effect when card knob is turned)
        if (this.onTurnCallback) {
            this.onTurnCallback(newValue);
        }

        this.lastY = e.clientY;
    }

    endDrag() {
        if (this.isDragging) {
            this.isDragging = false;
            this.justEndedDrag = true;  // Prevent subsequent click from propagating
            this.element.classList.remove('active');
            this.element.classList.remove('adjusting');
            if (this.sliderState) {
                this.sliderState.sliderDragEnded();
            }
            // Clear the flag after a short delay (allows click event to be blocked)
            setTimeout(() => {
                this.justEndedDrag = false;
            }, 50);
        }
    }

    setValue(normalizedValue) {
        this.value = normalizedValue;
        const angle = this.minAngle + (this.maxAngle - this.minAngle) * normalizedValue;
        if (this.indicator) {
            // Different transform for dot vs line indicator styles
            if (this.indicator.classList.contains('knob-dot')) {
                // Dot style: translateX(-50%) centers dot, rotate spins around transform-origin
                this.indicator.style.transform = `translateX(-50%) rotate(${angle}deg)`;
            } else {
                // Line style: translateY(-100%) moves indicator up so bottom is at center
                // rotate() then spins around that bottom point (the knob center)
                this.indicator.style.transform = `translateY(-100%) rotate(${angle}deg)`;
            }
        }
        if (this.valueDisplay) {
            const formattedValue = this.formatValue(normalizedValue);
            // Only update textContent if formatValue returns a non-empty string
            // (allows formatValue to handle innerHTML updates directly for complex displays)
            if (formattedValue) {
                this.valueDisplay.textContent = formattedValue;
            }
        }
        // Call value change callback if provided
        if (this.onValueChange) {
            this.onValueChange(normalizedValue);
        }
    }

    sendToJuce() {
        if (this.sliderState) {
            // Debug logging for pitch parameter
            if (this.paramName === 'loopPitch') {
                const scaled = this.sliderState.normalisedToScaledValue(this.value);
                console.log(`[PITCH SEND] normalized=${this.value.toFixed(3)} -> scaled=${scaled.toFixed(2)} (props: start=${this.sliderState.properties.start}, end=${this.sliderState.properties.end})`);
            }
            this.sliderState.setNormalisedValue(this.value);
        }
    }
}

// Tab Controller with LED toggle support
class TabController {
    constructor() {
        this.tabs = document.querySelectorAll('.tab');
        this.contents = document.querySelectorAll('.tab-content');
        this.currentTab = 'looper';
        // Note: Delay and Lofi LED toggles are handled by EffectsRackController
        this.setupEvents();
    }

    setupEvents() {
        // Tab switching only - LED toggles handled by EffectsRackController
        this.tabs.forEach(tab => {
            tab.addEventListener('click', (e) => {
                // Ignore clicks on LEDs - those are handled elsewhere
                if (e.target.classList.contains('tab-led') || e.target.classList.contains('section-led')) {
                    return;
                }
                const tabName = tab.dataset.tab;
                this.switchTab(tabName);
            });
        });
    }

    switchTab(tabName) {
        this.currentTab = tabName;

        // Update tab buttons
        this.tabs.forEach(tab => {
            tab.classList.toggle('active', tab.dataset.tab === tabName);
        });

        // Update tab content
        this.contents.forEach(content => {
            const contentId = content.id.replace('-tab', '');
            if (contentId === tabName) {
                content.classList.remove('hidden');
                content.classList.add('active');
            } else {
                content.classList.add('hidden');
                content.classList.remove('active');
            }
        });

        // When switching back to looper tab, resize the waveform canvas
        // This fixes the issue where the canvas becomes blank after visiting other tabs
        if (tabName === 'looper' && looperController) {
            // Small delay to let the DOM update first
            requestAnimationFrame(() => {
                looperController.resizeCanvas();
                // Also redraw the waveform if we have data
                if (looperController.lastWaveformData || looperController.lastLayerWaveforms) {
                    looperController.drawWaveform(
                        looperController.lastWaveformData,
                        looperController.lastLayerWaveforms,
                        looperController.lastLayerMutes
                    );
                } else {
                    looperController.drawEmptyWaveform();
                }
            });
        }
    }
}

// Looper Controller
class LooperController {
    constructor() {
        // Transport state
        this.state = 'idle'; // idle, recording, playing, overdubbing
        this.currentLayer = 1;
        this.highestLayer = 0;
        this.loopLength = 0;
        this.playheadPosition = 0;
        this.recordingStartTime = 0;
        this.isReversed = false;
        this.reverseButtonPending = false; // Flag to prevent polling from overwriting during button click

        // Zoom state
        this.zoomLevel = 1.0;
        this.zoomOffset = 0; // For panning when zoomed
        this.minZoom = 1.0;
        this.maxZoom = 8.0;

        // Loop region values (0-1 normalized) - global defaults
        this.loopStart = 0;
        this.loopEnd = 1;

        // Per-layer loop bounds (0 = global, 1-8 = layer-specific)
        this.selectedLayerForHandles = 0;  // 0 = global mode, 1-8 = per-layer
        this.layerLoopBounds = [];
        for (let i = 0; i < 8; i++) {
            this.layerLoopBounds.push({ start: 0, end: 1 });
        }

        // Per-layer colors for waveform visualization
        // 8 distinct colors: base blue, then variations for each layer
        this.layerColors = [
            '#4fc3f7',  // Layer 1: Cyan/Light blue (base)
            '#ff7043',  // Layer 2: Deep orange
            '#66bb6a',  // Layer 3: Green
            '#ab47bc',  // Layer 4: Purple
            '#ffa726',  // Layer 5: Orange
            '#26c6da',  // Layer 6: Teal
            '#ec407a',  // Layer 7: Pink
            '#9ccc65',  // Layer 8: Light green
        ];
        this.accentColor = '#4fc3f7';
        this.accentColorDim = '#29b6f6';

        // Native functions
        this.recordFn = getNativeFunction("loopRecord");
        this.playFn = getNativeFunction("loopPlay");
        this.stopFn = getNativeFunction("loopStop");
        this.overdubFn = getNativeFunction("loopOverdub");
        this.undoFn = getNativeFunction("loopUndo");
        this.redoFn = getNativeFunction("loopRedo");
        this.clearFn = getNativeFunction("loopClear");
        this.setAdditiveModeEnabledFn = getNativeFunction("setAdditiveModeEnabled");
        this.canAddLayerFn = getNativeFunction("canAddLayer");
        this.isAdditiveModeEnabledFn = getNativeFunction("isAdditiveModeEnabled");
        this.isAdditiveRecordingActiveFn = getNativeFunction("isAdditiveRecordingActive");
        this.clearLayerFn = getNativeFunction("clearLayer");
        this.deleteLayerFn = getNativeFunction("deleteLayer");
        this.getStateFn = getNativeFunction("getLoopState");
        this.getLayerContentFn = getNativeFunction("getLayerContentStates");
        this.jumpToLayerFn = getNativeFunction("loopJumpToLayer");
        this.resetParamsFn = getNativeFunction("resetLoopParams");
        this.setInputMutedFn = getNativeFunction("setInputMuted");

        // Layer mode state (Track mode = false, Layer mode = true)
        this.layerModeEnabled = false;
        this.setLayerModeFn = getNativeFunction("setLayerMode");

        // Input monitoring state
        this.inputMuted = false;

        // Track layer content states
        this.layerContentStates = [false, false, false, false, false, false, false, false];
        // Track which layers are override (ADD+) layers
        this.layerOverrideStates = [false, false, false, false, false, false, false, false];

        // Waveform crossfade animation state
        // Store smoothed waveform amplitudes for smooth visual transitions
        // Instead of jumping to new values, we smoothly interpolate toward target
        this.smoothedLayerWaveforms = null;
        this.waveformSmoothingFactor = 0.15; // How fast to move toward target (0-1, lower = smoother)

        // Layer spread for vertical spacing in waveform display (0-1)
        this.layerSpread = 0;

        // Selected layer for volume/pan editing (1-indexed, default to layer 1)
        this.selectedLayer = 1;

        // Per-layer volume/pan native functions
        this.setLayerVolumeFn = getNativeFunction("setLayerVolume");
        this.getLayerVolumeFn = getNativeFunction("getLayerVolume");
        this.setLayerPanFn = getNativeFunction("setLayerPan");
        this.getLayerPanFn = getNativeFunction("getLayerPan");

        // Per-layer loop bounds native functions
        this.setLayerLoopStartFn = getNativeFunction("setLayerLoopStart");
        this.setLayerLoopEndFn = getNativeFunction("setLayerLoopEnd");
        this.getLayerLoopBoundsFn = getNativeFunction("getLayerLoopBounds");

        // Loop length setting (bars + beats)
        this.loopLengthBars = 0;
        this.loopLengthBeats = 0;
        this.setLoopLengthBarsFn = getNativeFunction("setLoopLengthBars");
        this.setLoopLengthBeatsFn = getNativeFunction("setLoopLengthBeats");

        this.setupTransport();
        this.setupInputMonitor();
        this.setupLoopLengthSelector();
        this.setupLayers();
        this.setupModeToggle();
        this.setupWaveform();
        this.setupZoomControls();
        this.setupRecordingOverlay();
        this.setupLayerSpread();
        this.startStatePolling();

        // Sync UI with current C++ state on open
        // Delay to ensure JUCE bindings are ready
        setTimeout(() => {
            this.syncUIWithBackend();
        }, 500);
    }

    async syncUIWithBackend() {
        console.log('%c[SYNC] syncUIWithBackend called - forcing reset of loop params', 'color: #ff6b35; font-weight: bold;');
        try {
            // ALWAYS reset loop params to defaults on UI open
            console.log('[SYNC] Calling resetParamsFn...');
            const result = await this.resetParamsFn();
            console.log('[SYNC] resetParamsFn completed, result:', result);

            // Reset local UI state
            this.loopStart = 0;
            this.loopEnd = 1;
            this.updateLoopRegionShade();

            // Use the knob's resetToDefault method which handles JUCE sync properly
            // Note: loopStart/loopEnd knobs removed - now using drag handles
            if (loopSpeedKnob) {
                loopSpeedKnob.resetToDefault();
                console.log('[SYNC] Reset loopSpeed knob to default');
            }
            if (loopPitchKnob) {
                loopPitchKnob.resetToDefault();
                console.log('[SYNC] Reset loopPitch knob to default');
            }
            if (loopFadeKnob) {
                loopFadeKnob.resetToDefault();
                console.log('[SYNC] Reset loopFade knob to default');
            }

            // Reset reverse button state via native function
            console.log('[SYNC] Resetting reverse state...');
            const setReverseFn = getNativeFunction("setLoopReverse");
            await setReverseFn(false);
            this.setReversed(false);

            // Fetch initial layer content states
            console.log('[SYNC] Fetching layer content states...');
            await this.updateLayerContentStates();

            console.log('%c[SYNC] UI state reset complete', 'color: #4caf50; font-weight: bold;');
        } catch (e) {
            console.error('[SYNC] Error syncing UI with backend:', e);
        }
    }

    setupTransport() {
        this.recBtn = document.getElementById('rec-btn');
        this.recLabel = this.recBtn ? this.recBtn.querySelector('.transport-label') : null;
        this.playBtn = document.getElementById('loop-play-btn');
        this.stopBtn = document.getElementById('loop-stop-btn');
        this.undoBtn = document.getElementById('undo-btn');
        this.redoBtn = document.getElementById('redo-btn');
        this.clearBtn = document.getElementById('clear-btn');
        this.addBtn = document.getElementById('add-btn');
        this.timeDisplay = document.getElementById('loop-time-display');

        if (this.recBtn) {
            this.recBtn.addEventListener('click', () => this.record());
        }
        if (this.playBtn) {
            this.playBtn.addEventListener('click', () => this.play());
        }
        if (this.stopBtn) {
            this.stopBtn.addEventListener('click', () => this.stop());
        }
        if (this.undoBtn) {
            this.undoBtn.addEventListener('click', () => this.undo());
        }
        if (this.redoBtn) {
            this.redoBtn.addEventListener('click', () => this.redo());
        }
        if (this.clearBtn) {
            this.clearBtn.addEventListener('click', () => this.clear());
        }

        // ADD+ button - MODE TOGGLE for additive recording (Blooper-style)
        // When ADD+ mode is ON, DUB creates override layers instead of normal layers
        // Track mode state locally
        this.additiveModeEnabled = false;
        this.additiveRecordingActive = false;

        if (this.addBtn) {
            this.addBtn.addEventListener('click', async () => {
                await this.toggleAdditiveMode();
            });
        }
    }

    // Toggle ADD+ mode on/off (mode toggle, not recording toggle)
    async toggleAdditiveMode() {
        try {
            // Toggle the mode
            this.additiveModeEnabled = !this.additiveModeEnabled;
            await this.setAdditiveModeEnabledFn(this.additiveModeEnabled);

            if (this.additiveModeEnabled) {
                this.addBtn.classList.add('active');
                console.log('[LOOPER] ADD+ mode ENABLED - DUB will create override layers');
            } else {
                this.addBtn.classList.remove('active');
                console.log('[LOOPER] ADD+ mode DISABLED - DUB will create normal layers');
            }
        } catch (e) {
            console.error('Error toggling ADD+ mode:', e);
        }
    }

    // Setup Track/Layer mode toggle
    setupModeToggle() {
        this.modeToggleBtn = document.getElementById('mode-toggle-btn');
        this.modeIcon = document.getElementById('mode-icon');
        this.modeLabel = document.getElementById('mode-label');

        if (this.modeToggleBtn) {
            this.modeToggleBtn.addEventListener('click', async () => {
                await this.toggleLayerMode();
            });
        }
    }

    async toggleLayerMode() {
        try {
            this.layerModeEnabled = !this.layerModeEnabled;
            await this.setLayerModeFn(this.layerModeEnabled);
            this.updateModeToggleUI();
            console.log(`[LOOPER] Layer mode ${this.layerModeEnabled ? 'ENABLED (Layer)' : 'DISABLED (Track)'}`);
        } catch (e) {
            console.error('Error toggling layer mode:', e);
        }
    }

    updateModeToggleUI() {
        if (this.modeIcon) {
            this.modeIcon.textContent = this.layerModeEnabled ? 'L' : 'T';
        }
        if (this.modeLabel) {
            this.modeLabel.textContent = this.layerModeEnabled ? 'LAYER' : 'TRACK';
        }
        if (this.modeToggleBtn) {
            this.modeToggleBtn.classList.toggle('layer-mode', this.layerModeEnabled);
        }
    }

    setupInputMonitor() {
        this.inputMuteBtn = document.getElementById('input-mute-btn');
        this.inputMeterBarL = document.getElementById('input-meter-bar-l');
        this.inputMeterBarR = document.getElementById('input-meter-bar-r');
        this.inputMonitor = document.querySelector('.input-monitor-v');

        if (this.inputMuteBtn) {
            this.inputMuteBtn.addEventListener('click', () => this.toggleInputMute());
        }
    }

    async toggleInputMute() {
        this.inputMuted = !this.inputMuted;
        console.log(`[INPUT] Input ${this.inputMuted ? 'muted' : 'unmuted'}`);

        // Update UI
        if (this.inputMuteBtn) {
            this.inputMuteBtn.classList.toggle('muted', this.inputMuted);
        }
        if (this.inputMonitor) {
            this.inputMonitor.classList.toggle('muted', this.inputMuted);
        }

        // Send to backend
        try {
            await this.setInputMutedFn(this.inputMuted);
        } catch (e) {
            console.error('Error setting input mute:', e);
        }
    }

    updateInputMeter(levelL, levelR) {
        if (this.inputMeterBarL) {
            // Convert to dB-like scale (more visible at lower levels)
            // Use height % for vertical meter
            const heightL = Math.pow(Math.min(levelL, 1.0), 0.5) * 100;
            this.inputMeterBarL.style.height = `${heightL}%`;

            // Add clipping indicator
            if (levelL >= 0.95) {
                this.inputMeterBarL.classList.add('clipping');
                setTimeout(() => this.inputMeterBarL.classList.remove('clipping'), 100);
            }
        }
        if (this.inputMeterBarR) {
            const heightR = Math.pow(Math.min(levelR, 1.0), 0.5) * 100;
            this.inputMeterBarR.style.height = `${heightR}%`;

            if (levelR >= 0.95) {
                this.inputMeterBarR.classList.add('clipping');
                setTimeout(() => this.inputMeterBarR.classList.remove('clipping'), 100);
            }
        }
    }

    // Clear a specific layer (1-indexed)
    async clearLayer(layer) {
        console.log(`[LOOPER] Clearing layer ${layer}`);
        try {
            await this.clearLayerFn(layer);
            // Refresh layer content states after clear
            this.updateLayerContentStates();
        } catch (e) {
            console.error('Error clearing layer:', e);
        }
    }

    // Delete a specific layer and shuffle subsequent layers down (1-indexed)
    async deleteLayer(layer) {
        console.log(`[LOOPER] Deleting layer ${layer} and shuffling`);
        try {
            await this.deleteLayerFn(layer);
            // Refresh layer content states after delete
            this.updateLayerContentStates();
        } catch (e) {
            console.error('Error deleting layer:', e);
        }
    }

    setupLoopLengthSelector() {
        this.barsSelect = document.getElementById('bars-select');
        this.beatsSelect = document.getElementById('beats-select');

        // Restore saved loop length settings from localStorage
        const savedBars = localStorage.getItem('loopEngine.loopLengthBars');
        const savedBeats = localStorage.getItem('loopEngine.loopLengthBeats');

        if (savedBars !== null && this.barsSelect) {
            const bars = parseInt(savedBars);
            this.barsSelect.value = bars;
            this.loopLengthBars = bars;
            // Apply to backend after a short delay to ensure JUCE is ready
            setTimeout(() => this.setLoopLengthBars(bars), 100);
            console.log(`[LOOPER] Restored loop length bars: ${bars}`);
        }

        if (savedBeats !== null && this.beatsSelect) {
            const beats = parseInt(savedBeats);
            this.beatsSelect.value = beats;
            this.loopLengthBeats = beats;
            setTimeout(() => this.setLoopLengthBeats(beats), 150);
            console.log(`[LOOPER] Restored loop length beats: ${beats}`);
        }

        if (this.barsSelect) {
            this.barsSelect.addEventListener('change', () => {
                const bars = parseInt(this.barsSelect.value);
                this.setLoopLengthBars(bars);
            });
        }

        if (this.beatsSelect) {
            this.beatsSelect.addEventListener('change', () => {
                const beats = parseInt(this.beatsSelect.value);
                this.setLoopLengthBeats(beats);
            });
        }
    }

    async setLoopLengthBars(bars) {
        this.loopLengthBars = bars;
        const totalBeats = (bars * 4) + this.loopLengthBeats;
        console.log(`[LOOPER] Setting loop length: ${bars} bars + ${this.loopLengthBeats} beats = ${totalBeats} total beats`);

        // Persist to localStorage
        localStorage.setItem('loopEngine.loopLengthBars', bars.toString());

        try {
            await this.setLoopLengthBarsFn(bars);
        } catch (e) {
            console.error('Error setting loop length bars:', e);
        }
    }

    async setLoopLengthBeats(beats) {
        this.loopLengthBeats = beats;
        const totalBeats = (this.loopLengthBars * 4) + beats;
        console.log(`[LOOPER] Setting loop length: ${this.loopLengthBars} bars + ${beats} beats = ${totalBeats} total beats`);

        // Persist to localStorage
        localStorage.setItem('loopEngine.loopLengthBeats', beats.toString());

        try {
            await this.setLoopLengthBeatsFn(beats);
        } catch (e) {
            console.error('Error setting loop length beats:', e);
        }
    }

    setupLayers() {
        this.layerBtns = document.querySelectorAll('.layer-btn');
        this.setLayerMutedFn = getNativeFunction("setLayerMuted");
        this.flattenLayersFn = getNativeFunction("flattenLayers");

        // Flatten button
        this.flattenBtn = document.getElementById('flatten-btn');
        if (this.flattenBtn) {
            this.flattenBtn.addEventListener('click', () => this.flatten());
        }

        this.layerBtns.forEach(btn => {
            // Left click: jump to layer
            // Shift+click: delete layer and shuffle
            // CMD+click: solo this layer
            // Option+click: toggle mute
            btn.addEventListener('click', async (e) => {
                const layer = parseInt(btn.dataset.layer);
                if (e.shiftKey) {
                    // Shift+click: delete this layer and shuffle subsequent layers down
                    this.deleteLayer(layer);
                } else if (e.metaKey) {
                    // CMD+click: solo this layer
                    this.soloLayer(layer);
                } else if (e.altKey) {
                    // Option+click: toggle mute
                    const isMuted = btn.classList.contains('muted');
                    const newMuted = !isMuted;
                    btn.classList.toggle('muted', newMuted);
                    try {
                        await this.setLayerMutedFn(layer, newMuted);
                        console.log(`Layer ${layer} muted: ${newMuted}`);
                        this.updateSoloIndicators();
                    } catch (err) {
                        console.error('Error toggling layer mute:', err);
                    }
                } else {
                    // Normal click: select layer for editing (volume/pan)
                    this.selectLayer(layer);
                }
            });

            // Right click: also toggle mute (alternative to Option+click)
            btn.addEventListener('contextmenu', async (e) => {
                e.preventDefault();
                const layer = parseInt(btn.dataset.layer);
                const isMuted = btn.classList.contains('muted');
                const newMuted = !isMuted;
                btn.classList.toggle('muted', newMuted);
                try {
                    await this.setLayerMutedFn(layer, newMuted);
                    console.log(`Layer ${layer} muted: ${newMuted}`);
                    this.updateSoloIndicators();
                } catch (err) {
                    console.error('Error toggling layer mute:', err);
                }
            });
        });
    }

    // Solo a layer: mute all other layers with content, unmute the target layer
    async soloLayer(targetLayer) {
        console.log(`[LOOPER] Soloing layer ${targetLayer}`);

        // Check if this layer is already soloed (all others muted, this one unmuted)
        const targetBtn = document.querySelector(`.layer-btn[data-layer="${targetLayer}"]`);
        const isTargetMuted = targetBtn?.classList.contains('muted');

        // Count how many other layers with content are muted
        let othersMutedCount = 0;
        let othersWithContentCount = 0;

        this.layerBtns.forEach(btn => {
            const layer = parseInt(btn.dataset.layer);
            if (layer !== targetLayer && this.layerContentStates[layer - 1]) {
                othersWithContentCount++;
                if (btn.classList.contains('muted')) {
                    othersMutedCount++;
                }
            }
        });

        // If target is unmuted and all others are muted, unsolo (unmute all)
        const isCurrentlySoloed = !isTargetMuted && othersMutedCount === othersWithContentCount && othersWithContentCount > 0;

        try {
            for (const btn of this.layerBtns) {
                const layer = parseInt(btn.dataset.layer);

                if (isCurrentlySoloed) {
                    // Unsolo: unmute all layers
                    btn.classList.remove('muted');
                    await this.setLayerMutedFn(layer, false);
                } else {
                    // Solo: mute all except target
                    if (layer === targetLayer) {
                        btn.classList.remove('muted');
                        await this.setLayerMutedFn(layer, false);
                    } else {
                        btn.classList.add('muted');
                        await this.setLayerMutedFn(layer, true);
                    }
                }
            }
            console.log(`Layer ${targetLayer} ${isCurrentlySoloed ? 'unsolo' : 'solo'} complete`);

            // Update solo indicator on soloed layer
            this.updateSoloIndicators();
        } catch (err) {
            console.error('Error soloing layer:', err);
        }
    }

    // Update solo indicator on layers
    updateSoloIndicators() {
        // A layer is "soloed" if it's unmuted and all other layers with content are muted
        this.layerBtns.forEach(btn => {
            const layer = parseInt(btn.dataset.layer);
            const hasContent = this.layerContentStates[layer - 1];
            const isMuted = btn.classList.contains('muted');

            if (!hasContent) {
                btn.classList.remove('soloed');
                return;
            }

            // Count how many other layers with content are muted
            let othersMutedCount = 0;
            let othersWithContentCount = 0;

            this.layerBtns.forEach(otherBtn => {
                const otherLayer = parseInt(otherBtn.dataset.layer);
                if (otherLayer !== layer && this.layerContentStates[otherLayer - 1]) {
                    othersWithContentCount++;
                    if (otherBtn.classList.contains('muted')) {
                        othersMutedCount++;
                    }
                }
            });

            // This layer is soloed if: it's not muted, has content, and all others with content are muted
            const isSoloed = !isMuted && othersWithContentCount > 0 && othersMutedCount === othersWithContentCount;
            btn.classList.toggle('soloed', isSoloed);
        });
    }

    // Flatten all non-muted layers into layer 1
    async flatten() {
        console.log('[LOOPER] Flattening layers');
        try {
            await this.flattenLayersFn();
            // Refresh layer content states after flatten
            await this.updateLayerContentStates();
            this.updateSoloIndicators();
        } catch (e) {
            console.error('Error flattening layers:', e);
        }
    }

    setupWaveform() {
        this.waveformCanvas = document.getElementById('waveform-canvas');
        this.waveformContainer = document.getElementById('waveform-container');
        this.playhead = document.getElementById('playhead');
        this.loopStartHandle = document.getElementById('loop-start-handle');
        this.loopEndHandle = document.getElementById('loop-end-handle');
        this.loopRegionShade = document.getElementById('loop-region-shade');

        if (this.waveformCanvas) {
            this.ctx = this.waveformCanvas.getContext('2d');
            this.resizeCanvas();
            window.addEventListener('resize', () => this.resizeCanvas());
            this.drawEmptyWaveform();
        }

        // Initialize loop region shade
        this.updateLoopRegionShade();

        // Setup handle dragging
        this.setupHandleDragging();
    }

    setupHandleDragging() {
        this.draggingHandle = null;
        this.setLoopStartFn = getNativeFunction("setLoopStart");
        this.setLoopEndFn = getNativeFunction("setLoopEnd");

        // Start handle drag
        if (this.loopStartHandle) {
            this.loopStartHandle.addEventListener('mousedown', (e) => {
                e.stopPropagation();
                this.draggingHandle = 'start';
                document.body.style.cursor = 'ew-resize';
            });
        }

        if (this.loopEndHandle) {
            this.loopEndHandle.addEventListener('mousedown', (e) => {
                e.stopPropagation();
                this.draggingHandle = 'end';
                document.body.style.cursor = 'ew-resize';
            });
        }

        // Loop region drag (move both start and end together)
        this.loopRegion = document.getElementById('loop-region');
        if (this.loopRegion) {
            this.loopRegion.addEventListener('mousedown', (e) => {
                // Only drag if clicking on the region itself, not the handles
                if (e.target === this.loopRegion) {
                    e.stopPropagation();
                    // Get current bounds based on mode
                    const targetLayer = this.selectedLayerForHandles;
                    let currentStart, currentEnd;
                    if (targetLayer === 0) {
                        currentStart = this.loopStart;
                        currentEnd = this.loopEnd;
                    } else {
                        const bounds = this.layerLoopBounds[targetLayer - 1];
                        currentStart = bounds.start;
                        currentEnd = bounds.end;
                    }
                    const regionWidth = currentEnd - currentStart;
                    // Only allow dragging if region is smaller than full width
                    if (regionWidth < 0.99) {
                        this.draggingHandle = 'region';
                        this.regionDragWidth = regionWidth;
                        const rect = this.waveformContainer.getBoundingClientRect();
                        this.regionDragStartX = (e.clientX - rect.left) / rect.width;
                        this.regionDragStartLoopStart = currentStart;
                        document.body.style.cursor = 'grab';
                    }
                }
            });
        }

        // Handle drag movement
        document.addEventListener('mousemove', (e) => {
            if (!this.draggingHandle || !this.waveformContainer) return;

            const rect = this.waveformContainer.getBoundingClientRect();
            let normalizedPos = (e.clientX - rect.left) / rect.width;
            normalizedPos = Math.max(0, Math.min(1, normalizedPos));

            // Get current bounds based on whether we're in global or per-layer mode
            const targetLayer = this.selectedLayerForHandles;
            let currentStart, currentEnd;
            if (targetLayer === 0) {
                currentStart = this.loopStart;
                currentEnd = this.loopEnd;
            } else {
                const bounds = this.layerLoopBounds[targetLayer - 1];
                currentStart = bounds.start;
                currentEnd = bounds.end;
            }

            if (this.draggingHandle === 'start') {
                // Start can't go past end - 1%
                const maxStart = currentEnd - 0.01;
                normalizedPos = Math.min(normalizedPos, maxStart);
                this.applyLoopStart(normalizedPos, targetLayer);
            } else if (this.draggingHandle === 'end') {
                // End can't go before start + 1%
                const minEnd = currentStart + 0.01;
                normalizedPos = Math.max(normalizedPos, minEnd);
                this.applyLoopEnd(normalizedPos, targetLayer);
            } else if (this.draggingHandle === 'region') {
                // Move the entire region while maintaining width
                document.body.style.cursor = 'grabbing';
                const delta = normalizedPos - this.regionDragStartX;
                let newStart = this.regionDragStartLoopStart + delta;
                let newEnd = newStart + this.regionDragWidth;

                // Clamp to valid range
                if (newStart < 0) {
                    newStart = 0;
                    newEnd = this.regionDragWidth;
                }
                if (newEnd > 1) {
                    newEnd = 1;
                    newStart = 1 - this.regionDragWidth;
                }

                this.applyLoopStart(newStart, targetLayer);
                this.applyLoopEnd(newEnd, targetLayer);
            }
        });

        // End drag
        document.addEventListener('mouseup', () => {
            if (this.draggingHandle) {
                this.draggingHandle = null;
                this.regionDragWidth = null;
                this.regionDragStartX = null;
                this.regionDragStartLoopStart = null;
                document.body.style.cursor = '';
            }
        });

        // Also support clicking on waveform to set playhead (future feature)
    }

    async sendLoopStartToJuce(value) {
        try {
            // Get the slider state and update it
            const sliderState = getSliderState('loopStart');
            if (sliderState) {
                sliderState.setNormalisedValue(value);
            }
        } catch (e) {
            console.error('Error setting loop start:', e);
        }
    }

    async sendLoopEndToJuce(value) {
        try {
            // Get the slider state and update it
            const sliderState = getSliderState('loopEnd');
            if (sliderState) {
                sliderState.setNormalisedValue(value);
            }
        } catch (e) {
            console.error('Error setting loop end:', e);
        }
    }

    // Apply loop start to either global (layer=0) or specific layer (1-8)
    applyLoopStart(value, targetLayer) {
        if (targetLayer === 0) {
            // Global mode - apply to all layers via parameter
            this.loopStart = value;
            this.sendLoopStartToJuce(value);
        } else {
            // Per-layer mode - apply to specific layer
            this.layerLoopBounds[targetLayer - 1].start = value;
            this.sendLayerLoopStartToJuce(targetLayer, value);
        }
        this.updateLoopRegionShade();
    }

    // Apply loop end to either global (layer=0) or specific layer (1-8)
    applyLoopEnd(value, targetLayer) {
        if (targetLayer === 0) {
            // Global mode - apply to all layers via parameter
            this.loopEnd = value;
            this.sendLoopEndToJuce(value);
        } else {
            // Per-layer mode - apply to specific layer
            this.layerLoopBounds[targetLayer - 1].end = value;
            this.sendLayerLoopEndToJuce(targetLayer, value);
        }
        this.updateLoopRegionShade();
    }

    // Send per-layer loop start to C++ backend
    async sendLayerLoopStartToJuce(layer, value) {
        try {
            if (this.setLayerLoopStartFn) {
                await this.setLayerLoopStartFn(layer, value);
            }
        } catch (e) {
            console.error('Error setting layer loop start:', e);
        }
    }

    // Send per-layer loop end to C++ backend
    async sendLayerLoopEndToJuce(layer, value) {
        try {
            if (this.setLayerLoopEndFn) {
                await this.setLayerLoopEndFn(layer, value);
            }
        } catch (e) {
            console.error('Error setting layer loop end:', e);
        }
    }

    // Load per-layer loop bounds from C++ backend
    async loadLayerLoopBounds(layer) {
        try {
            if (this.getLayerLoopBoundsFn) {
                const bounds = await this.getLayerLoopBoundsFn(layer);
                if (bounds) {
                    this.layerLoopBounds[layer - 1] = {
                        start: bounds.start || 0,
                        end: bounds.end || 1
                    };
                    this.updateLoopRegionShade();
                    console.log(`[Loop] Loaded layer ${layer} bounds:`, bounds);
                }
            }
        } catch (e) {
            console.error('Error loading layer loop bounds:', e);
        }
    }

    // Update loop region color based on selected layer
    updateLoopRegionColor(layer) {
        if (!this.loopRegion) return;

        if (layer === 0) {
            // Global mode - use default accent color
            this.loopRegion.style.setProperty('--loop-region-color', '#4fc3f7');
            this.loopRegionShade?.style.setProperty('--loop-region-color', '#4fc3f7');
        } else {
            // Use layer-specific color
            const color = this.layerColors[layer - 1];
            this.loopRegion.style.setProperty('--loop-region-color', color);
            this.loopRegionShade?.style.setProperty('--loop-region-color', color);
        }
    }

    setupZoomControls() {
        this.zoomInBtn = document.getElementById('zoom-in-btn');
        this.zoomOutBtn = document.getElementById('zoom-out-btn');
        this.zoomFitBtn = document.getElementById('zoom-fit-btn');
        this.zoomLevelDisplay = document.getElementById('zoom-level');

        if (this.zoomInBtn) {
            this.zoomInBtn.addEventListener('click', () => this.zoomIn());
        }
        if (this.zoomOutBtn) {
            this.zoomOutBtn.addEventListener('click', () => this.zoomOut());
        }
        if (this.zoomFitBtn) {
            this.zoomFitBtn.addEventListener('click', () => this.zoomFit());
        }

        this.updateZoomUI();
    }

    setupRecordingOverlay() {
        this.recordingOverlay = document.getElementById('recording-overlay');
        this.recTimeDisplay = document.getElementById('rec-time');
        this.inputLevelBar = document.getElementById('input-level-bar');
    }

    setupLayerSpread() {
        const slider = document.getElementById('layer-spread-slider');
        if (slider) {
            slider.addEventListener('input', (e) => {
                this.layerSpread = parseInt(e.target.value) / 100;
                // Redraw waveform with new spread if we have cached data
                if (this.lastWaveformData || this.lastLayerWaveforms) {
                    this.drawWaveform(
                        this.lastWaveformData,
                        this.lastLayerWaveforms,
                        this.lastLayerMutes
                    );
                }
            });
        }
    }

    // Select a layer for editing - shows white outline
    selectLayer(layer) {
        // Toggle selection behavior for loop handles
        if (this.selectedLayerForHandles === layer) {
            // Same layer clicked again - deselect (return to global)
            this.selectedLayerForHandles = 0;
            this.selectedLayer = 0;
            this.updateLoopRegionShade();
            this.updateLoopRegionColor(0);
            // Hide the layer panel
            if (window.layerPanelController) {
                window.layerPanelController.hidePanel();
            }
            // Remove selection highlight from all buttons
            this.layerBtns.forEach(btn => {
                btn.classList.remove('selected');
            });
            console.log('[Loop] Deselected layer, returning to global handles');
        } else {
            // New layer selected
            this.selectedLayerForHandles = layer;
            this.selectedLayer = layer;
            // Load that layer's loop bounds and update display
            this.loadLayerLoopBounds(layer);
            this.updateLoopRegionColor(layer);
            // Show layer panel
            if (window.layerPanelController) {
                window.layerPanelController.showPanel(layer);
            }
            // Update visual selection on layer buttons
            this.layerBtns.forEach(btn => {
                const btnLayer = parseInt(btn.dataset.layer);
                btn.classList.toggle('selected', btnLayer === layer);
            });
            console.log(`[Loop] Selected layer ${layer} for handles`);
        }
    }

    // Zoom methods
    zoomIn() {
        this.zoomLevel = Math.min(this.maxZoom, this.zoomLevel * 1.5);
        this.updateZoomUI();
    }

    zoomOut() {
        this.zoomLevel = Math.max(this.minZoom, this.zoomLevel / 1.5);
        if (this.zoomLevel <= 1.0) {
            this.zoomOffset = 0;
        }
        this.updateZoomUI();
    }

    zoomFit() {
        this.zoomLevel = 1.0;
        this.zoomOffset = 0;
        this.updateZoomUI();
    }

    updateZoomUI() {
        if (this.zoomLevelDisplay) {
            this.zoomLevelDisplay.textContent = `${this.zoomLevel.toFixed(1)}x`;
        }
        if (this.zoomOutBtn) {
            this.zoomOutBtn.disabled = this.zoomLevel <= this.minZoom;
        }
        if (this.zoomInBtn) {
            this.zoomInBtn.disabled = this.zoomLevel >= this.maxZoom;
        }
    }

    // Loop region visualization
    updateLoopRegionShade() {
        // Get the appropriate loop bounds based on mode
        const targetLayer = this.selectedLayerForHandles;
        let start, end;
        if (targetLayer === 0) {
            // Global mode
            start = this.loopStart;
            end = this.loopEnd;
        } else {
            // Per-layer mode
            const bounds = this.layerLoopBounds[targetLayer - 1];
            start = bounds.start;
            end = bounds.end;
        }

        if (this.loopRegionShade) {
            this.loopRegionShade.style.setProperty('--loop-start', `${start * 100}%`);
            this.loopRegionShade.style.setProperty('--loop-end', `${end * 100}%`);
        }

        // Update the draggable loop region position via CSS variables
        // The handles are positioned relative to the loop-region edges via CSS
        if (this.loopRegion) {
            this.loopRegion.style.setProperty('--loop-start', `${start * 100}%`);
            this.loopRegion.style.setProperty('--loop-end', `${end * 100}%`);

            // Add/remove full-width class to control cursor
            const regionWidth = end - start;
            if (regionWidth >= 0.99) {
                this.loopRegion.classList.add('full-width');
            } else {
                this.loopRegion.classList.remove('full-width');
            }
        }
    }

    setLoopStart(value) {
        this.loopStart = value;
        this.updateLoopRegionShade();
    }

    setLoopEnd(value) {
        this.loopEnd = value;
        this.updateLoopRegionShade();
    }

    // Recording overlay methods
    showRecordingOverlay() {
        if (this.recordingOverlay) {
            this.recordingOverlay.classList.remove('hidden');
            this.recordingStartTime = Date.now();
        }
    }

    hideRecordingOverlay() {
        if (this.recordingOverlay) {
            this.recordingOverlay.classList.add('hidden');
        }
    }

    updateRecordingTime() {
        if (this.recTimeDisplay && this.state === 'recording') {
            const elapsed = (Date.now() - this.recordingStartTime) / 1000;
            this.recTimeDisplay.textContent = `${elapsed.toFixed(1)}s`;
        }
    }

    updateInputLevel(level) {
        if (this.inputLevelBar) {
            // level should be 0-1
            const percent = Math.min(100, level * 100);
            this.inputLevelBar.style.width = `${percent}%`;
        }
    }

    resizeCanvas() {
        if (this.waveformCanvas) {
            const rect = this.waveformCanvas.parentElement.getBoundingClientRect();
            this.waveformCanvas.width = rect.width;
            this.waveformCanvas.height = rect.height;
        }
    }

    drawEmptyWaveform() {
        if (!this.ctx) return;
        const { width, height } = this.waveformCanvas;

        this.ctx.fillStyle = '#0a0a0a';
        this.ctx.fillRect(0, 0, width, height);

        // Draw center line
        this.ctx.strokeStyle = '#1a1a1a';
        this.ctx.lineWidth = 1;
        this.ctx.beginPath();
        this.ctx.moveTo(0, height / 2);
        this.ctx.lineTo(width, height / 2);
        this.ctx.stroke();

        // Draw "No loop recorded" text
        this.ctx.fillStyle = '#333';
        this.ctx.font = '11px "JetBrains Mono", monospace';
        this.ctx.textAlign = 'center';
        this.ctx.fillText('No loop recorded', width / 2, height / 2 + 4);
    }

    drawWaveform(waveformData, layerWaveforms, layerMutes) {
        // Store the last waveform data for redrawing after tab switches
        this.lastWaveformData = waveformData;
        this.lastLayerWaveforms = layerWaveforms;
        this.lastLayerMutes = layerMutes;

        if (!this.ctx) {
            return;
        }

        // Check if we have any content to draw
        const hasLayerData = layerWaveforms && layerWaveforms.length > 0;
        const hasBasicData = waveformData && waveformData.length > 0;

        if (!hasLayerData && !hasBasicData) {
            this.drawEmptyWaveform();
            return;
        }

        const { width, height } = this.waveformCanvas;
        const centerY = height / 2;

        this.ctx.fillStyle = '#0a0a0a';
        this.ctx.fillRect(0, 0, width, height);

        // Calculate zoom transform
        const zoomedWidth = width * this.zoomLevel;
        const offsetX = this.zoomOffset * (zoomedWidth - width);

        // Draw per-layer waveforms if available (colored, stacked)
        if (hasLayerData) {
            // Debug: log once when we have multiple layers
            if (layerWaveforms.length > 1 && !this._loggedLayerColors) {
                console.log(`[DRAW] Drawing ${layerWaveforms.length} layers with colors:`, this.layerColors.slice(0, layerWaveforms.length));
                this._loggedLayerColors = true;
            }

            // Waveform smoothing: instead of jumping to new values, smoothly interpolate
            // This creates a beautiful crossfade effect when fade multiplier changes
            let displayWaveforms = layerWaveforms;

            // Initialize smoothed waveforms if needed
            if (!this.smoothedLayerWaveforms || this.smoothedLayerWaveforms.length !== layerWaveforms.length) {
                // First time or layer count changed - start with current values
                this.smoothedLayerWaveforms = layerWaveforms.map(layer => layer ? [...layer] : []);
            } else {
                // Smoothly interpolate each layer's waveform toward target
                displayWaveforms = [];
                const smoothing = this.waveformSmoothingFactor;

                for (let layerIdx = 0; layerIdx < layerWaveforms.length; layerIdx++) {
                    const targetData = layerWaveforms[layerIdx] || [];
                    const smoothedData = this.smoothedLayerWaveforms[layerIdx] || [];
                    const resultData = [];

                    // Ensure smoothed array is same length as target
                    const len = targetData.length;

                    for (let i = 0; i < len; i++) {
                        const target = targetData[i] || 0;
                        const current = (i < smoothedData.length) ? smoothedData[i] : target;
                        // Exponential smoothing: move fraction of distance toward target each frame
                        const smoothed = current + (target - current) * smoothing;
                        resultData.push(smoothed);
                    }

                    displayWaveforms.push(resultData);
                    // Update stored smoothed values for next frame
                    this.smoothedLayerWaveforms[layerIdx] = resultData;
                }
            }

            // Count active (non-empty) layers for spread calculation
            const activeLayers = displayWaveforms.filter(d => d && d.length > 0).length;

            // When a layer is selected for handle editing, only show that layer's waveform
            // This makes it easier to see individual layer loop bounds
            const selectedLayer = this.selectedLayerForHandles;  // 0 = global/all, 1-8 = specific layer

            for (let layerIdx = 0; layerIdx < displayWaveforms.length; layerIdx++) {
                const layerData = displayWaveforms[layerIdx];
                if (!layerData || layerData.length === 0) continue;

                // If a layer is selected, only draw that layer (skip others)
                if (selectedLayer > 0 && (layerIdx + 1) !== selectedLayer) {
                    continue;
                }

                // Check if this layer is muted (undone) - draw as outline if so
                const isMuted = layerMutes && layerMutes[layerIdx];
                const layerColor = this.layerColors[layerIdx % this.layerColors.length];

                // Calculate per-layer vertical offset based on spread
                // When spread is 0, all layers are centered (layerCenterY = centerY)
                // When spread is 1, layers are evenly distributed vertically with padding
                let layerCenterY = centerY;
                let layerSpacing = height;  // Default: full height available for single layer

                // Reserve small space for waveform amplitude at top and bottom
                const padding = height * 0.05;  // 5% padding on each side (just enough to prevent clipping)
                const usableHeight = height - (padding * 2);

                if (this.layerSpread > 0 && activeLayers > 1) {
                    // At full spread, distribute layers evenly within usable area
                    // Each layer gets equal vertical space
                    const slotHeight = usableHeight / activeLayers;
                    layerSpacing = slotHeight;

                    // Find this layer's position among active layers
                    let activeIdx = 0;
                    for (let i = 0; i < layerIdx; i++) {
                        if (displayWaveforms[i] && displayWaveforms[i].length > 0) activeIdx++;
                    }

                    // Position: padding + half slot + (activeIdx * slotHeight)
                    // Interpolate between centered (spread=0) and distributed (spread=1)
                    const distributedY = padding + slotHeight / 2 + (activeIdx * slotHeight);
                    layerCenterY = centerY * (1 - this.layerSpread) + distributedY * this.layerSpread;
                }

                const barWidth = zoomedWidth / layerData.length;

                // Reduce amplitude when spread to prevent overlapping
                // At 0% spread: use 35% of height (centered, overlapping)
                // At 100% spread: scale to fit within the allocated slot
                let amplitudeScale = 0.35;
                if (this.layerSpread > 0 && activeLayers > 1) {
                    // At full spread, each waveform should fit within 80% of its slot
                    const slotHeight = usableHeight / activeLayers;
                    const maxAmplitudeAtFullSpread = (slotHeight * 0.4) / height;  // 40% of slot on each side
                    amplitudeScale = 0.35 * (1 - this.layerSpread) + maxAmplitudeAtFullSpread * this.layerSpread;
                }

                if (isMuted) {
                    // Muted/undone layers: draw as colored outline (ghost effect)
                    this.ctx.strokeStyle = layerColor + '60';  // 40% opacity outline
                    this.ctx.lineWidth = 1;
                    this.ctx.beginPath();

                    // Draw outline path for the waveform shape
                    let started = false;
                    for (let i = 0; i < layerData.length; i++) {
                        const amplitude = layerData[i] * (height * amplitudeScale);
                        const x = (i * barWidth) - offsetX;
                        if (x + barWidth > 0 && x < width && amplitude > 0.5) {
                            if (!started) {
                                this.ctx.moveTo(x, layerCenterY - amplitude);
                                started = true;
                            } else {
                                this.ctx.lineTo(x, layerCenterY - amplitude);
                            }
                        }
                    }
                    // Draw back along the bottom
                    for (let i = layerData.length - 1; i >= 0; i--) {
                        const amplitude = layerData[i] * (height * amplitudeScale);
                        const x = (i * barWidth) - offsetX;
                        if (x + barWidth > 0 && x < width && amplitude > 0.5) {
                            this.ctx.lineTo(x, layerCenterY + amplitude);
                        }
                    }
                    this.ctx.closePath();
                    this.ctx.stroke();
                } else {
                    // Active layers: draw as filled bars
                    this.ctx.fillStyle = layerColor + '99';  // 60% opacity for active

                    for (let i = 0; i < layerData.length; i++) {
                        const amplitude = layerData[i] * (height * amplitudeScale);
                        const x = (i * barWidth) - offsetX;
                        // Only draw visible bars
                        if (x + barWidth > 0 && x < width && amplitude > 0.5) {
                            this.ctx.fillRect(x, layerCenterY - amplitude, Math.max(1, barWidth - 1), amplitude * 2);
                        }
                    }
                }
            }
        } else if (hasBasicData) {
            // Fallback to single-color combined waveform
            this.ctx.fillStyle = this.accentColor;
            const barWidth = zoomedWidth / waveformData.length;

            for (let i = 0; i < waveformData.length; i++) {
                const amplitude = waveformData[i] * (height * 0.4);
                const x = (i * barWidth) - offsetX;
                if (x + barWidth > 0 && x < width) {
                    this.ctx.fillRect(x, centerY - amplitude, Math.max(1, barWidth - 1), amplitude * 2);
                }
            }
        }

        // Draw loop region boundaries as vertical lines
        const startX = (this.loopStart * zoomedWidth) - offsetX;
        const endX = (this.loopEnd * zoomedWidth) - offsetX;

        this.ctx.strokeStyle = this.accentColor;
        this.ctx.lineWidth = 2;
        this.ctx.setLineDash([4, 4]);

        if (startX >= 0 && startX <= width) {
            this.ctx.beginPath();
            this.ctx.moveTo(startX, 0);
            this.ctx.lineTo(startX, height);
            this.ctx.stroke();
        }

        if (endX >= 0 && endX <= width) {
            this.ctx.beginPath();
            this.ctx.moveTo(endX, 0);
            this.ctx.lineTo(endX, height);
            this.ctx.stroke();
        }

        this.ctx.setLineDash([]);
    }

    updatePlayhead(position) {
        if (this.playhead && this.waveformCanvas) {
            // Get the appropriate loop bounds based on whether a layer is selected
            const targetLayer = this.selectedLayerForHandles;
            let loopStart, loopEnd;

            if (targetLayer === 0) {
                // Global mode - use global bounds
                loopStart = this.loopStart;
                loopEnd = this.loopEnd;
            } else {
                // Per-layer mode - use that layer's bounds
                const bounds = this.layerLoopBounds[targetLayer - 1];
                loopStart = bounds.start;
                loopEnd = bounds.end;
            }

            // Position is 0-1 within the layer's effective loop region
            // The backend already calculates this relative to the layer's loopStart/loopEnd
            // So position 0 = at layer's loop start, position 1 = at layer's loop end
            //
            // We need to map this to the visual position:
            // - The visual loop region goes from loopStart to loopEnd (in normalized 0-1 space)
            // - So absolutePosition = loopStart + position * (loopEnd - loopStart)
            const loopRegionWidth = loopEnd - loopStart;
            const absolutePosition = loopStart + (position * loopRegionWidth);

            // Apply zoom
            const zoomedWidth = this.waveformCanvas.width * this.zoomLevel;
            const offsetX = this.zoomOffset * (zoomedWidth - this.waveformCanvas.width);
            const x = (absolutePosition * zoomedWidth) - offsetX;

            this.playhead.style.left = `${x}px`;

            // Hide playhead if outside visible area
            if (x < 0 || x > this.waveformCanvas.width) {
                this.playhead.style.opacity = '0';
            } else {
                this.playhead.style.opacity = '1';
            }
        }
    }

    setReversed(reversed) {
        console.log(`[REV] setReversed(${reversed}) called`);
        this.isReversed = reversed;
        // Update playhead color/style to indicate direction
        if (this.playhead) {
            this.playhead.classList.toggle('reversed', reversed);
        }
        // Update button state with BOTH class and inline styles for debugging
        const reverseBtn = document.getElementById('reverse-btn');
        if (reverseBtn) {
            reverseBtn.classList.toggle('active', reversed);
            // Also apply inline styles to force visual change
            if (reversed) {
                reverseBtn.style.background = 'rgba(79, 195, 247, 0.3)';
                reverseBtn.style.borderColor = '#4fc3f7';
                reverseBtn.style.color = '#81d4fa';
                reverseBtn.style.boxShadow = '0 0 12px rgba(79, 195, 247, 0.4)';
            } else {
                reverseBtn.style.background = '';
                reverseBtn.style.borderColor = '';
                reverseBtn.style.color = '';
                reverseBtn.style.boxShadow = '';
            }
            console.log(`[REV] Button active class: ${reverseBtn.classList.contains('active')}, inline styles applied: ${reversed}`);
        } else {
            console.error('[REV] Reverse button element not found');
        }
    }

    setupReverseButton() {
        const reverseBtn = document.getElementById('reverse-btn');
        if (!reverseBtn) {
            console.error('Reverse button not found');
            return;
        }

        console.log('[REV] Setting up reverse button handler');
        const setReverseFn = getNativeFunction("setLoopReverse");

        reverseBtn.addEventListener('click', async () => {
            const newReversed = !this.isReversed;
            console.log(`[REV] Button clicked, toggling from ${this.isReversed} to ${newReversed}`);

            // Set pending flag to prevent polling from overwriting
            this.reverseButtonPending = true;
            this.setReversed(newReversed);

            try {
                console.log(`[REV] Calling native setLoopReverse(${newReversed})`);
                await setReverseFn(newReversed);
                console.log('[REV] Native function call completed');
            } catch (e) {
                console.error('[REV] Error setting reverse:', e);
                // Revert UI state on error
                this.setReversed(!newReversed);
            } finally {
                // Clear pending flag after a short delay to ensure C++ state has propagated
                setTimeout(() => {
                    this.reverseButtonPending = false;
                    console.log('[REV] Pending flag cleared');
                }, 200);
            }
        });
    }

    updateTimeDisplay(currentTime, totalTime) {
        if (this.timeDisplay) {
            this.timeDisplay.textContent = `${currentTime.toFixed(1)}s / ${totalTime.toFixed(1)}s`;
        }
    }

    async record() {
        try {
            // Check if any layer has content (layer 1 = index 0)
            const hasContent = this.layerContentStates && this.layerContentStates[0] === true;

            // When idle with existing content, use overdub (starts playback + records)
            // When idle with no content, use record (starts fresh recording)
            if (this.state === 'idle' && hasContent) {
                console.log('[LOOPER] DUB from idle - calling overdub to start playback + record');
                await this.overdubFn();
                this.updateTransportUI('overdubbing');
            } else {
                await this.recordFn();
                this.updateTransportUI('recording');
            }
        } catch (e) {
            console.error('Error starting record:', e);
        }
    }

    async play() {
        try {
            await this.playFn();
            this.updateTransportUI('playing');
        } catch (e) {
            console.error('Error starting playback:', e);
        }
    }

    async stop() {
        try {
            await this.stopFn();
            this.updateTransportUI('idle');

            // Stop micro looper when main looper stops
            // This syncs the micro looper state with the main transport
            const microLooperStopFn = getNativeFunction('microLooperStop');
            if (microLooperStopFn) {
                await microLooperStopFn();
                console.log('[LOOPER] Stopped micro looper with main transport');
            }
        } catch (e) {
            console.error('Error stopping:', e);
        }
    }

    async overdub() {
        try {
            await this.overdubFn();
            this.updateTransportUI('overdubbing');
        } catch (e) {
            console.error('Error starting overdub:', e);
        }
    }

    async undo() {
        try {
            await this.undoFn();
        } catch (e) {
            console.error('Error undoing:', e);
        }
    }

    async redo() {
        try {
            await this.redoFn();
        } catch (e) {
            console.error('Error redoing:', e);
        }
    }

    async clear() {
        try {
            await this.clearFn();
            this.drawEmptyWaveform();
            this.updateTransportUI('idle');
            this.updateLayerUI(1, 0);

            // Reset loop start/end to defaults (global)
            this.loopStart = 0;
            this.loopEnd = 1;

            // Reset per-layer loop bounds
            this.selectedLayerForHandles = 0;
            for (let i = 0; i < 8; i++) {
                this.layerLoopBounds[i] = { start: 0, end: 1 };
            }
            this.updateLoopRegionShade();
            this.updateLoopRegionColor(0);

            // Remove selection highlight from layer buttons
            this.layerBtns.forEach(btn => {
                btn.classList.remove('selected');
            });

            // Also reset layer content states
            this.layerContentStates = [false, false, false, false, false, false, false, false];

            // CRITICAL: Reset waveform smoothing buffers to prevent memory accumulation
            // These buffers grow with each recording and must be cleared on reset
            this.smoothedLayerWaveforms = null;
            this.lastWaveformData = null;
            this.lastLayerWaveforms = null;
            this.lastLayerMutes = null;
            this._loggedLayerColors = false;

            // Reset mixer visuals and state
            if (window.mixerController) {
                window.mixerController.resetAll();
            }

            // Hide layer panel if open
            if (window.layerPanelController) {
                window.layerPanelController.hidePanel();
            }
        } catch (e) {
            console.error('Error clearing:', e);
        }
    }

    async jumpToLayer(layer) {
        try {
            await this.jumpToLayerFn(layer);
        } catch (e) {
            console.error('Error jumping to layer:', e);
        }
    }

    updateTransportUI(state) {
        const previousState = this.state;
        this.state = state;

        // Reset all buttons
        [this.recBtn, this.playBtn].forEach(btn => {
            if (btn) btn.classList.remove('active');
        });

        // Reset REC button styling (clear any layer color and special modes)
        if (this.recBtn) {
            this.recBtn.style.borderColor = '';
            this.recBtn.style.boxShadow = '';
            this.recBtn.classList.remove('dub-plus-mode', 'dub-ready');
        }
        if (this.recLabel) {
            this.recLabel.style.color = '';
        }

        // Update REC button label and state based on current state
        // Blooper-style: REC button shows "REC" normally, "DUB" when overdubbing
        // Shows "DUB+" when clicking will add a new layer, colored with next layer's color
        switch (state) {
            case 'recording':
                if (this.recBtn) this.recBtn.classList.add('active');
                if (this.recLabel) this.recLabel.textContent = 'REC';
                break;
            case 'playing':
                if (this.playBtn) this.playBtn.classList.add('active');
                // Mark REC button as "ready to dub" (not actively recording) - gray styling via CSS
                if (this.recBtn) this.recBtn.classList.add('dub-ready');
                if (this.recLabel) this.recLabel.textContent = 'DUB';
                // Check if clicking DUB will add a new layer (current layer has content)
                // The + is shown via CSS pseudo-element on the icon, not in the label
                const willAddLayer = this.layerContentStates[this.currentLayer - 1];
                if (willAddLayer && this.highestLayer > 0) {
                    if (this.recBtn) this.recBtn.classList.add('dub-plus-mode');
                } else {
                    if (this.recBtn) this.recBtn.classList.remove('dub-plus-mode');
                }
                break;
            case 'overdubbing':
                if (this.recBtn) this.recBtn.classList.add('active');
                if (this.recLabel) this.recLabel.textContent = 'DUB';
                // During overdubbing, show + in icon if we can add more layers
                const willAddLayerAfterOverdub = this.highestLayer < 7;
                // Color matches the layer currently being recorded to (currentLayer)
                {
                    const currentLayerColor = this.layerColors[this.currentLayer];  // currentLayer is 1-indexed
                    if (this.recBtn) {
                        this.recBtn.style.borderColor = currentLayerColor;
                        this.recBtn.style.boxShadow = `0 0 8px ${currentLayerColor}60`;
                        if (willAddLayerAfterOverdub) {
                            this.recBtn.classList.add('dub-plus-mode');
                        } else {
                            this.recBtn.classList.remove('dub-plus-mode');
                        }
                    }
                    if (this.recLabel) {
                        this.recLabel.style.color = currentLayerColor;
                    }
                }
                break;
            case 'idle':
            default:
                // When idle with recorded content, show DUB (clicking will start playback + overdub)
                // When idle with no content, show REC (clicking will start fresh recording)
                const hasContentIdle = this.layerContentStates && this.layerContentStates[0] === true;
                if (hasContentIdle) {
                    if (this.recLabel) this.recLabel.textContent = 'DUB';
                    // Color to match the next layer that will be recorded to
                    // highestLayer is 1-indexed, so highestLayer=1 means layer 0 has content, next is layer 1 (index 1)
                    const nextLayerIdx = Math.min(this.highestLayer, 7);
                    const nextLayerColor = this.layerColors[nextLayerIdx];
                    if (this.recBtn) {
                        this.recBtn.style.borderColor = nextLayerColor;
                        this.recBtn.style.boxShadow = `0 0 8px ${nextLayerColor}60`;
                    }
                    if (this.recLabel) {
                        this.recLabel.style.color = nextLayerColor;
                    }
                } else {
                    if (this.recLabel) this.recLabel.textContent = 'REC';
                }
                break;
        }

        // Handle recording overlay
        if (state === 'recording' && previousState !== 'recording') {
            this.showRecordingOverlay();
        } else if (state !== 'recording' && previousState === 'recording') {
            this.hideRecordingOverlay();
        }
    }

    updateLayerUI(currentLayer, highestLayer, layerOverrides = null) {
        this.currentLayer = currentLayer;
        this.highestLayer = highestLayer;
        if (layerOverrides) {
            this.layerOverrideStates = layerOverrides;
        }

        this.layerBtns.forEach(async btn => {
            const layer = parseInt(btn.dataset.layer);
            const idx = layer - 1;  // 0-indexed
            btn.classList.remove('active', 'has-content', 'override-layer');

            // Reset inline styles
            btn.style.background = '';
            btn.style.borderColor = '';
            btn.style.color = '';
            btn.style.boxShadow = '';

            if (layer === currentLayer) {
                btn.classList.add('active');
            }

            // Check if this is an override (ADD+) layer
            const isOverride = this.layerOverrideStates && this.layerOverrideStates[idx];

            // Use actual content state instead of just highestLayer
            if (this.layerContentStates[idx]) {
                btn.classList.add('has-content');

                // Override layers get special styling - orange/amber color
                if (isOverride) {
                    btn.classList.add('override-layer');
                    btn.style.background = '#ff9800';  // Orange for ADD+ layers
                    btn.style.borderColor = '#ffb74d';
                    btn.style.color = '#0a0a0a';
                } else {
                    // Apply the layer-specific color from layerColors array
                    const layerColor = this.layerColors[idx % this.layerColors.length];
                    btn.style.background = layerColor;
                    btn.style.borderColor = layerColor;
                    btn.style.color = '#0a0a0a';
                }

                // Fetch and set volume/pan CSS variables for visual feedback
                try {
                    const vol = await this.getLayerVolumeFn(layer);
                    const pan = await this.getLayerPanFn(layer);
                    btn.style.setProperty('--layer-volume', vol);
                    btn.style.setProperty('--layer-pan', pan);
                } catch (e) {
                    // Default to full volume, center pan if fetch fails
                    btn.style.setProperty('--layer-volume', '1');
                    btn.style.setProperty('--layer-pan', '0');
                }

                // Add glow if this is also the active layer
                if (layer === currentLayer) {
                    const glowColor = isOverride ? '#ff9800' : this.layerColors[idx % this.layerColors.length];
                    btn.style.boxShadow = `0 0 12px ${glowColor}80`;  // 50% opacity glow
                }
            }
        });
    }

    // Update layer content states from backend
    async updateLayerContentStates() {
        try {
            const states = await this.getLayerContentFn();
            if (states && Array.isArray(states)) {
                this.layerContentStates = states;
                // Update UI with current states
                this.updateLayerUI(this.currentLayer, this.highestLayer);
            }
        } catch (e) {
            console.error('Error getting layer content states:', e);
        }
    }

    // Sync layer mute button UI with backend state
    // Also distinguishes between undone layers (from undo) and explicitly muted layers
    syncLayerMuteUI(muteStates, currentLayer, highestLayer) {
        this.layerBtns.forEach(btn => {
            const layer = parseInt(btn.dataset.layer);
            const idx = layer - 1;  // 0-indexed
            if (idx >= 0 && idx < muteStates.length) {
                const isMuted = muteStates[idx];
                // An "undone" layer is one that is muted AND is above currentLayer
                // (layers that were muted via undo rather than explicit user mute)
                const isUndone = isMuted && layer > currentLayer && layer <= highestLayer;
                btn.classList.toggle('muted', isMuted && !isUndone);  // Explicit mute
                btn.classList.toggle('undone', isUndone);  // Undo state
            } else {
                btn.classList.remove('muted', 'undone');
            }
        });
        // Update solo indicators based on mute states
        this.updateSoloIndicators();
    }

    startStatePolling() {
        // Poll loop state every 50ms for smooth playhead updates
        setInterval(async () => {
            try {
                const state = await this.getStateFn();
                if (state) {
                    // Store layer playhead positions for per-layer display
                    if (state.layerPlayheads) {
                        this.layerPlayheads = state.layerPlayheads;
                    }

                    // Update playhead position - use layer-specific if a layer is selected
                    if (typeof state.playhead !== 'undefined') {
                        let playheadPos = state.playhead;

                        // If a layer is selected, use that layer's playhead position
                        const targetLayer = this.selectedLayerForHandles;
                        if (targetLayer > 0 && this.layerPlayheads && this.layerPlayheads[targetLayer - 1] !== undefined) {
                            playheadPos = this.layerPlayheads[targetLayer - 1];
                        }

                        this.updatePlayhead(playheadPos);
                    }

                    // Update time display
                    if (typeof state.loopLength !== 'undefined') {
                        const currentTime = (state.playhead || 0) * state.loopLength;
                        this.updateTimeDisplay(currentTime, state.loopLength);
                    }

                    // Update transport state
                    if (typeof state.state !== 'undefined') {
                        const stateNames = ['idle', 'recording', 'playing', 'overdubbing'];
                        const stateName = stateNames[state.state] || 'idle';
                        if (stateName !== this.state) {
                            this.updateTransportUI(stateName);
                        }
                    }

                    // Update recording time if recording
                    if (this.state === 'recording') {
                        this.updateRecordingTime();
                    }

                    // Update input level meter (always, not just when recording)
                    if (typeof state.inputLevelL !== 'undefined' && typeof state.inputLevelR !== 'undefined') {
                        this.updateInputMeter(state.inputLevelL, state.inputLevelR);
                    }

                    // Update layer UI and content states
                    if (typeof state.layer !== 'undefined' && typeof state.highestLayer !== 'undefined') {
                        if (state.layer !== this.currentLayer || state.highestLayer !== this.highestLayer) {
                            // Fetch actual layer content states when layer changes
                            this.updateLayerContentStates();
                        }
                        // Pass override states for ADD+ layer styling
                        this.updateLayerUI(state.layer, state.highestLayer, state.layerOverrides);
                    }

                    // Sync ADD+ mode state from backend
                    if (typeof state.additiveModeEnabled !== 'undefined') {
                        if (state.additiveModeEnabled !== this.additiveModeEnabled) {
                            this.additiveModeEnabled = state.additiveModeEnabled;
                            if (this.addBtn) {
                                if (this.additiveModeEnabled) {
                                    this.addBtn.classList.add('active');
                                } else {
                                    this.addBtn.classList.remove('active');
                                }
                            }
                        }
                    }
                    // Track capture state for potential visual feedback
                    if (typeof state.additiveRecordingActive !== 'undefined') {
                        this.additiveRecordingActive = state.additiveRecordingActive;
                    }

                    // Sync layer mode state from backend
                    if (typeof state.layerModeEnabled !== 'undefined' && state.layerModeEnabled !== this.layerModeEnabled) {
                        this.layerModeEnabled = state.layerModeEnabled;
                        this.updateModeToggleUI();
                    }

                    // Update waveform if provided (with per-layer colors if available)
                    if (state.layerWaveforms || state.waveform) {
                        // Debug: log layer waveform data
                        if (state.layerWaveforms && state.layerWaveforms.length > 1) {
                            console.log(`[WAVEFORM] ${state.layerWaveforms.length} layers, mutes:`, state.layerMutes);
                        }
                        this.drawWaveform(
                            state.waveform,
                            state.layerWaveforms,
                            state.layerMutes
                        );
                    }

                    // Sync layer mute UI states from backend
                    if (state.layerMutes && state.layerMutes.length > 0) {
                        this.syncLayerMuteUI(state.layerMutes, state.layer || 1, state.highestLayer || 1);
                    }
                }
            } catch (e) {
                // Silently ignore polling errors
            }
        }, 50);
    }
}

// Store looper controller globally so knobs can access it
let looperController = null;

// Store loop knob controllers for direct reset
// loopStartKnob and loopEndKnob removed - now using drag handles on waveform
let loopSpeedKnob = null;
let loopPitchKnob = null;
let loopFadeKnob = null;

// Host Transport Sync Controller
class HostSyncController {
    constructor() {
        this.btn = document.getElementById('host-sync-btn');
        this.led = document.getElementById('host-sync-led');
        this.divisionSelect = document.getElementById('note-value-select');
        this.isEnabled = false;
        this.isHostPlaying = false;
        this.setHostSyncFn = getNativeFunction("setHostTransportSync");

        this.setupEvents();
        this.fetchInitialState();
        this.startPolling();
    }

    async fetchInitialState() {
        try {
            const getStateFn = getNativeFunction("getTempoState");
            const state = await getStateFn();
            if (state) {
                if (typeof state.hostTransportSync !== 'undefined') {
                    this.isEnabled = state.hostTransportSync;
                    this.updateUI();
                }
                if (typeof state.hostPlaying !== 'undefined') {
                    this.isHostPlaying = state.hostPlaying;
                    this.updateUI();
                }
            }
        } catch (e) {
            console.log('Could not fetch initial host sync state');
        }
    }

    setupEvents() {
        // Click on button toggles
        if (this.btn) {
            this.btn.addEventListener('click', () => this.toggle());
        }
    }

    async toggle() {
        this.isEnabled = !this.isEnabled;
        this.updateUI();

        try {
            await this.setHostSyncFn(this.isEnabled);
            console.log(`[HOST SYNC] ${this.isEnabled ? 'Enabled' : 'Disabled'}`);
        } catch (e) {
            console.error('Error toggling host sync:', e);
            this.isEnabled = !this.isEnabled;
            this.updateUI();
        }
    }

    updateUI() {
        // Update button state
        if (this.btn) {
            this.btn.classList.toggle('active', this.isEnabled);
            this.btn.classList.toggle('playing', this.isEnabled && this.isHostPlaying);
        }
        // Enable/disable division dropdown based on HOST sync state
        if (this.divisionSelect) {
            this.divisionSelect.disabled = !this.isEnabled;
        }
    }

    // Poll for host playing state
    startPolling() {
        setInterval(async () => {
            try {
                const getStateFn = getNativeFunction("getTempoState");
                const state = await getStateFn();
                if (state && typeof state.hostPlaying !== 'undefined') {
                    if (state.hostPlaying !== this.isHostPlaying) {
                        this.isHostPlaying = state.hostPlaying;
                        this.updateUI();
                    }
                }
            } catch (e) {
                // Silently ignore
            }
        }, 200);
    }
}

// BPM Display and Note Value Controller
class BpmDisplayController {
    constructor() {
        this.bpmEl = document.getElementById('bpm-display');
        this.noteSelect = document.getElementById('note-value-select');

        this.setNoteFn = getNativeFunction("setTempoNote");

        this.setupEvents();
        this.fetchInitialState();
    }

    setupEvents() {
        if (this.noteSelect) {
            this.noteSelect.addEventListener('change', (e) => this.setNoteValue(parseInt(e.target.value)));
        }
    }

    async fetchInitialState() {
        try {
            const getStateFn = getNativeFunction("getTempoState");
            const state = await getStateFn();
            if (state) {
                if (typeof state.noteValue !== 'undefined' && this.noteSelect) {
                    this.noteSelect.value = state.noteValue.toString();
                }
                if (typeof state.bpm !== 'undefined') {
                    this.updateBpm(state.bpm);
                }
            }
        } catch (e) {
            console.log('Could not fetch initial tempo state');
        }
    }

    updateBpm(bpm) {
        if (this.bpmEl) {
            this.bpmEl.textContent = bpm.toFixed(1);
        }
    }

    async setNoteValue(noteIndex) {
        try {
            await this.setNoteFn(noteIndex);
        } catch (e) {
            console.error('Error setting note value:', e);
        }
    }
}

// MicroLooper controller (MOOD-inspired always-listening micro-looper)
class MicroLooperController {
    constructor() {
        // Transport buttons
        this.playBtn = document.getElementById('micro-play-btn');
        this.overdubBtn = document.getElementById('micro-overdub-btn');
        this.freezeBtn = document.getElementById('micro-freeze-btn');
        this.clearBtn = document.getElementById('micro-clear-btn');
        this.reverseBtn = document.getElementById('micro-reverse-btn');

        // Mode buttons
        this.modeEnvBtn = document.getElementById('micro-mode-env');
        this.modeTapeBtn = document.getElementById('micro-mode-tape');
        this.modeStretchBtn = document.getElementById('micro-mode-stretch');

        // Waveform visualization elements
        this.waveformCanvas = document.getElementById('micro-waveform-canvas');
        this.waveformCtx = this.waveformCanvas?.getContext('2d');
        this.playhead = document.getElementById('micro-playhead');
        this.bufferFill = document.getElementById('micro-buffer-fill');
        this.modeDesc = document.getElementById('micro-mode-desc');

        // Mode descriptions
        this.modeDescriptions = {
            0: 'ENV: Volume-ducked playback, speed controls pitch',
            1: 'TAPE: Direct speed control with wow flutter',
            2: 'STRETCH: Time stretch independent of pitch'
        };

        // State
        this.isPlaying = false;
        this.isOverdubbing = false;
        this.isFrozen = false;
        this.isReversed = false;
        this.currentMode = 1; // 0=ENV, 1=TAPE, 2=STRETCH (default to TAPE)
        this.waveformData = [];
        this.playPosition = 0;
        this.bufferFillAmount = 0;

        // Scale buttons
        this.scaleButtons = document.querySelectorAll('.micro-scale-btn');
        this.currentScale = 0; // 0=FREE, 1=CHROMATIC, 2=MAJOR, 3=MINOR, 4=PENTATONIC, 5=OCTAVES

        // Native functions
        this.playFn = getNativeFunction('microLooperPlay');
        this.overdubFn = getNativeFunction('microLooperOverdub');
        this.freezeFn = getNativeFunction('microLooperFreeze');
        this.clearFn = getNativeFunction('microLooperClear');
        this.setModeFn = getNativeFunction('setMicroLooperMode');
        this.setReverseFn = getNativeFunction('setMicroLooperReverse');
        this.getStateFn = getNativeFunction('getMicroLooperState');
        this.getWaveformFn = getNativeFunction('getMicroLooperWaveform');
        this.setScaleFn = getNativeFunction('setMicroLooperScale');

        // Setup canvas
        this.setupCanvas();
        window.addEventListener('resize', () => this.setupCanvas());

        this.setupEvents();
        this.fetchInitialState();
        this.startPolling();
        this.updateModeDescription();
        console.log('[MICROLOOP] Controller initialized');
    }

    setupCanvas() {
        if (!this.waveformCanvas) return;
        const rect = this.waveformCanvas.getBoundingClientRect();
        const dpr = window.devicePixelRatio || 1;
        this.waveformCanvas.width = rect.width * dpr;
        this.waveformCanvas.height = rect.height * dpr;
        if (this.waveformCtx) {
            this.waveformCtx.scale(dpr, dpr);
        }
        this.canvasWidth = rect.width;
        this.canvasHeight = rect.height;
        this.drawWaveform();
    }

    setupEvents() {
        // Play button
        if (this.playBtn) {
            this.playBtn.addEventListener('click', () => this.togglePlay());
        }

        // Overdub button
        if (this.overdubBtn) {
            this.overdubBtn.addEventListener('click', () => this.toggleOverdub());
        }

        // Freeze button
        if (this.freezeBtn) {
            this.freezeBtn.addEventListener('click', () => this.toggleFreeze());
        }

        // Clear button
        if (this.clearBtn) {
            this.clearBtn.addEventListener('click', () => this.clear());
        }

        // Reverse button
        if (this.reverseBtn) {
            this.reverseBtn.addEventListener('click', () => this.toggleReverse());
        }

        // Mode buttons
        if (this.modeEnvBtn) {
            this.modeEnvBtn.addEventListener('click', () => this.setMode(0));
        }
        if (this.modeTapeBtn) {
            this.modeTapeBtn.addEventListener('click', () => this.setMode(1));
        }
        if (this.modeStretchBtn) {
            this.modeStretchBtn.addEventListener('click', () => this.setMode(2));
        }

        // Scale buttons
        this.scaleButtons.forEach(btn => {
            btn.addEventListener('click', () => {
                const scale = parseInt(btn.dataset.scale, 10);
                this.setScale(scale);
            });
        });
    }

    async setScale(scaleIndex) {
        try {
            await this.setScaleFn(scaleIndex);
            this.currentScale = scaleIndex;
            this.updateScaleUI();
            console.log(`[MICROLOOP] Scale set to ${scaleIndex}`);
        } catch (e) {
            console.error('Error setting scale:', e);
        }
    }

    updateScaleUI() {
        this.scaleButtons.forEach(btn => {
            const scale = parseInt(btn.dataset.scale, 10);
            btn.classList.toggle('active', scale === this.currentScale);
        });
    }

    async fetchInitialState() {
        try {
            const state = await this.getStateFn();
            if (state) {
                this.isPlaying = state.isPlaying || false;
                this.isOverdubbing = state.isOverdubbing || false;
                this.isFrozen = state.isFrozen || false;
                if (state.scale !== undefined) {
                    this.currentScale = state.scale;
                    this.updateScaleUI();
                }
                this.updateUI();
            }
        } catch (e) {
            console.log('Could not fetch initial micro looper state');
        }
    }

    startPolling() {
        // Poll for state updates every 100ms
        setInterval(async () => {
            try {
                const state = await this.getStateFn();
                if (state) {
                    this.isPlaying = state.isPlaying || false;
                    this.isOverdubbing = state.isOverdubbing || false;
                    this.isFrozen = state.isFrozen || false;
                    this.playPosition = state.playPosition || 0;
                    this.bufferFillAmount = state.bufferFill || 0;
                    if (state.mode !== undefined && state.mode !== this.currentMode) {
                        this.currentMode = state.mode;
                    }
                    if (state.scale !== undefined && state.scale !== this.currentScale) {
                        this.currentScale = state.scale;
                        this.updateScaleUI();
                    }
                    this.updateUI();
                }
            } catch (e) {
                // Ignore polling errors
            }
        }, 100);

        // Poll for waveform updates every 200ms (less frequent)
        setInterval(() => this.fetchWaveform(), 200);
    }

    async togglePlay() {
        try {
            await this.playFn();
            this.isPlaying = !this.isPlaying;
            // Update play button icon
            if (this.playBtn) {
                const icon = this.playBtn.querySelector('.micro-icon');
                if (icon) {
                    icon.textContent = this.isPlaying ? '⏹' : '▶';
                }
            }
            this.updateUI();
            console.log(`[MICROLOOP] ${this.isPlaying ? 'Playing' : 'Stopped'}`);
        } catch (e) {
            console.error('Error toggling micro looper play:', e);
        }
    }

    async toggleOverdub() {
        try {
            await this.overdubFn();
            this.isOverdubbing = !this.isOverdubbing;
            this.updateUI();
            console.log(`[MICROLOOP] Overdub ${this.isOverdubbing ? 'ON' : 'OFF'}`);
        } catch (e) {
            console.error('Error toggling micro looper overdub:', e);
        }
    }

    async toggleFreeze() {
        try {
            await this.freezeFn();
            this.isFrozen = !this.isFrozen;
            this.updateUI();
            console.log(`[MICROLOOP] Freeze ${this.isFrozen ? 'ON' : 'OFF'}`);
        } catch (e) {
            console.error('Error toggling micro looper freeze:', e);
        }
    }

    async clear() {
        try {
            await this.clearFn();
            this.isPlaying = false;
            this.isOverdubbing = false;
            this.isFrozen = false;
            // Reset play button icon
            if (this.playBtn) {
                const icon = this.playBtn.querySelector('.micro-icon');
                if (icon) {
                    icon.textContent = '▶';
                }
            }
            this.updateUI();
            console.log('[MICROLOOP] Cleared');
        } catch (e) {
            console.error('Error clearing micro looper:', e);
        }
    }

    async toggleReverse() {
        try {
            this.isReversed = !this.isReversed;
            await this.setReverseFn(this.isReversed);
            this.updateUI();
            console.log(`[MICROLOOP] Reverse ${this.isReversed ? 'ON' : 'OFF'}`);
        } catch (e) {
            console.error('Error toggling micro looper reverse:', e);
            this.isReversed = !this.isReversed;
        }
    }

    async setMode(mode) {
        try {
            this.currentMode = mode;
            await this.setModeFn(mode);
            this.updateModeButtons();
            this.updateModeDescription();
            const modeNames = ['ENV', 'TAPE', 'STRETCH'];
            console.log(`[MICROLOOP] Mode: ${modeNames[mode]}`);
        } catch (e) {
            console.error('Error setting micro looper mode:', e);
        }
    }

    updateModeButtons() {
        // Update mode button states
        if (this.modeEnvBtn) this.modeEnvBtn.classList.toggle('active', this.currentMode === 0);
        if (this.modeTapeBtn) this.modeTapeBtn.classList.toggle('active', this.currentMode === 1);
        if (this.modeStretchBtn) this.modeStretchBtn.classList.toggle('active', this.currentMode === 2);
    }

    updateModeDescription() {
        if (this.modeDesc) {
            this.modeDesc.textContent = this.modeDescriptions[this.currentMode] || '';
            this.modeDesc.classList.toggle('active', this.isPlaying);
        }
    }

    drawWaveform() {
        if (!this.waveformCtx || !this.canvasWidth || !this.canvasHeight) return;

        const ctx = this.waveformCtx;
        const width = this.canvasWidth;
        const height = this.canvasHeight;
        const centerY = height / 2;

        // Clear canvas
        ctx.fillStyle = 'rgba(5, 5, 5, 1)';
        ctx.fillRect(0, 0, width, height);

        // Draw waveform if we have data and buffer has content
        if (this.waveformData && this.waveformData.length > 0 && this.bufferFillAmount > 0) {
            const numPoints = this.waveformData.length;
            const fillRatio = this.bufferFillAmount;

            // Waveform color based on state (matches card visualization)
            let waveColor;
            if (this.isFrozen) {
                waveColor = 'rgba(96, 165, 250, 0.7)';  // Blue for frozen
            } else if (this.isOverdubbing) {
                waveColor = 'rgba(236, 72, 153, 0.8)';  // Pink for overdub
            } else if (this.isPlaying) {
                waveColor = 'rgba(74, 222, 128, 0.7)';  // Green for playing
            } else {
                waveColor = 'rgba(168, 85, 247, 0.6)';  // Purple (granular accent) default
            }

            // Draw filled waveform (mirrored, like card)
            ctx.beginPath();
            ctx.moveTo(0, centerY);

            for (let i = 0; i < numPoints; i++) {
                const x = (i / numPoints) * width * fillRatio;
                const amplitude = this.waveformData[i] * (height * 0.8);
                ctx.lineTo(x, centerY - amplitude / 2);
            }

            // Mirror bottom half
            for (let i = numPoints - 1; i >= 0; i--) {
                const x = (i / numPoints) * width * fillRatio;
                const amplitude = this.waveformData[i] * (height * 0.8);
                ctx.lineTo(x, centerY + amplitude / 2);
            }

            ctx.closePath();
            ctx.fillStyle = waveColor;
            ctx.fill();

            // Draw center line
            ctx.strokeStyle = 'rgba(255, 255, 255, 0.1)';
            ctx.lineWidth = 1;
            ctx.beginPath();
            ctx.moveTo(0, centerY);
            ctx.lineTo(width * fillRatio, centerY);
            ctx.stroke();

            // Draw playhead position
            const playX = this.playPosition * width * fillRatio;

            // Playhead glow
            const gradient = ctx.createLinearGradient(playX - 4, 0, playX + 4, 0);
            gradient.addColorStop(0, 'rgba(255, 255, 255, 0)');
            gradient.addColorStop(0.5, 'rgba(255, 255, 255, 0.8)');
            gradient.addColorStop(1, 'rgba(255, 255, 255, 0)');

            ctx.fillStyle = gradient;
            ctx.fillRect(playX - 4, 0, 8, height);

            // Playhead line
            ctx.strokeStyle = '#ffffff';
            ctx.lineWidth = 2;
            ctx.beginPath();
            ctx.moveTo(playX, 0);
            ctx.lineTo(playX, height);
            ctx.stroke();
        } else {
            // Draw subtle grid lines when no data
            ctx.strokeStyle = 'rgba(255, 255, 255, 0.05)';
            ctx.lineWidth = 1;
            for (let i = 0; i < 4; i++) {
                const x = (width / 4) * i;
                ctx.beginPath();
                ctx.moveTo(x, 0);
                ctx.lineTo(x, height);
                ctx.stroke();
            }

            // Draw "no data" indicator
            ctx.fillStyle = '#333';
            ctx.font = '10px JetBrains Mono';
            ctx.textAlign = 'center';
            ctx.fillText('Listening...', width / 2, centerY + 4);
        }
    }

    async fetchWaveform() {
        try {
            const waveform = await this.getWaveformFn(64);
            if (waveform && Array.isArray(waveform)) {
                this.waveformData = waveform;
                this.drawWaveform();
            }
        } catch (e) {
            // Ignore waveform fetch errors
        }
    }

    updatePlayhead() {
        if (this.playhead && this.canvasWidth) {
            const leftPercent = this.playPosition * 100;
            this.playhead.style.left = `${leftPercent}%`;
            this.playhead.classList.toggle('visible', this.isPlaying && this.waveformData.length > 0);
        }
    }

    updateBufferFill() {
        if (this.bufferFill) {
            this.bufferFill.style.width = `${this.bufferFillAmount * 100}%`;
        }
    }

    updateUI() {
        // Update transport button states
        if (this.playBtn) this.playBtn.classList.toggle('active', this.isPlaying);
        if (this.overdubBtn) this.overdubBtn.classList.toggle('active', this.isOverdubbing);
        if (this.freezeBtn) this.freezeBtn.classList.toggle('active', this.isFrozen);
        if (this.reverseBtn) this.reverseBtn.classList.toggle('active', this.isReversed);

        // Update mode buttons
        this.updateModeButtons();

        // Update playhead and visualization
        this.updatePlayhead();
        this.updateBufferFill();
        this.updateModeDescription();
    }
}

// Lofi section controller with LEDs and filter visualization
class LofiController {
    constructor() {
        // Master LED for entire lofi section
        this.masterLed = document.getElementById('lofi-master-led');
        console.log('[LOFI] Master LED element:', this.masterLed);

        // Section LEDs
        this.lofiLed = document.getElementById('lofi-led');
        this.microLoopLed = document.getElementById('microloop-led');
        this.filterLed = document.getElementById('filter-led');
        console.log('[LOFI] Lofi LED:', this.lofiLed, 'MicroLoop LED:', this.microLoopLed, 'Filter LED:', this.filterLed);

        // Individual filter toggle labels (now using labels instead of buttons)
        this.hpToggleLabel = document.getElementById('hp-toggle-label');
        this.lpToggleLabel = document.getElementById('lp-toggle-label');
        this.hpGroup = document.getElementById('hp-group');
        this.lpGroup = document.getElementById('lp-group');
        console.log('[LOFI] HP label:', this.hpToggleLabel, 'LP label:', this.lpToggleLabel);

        // Lofi section container (for dimming when master disabled)
        this.lofiSection = document.querySelector('.lofi-section');

        // Filter visualization canvas
        this.filterCanvas = document.getElementById('filter-viz-canvas');
        this.filterCtx = this.filterCanvas?.getContext('2d');

        // Setup canvas sizing
        if (this.filterCanvas) {
            this.setupCanvas();
            // Resize on window resize
            window.addEventListener('resize', () => this.setupCanvas());
        }

        // State
        this.masterEnabled = true;
        this.lofiEnabled = true;
        this.microLoopEnabled = true;
        this.filterEnabled = true;
        this.hpEnabled = true;
        this.lpEnabled = true;

        // Native functions
        this.setMasterEnabledFn = getNativeFunction('setDegradeEnabled');
        this.setLofiEnabledFn = getNativeFunction('setDegradeLofiEnabled');
        this.setMicroLoopEnabledFn = getNativeFunction('setMicroLooperEnabled');
        this.microLooperPlayFn = getNativeFunction('microLooperPlay');  // For auto-start on enable
        this.setFilterEnabledFn = getNativeFunction('setDegradeFilterEnabled');
        this.setHPEnabledFn = getNativeFunction('setDegradeHPEnabled');
        this.setLPEnabledFn = getNativeFunction('setDegradeLPEnabled');
        this.getDegradeStateFn = getNativeFunction('getDegradeState');

        // Current filter params for visualization
        this.hpFreq = 20;
        this.lpFreq = 20000;
        this.hpQ = 0.707;
        this.lpQ = 0.707;

        this.setupEvents();
        this.fetchInitialState();
        this.startPolling();
        console.log('[LOFI] Controller initialized');
    }

    setupCanvas() {
        if (!this.filterCanvas) return;
        // Set canvas resolution to match display size
        const rect = this.filterCanvas.getBoundingClientRect();
        const dpr = window.devicePixelRatio || 1;
        this.filterCanvas.width = rect.width * dpr;
        this.filterCanvas.height = rect.height * dpr;
        if (this.filterCtx) {
            this.filterCtx.scale(dpr, dpr);
        }
        this.canvasWidth = rect.width;
        this.canvasHeight = rect.height;
        this.drawFilterVisualization();
    }

    setupEvents() {
        // Card LED toggle (lofi-master-led on the card) - toggles master enable
        if (this.masterLed) {
            this.masterLed.addEventListener('click', (e) => {
                e.stopPropagation();
                this.toggleMaster();
            });
        }
        // Panel LO-FI section LED toggle - ALSO toggles master enable (same as card)
        if (this.lofiLed) {
            this.lofiLed.addEventListener('click', (e) => {
                e.stopPropagation();
                this.toggleMaster();  // Same action as card LED for consistency
            });
        }
        if (this.microLoopLed) {
            this.microLoopLed.addEventListener('click', (e) => {
                e.stopPropagation();
                this.toggleMicroLoop();
            });
        }
        // Filter section LED toggle
        if (this.filterLed) {
            this.filterLed.addEventListener('click', (e) => {
                e.stopPropagation();
                this.toggleFilter();
            });
        }
        // Individual HP/LP label toggles
        if (this.hpToggleLabel) {
            this.hpToggleLabel.addEventListener('click', () => this.toggleHP());
        }
        if (this.lpToggleLabel) {
            this.lpToggleLabel.addEventListener('click', () => this.toggleLP());
        }
    }

    async fetchInitialState() {
        try {
            const state = await this.getDegradeStateFn();
            if (state) {
                // C++ returns 'enabled', we use 'masterEnabled' internally
                this.masterEnabled = state.enabled !== false;
                this.lofiEnabled = state.lofiEnabled !== false;
                this.microLoopEnabled = state.microLooperEnabled !== false;
                this.filterEnabled = state.filterEnabled !== false;
                this.hpEnabled = state.hpEnabled !== false;
                this.lpEnabled = state.lpEnabled !== false;
                this.updateUI();
            }
        } catch (e) {
            console.log('Could not fetch initial lofi state');
        }
    }

    async toggleFilter() {
        this.filterEnabled = !this.filterEnabled;
        this.updateUI();
        try {
            await this.setFilterEnabledFn(this.filterEnabled);
            console.log(`[LOFI] Filter section ${this.filterEnabled ? 'enabled' : 'disabled'}`);
        } catch (e) {
            console.error('Error toggling filter section:', e);
            this.filterEnabled = !this.filterEnabled;
            this.updateUI();
        }
    }

    async toggleMaster() {
        this.masterEnabled = !this.masterEnabled;
        this.updateUI();
        try {
            await this.setMasterEnabledFn(this.masterEnabled);
            console.log(`[LOFI] Master ${this.masterEnabled ? 'enabled' : 'disabled'}`);
        } catch (e) {
            console.error('Error toggling lofi master:', e);
            this.masterEnabled = !this.masterEnabled;
            this.updateUI();
        }
    }

    async toggleHP() {
        this.hpEnabled = !this.hpEnabled;
        this.updateFilterToggles();
        try {
            await this.setHPEnabledFn(this.hpEnabled);
            console.log(`[LOFI] HP filter ${this.hpEnabled ? 'enabled' : 'disabled'}`);
        } catch (e) {
            console.error('Error toggling HP filter:', e);
            this.hpEnabled = !this.hpEnabled;
            this.updateFilterToggles();
        }
    }

    async toggleLP() {
        this.lpEnabled = !this.lpEnabled;
        this.updateFilterToggles();
        try {
            await this.setLPEnabledFn(this.lpEnabled);
            console.log(`[LOFI] LP filter ${this.lpEnabled ? 'enabled' : 'disabled'}`);
        } catch (e) {
            console.error('Error toggling LP filter:', e);
            this.lpEnabled = !this.lpEnabled;
            this.updateFilterToggles();
        }
    }

    updateFilterToggles() {
        // Update HP/LP toggle labels
        if (this.hpToggleLabel) {
            this.hpToggleLabel.classList.toggle('active', this.hpEnabled);
        }
        if (this.lpToggleLabel) {
            this.lpToggleLabel.classList.toggle('active', this.lpEnabled);
        }
        // Dim the knob groups when disabled
        if (this.hpGroup) {
            this.hpGroup.style.opacity = this.hpEnabled ? '1' : '0.5';
        }
        if (this.lpGroup) {
            this.lpGroup.style.opacity = this.lpEnabled ? '1' : '0.5';
        }
    }


    async toggleLofi() {
        this.lofiEnabled = !this.lofiEnabled;
        this.updateUI();
        try {
            await this.setLofiEnabledFn(this.lofiEnabled);
            console.log(`[LOFI] Lo-Fi ${this.lofiEnabled ? 'enabled' : 'disabled'}`);
        } catch (e) {
            console.error('Error toggling lo-fi:', e);
            this.lofiEnabled = !this.lofiEnabled;
            this.updateUI();
        }
    }

    async toggleMicroLoop() {
        this.microLoopEnabled = !this.microLoopEnabled;
        this.updateUI();
        try {
            await this.setMicroLoopEnabledFn(this.microLoopEnabled);
            console.log(`[LOFI] MicroLoop ${this.microLoopEnabled ? 'enabled' : 'disabled'}`);

            // When enabling, also start playback so sonic transformation begins immediately
            if (this.microLoopEnabled) {
                await this.microLooperPlayFn();
                console.log('[LOFI] MicroLoop auto-started playback');
            }
        } catch (e) {
            console.error('Error toggling micro loop:', e);
            this.microLoopEnabled = !this.microLoopEnabled;
            this.updateUI();
        }
    }

    updateUI() {
        // Update Card LED (lofi-master-led) - reflects master enable state
        if (this.masterLed) {
            this.masterLed.classList.toggle('active', this.masterEnabled);
        }

        // Update panel LO-FI LED - SAME state as card (both show master enable)
        if (this.lofiLed) {
            this.lofiLed.classList.toggle('active', this.masterEnabled);
        }
        if (this.microLoopLed) {
            this.microLoopLed.classList.toggle('active', this.microLoopEnabled);
        }
        if (this.filterLed) {
            this.filterLed.classList.toggle('active', this.filterEnabled);
        }

        // Update entire lofi section opacity when master is disabled
        if (this.lofiSection) {
            // Apply dim to everything except the header with master LED
            const sections = this.lofiSection.querySelectorAll('.p-3');
            sections.forEach(section => {
                // Skip if it's a header section (contains the master LED)
                if (!section.querySelector('#lofi-master-led')) {
                    section.style.opacity = this.masterEnabled ? '1' : '0.4';
                    section.style.pointerEvents = this.masterEnabled ? 'auto' : 'none';
                }
            });
        }

        // Update individual filter toggles
        this.updateFilterToggles();

        // Update filter visualization based on filter enabled state
        this.drawFilterVisualization();
    }

    startPolling() {
        // Poll for filter visualization updates
        setInterval(async () => {
            try {
                const state = await this.getDegradeStateFn();
                if (state) {
                    // Update filter params for visualization
                    if (typeof state.hpFreq !== 'undefined') this.hpFreq = state.hpFreq;
                    if (typeof state.lpFreq !== 'undefined') this.lpFreq = state.lpFreq;
                    if (typeof state.hpQ !== 'undefined') this.hpQ = state.hpQ;
                    if (typeof state.lpQ !== 'undefined') this.lpQ = state.lpQ;

                    // Redraw filter visualization
                    this.drawFilterVisualization();
                }
            } catch (e) {
                // Silently ignore polling errors
            }
        }, 100);
    }

    drawFilterVisualization() {
        if (!this.filterCtx || !this.filterCanvas) return;

        const width = this.canvasWidth || this.filterCanvas.width;
        const height = this.canvasHeight || this.filterCanvas.height;
        const ctx = this.filterCtx;

        // Reset transform and clear
        ctx.setTransform(1, 0, 0, 1, 0, 0);
        const dpr = window.devicePixelRatio || 1;
        ctx.scale(dpr, dpr);

        // Clear canvas
        ctx.fillStyle = '#0a0a0a';
        ctx.fillRect(0, 0, width, height);

        // If filter section is disabled, show dimmed state
        if (!this.filterEnabled) {
            ctx.fillStyle = 'rgba(0, 0, 0, 0.5)';
            ctx.fillRect(0, 0, width, height);
            ctx.fillStyle = '#333';
            ctx.font = '10px "JetBrains Mono", monospace';
            ctx.textAlign = 'center';
            ctx.fillText('FILTER OFF', width / 2, height / 2 + 4);
            return;
        }

        // Draw grid lines
        ctx.strokeStyle = '#1a1a1a';
        ctx.lineWidth = 1;

        // Vertical grid (frequency markers: 100Hz, 1kHz, 10kHz)
        const freqMarkers = [100, 1000, 10000];
        freqMarkers.forEach(freq => {
            const x = this.freqToX(freq, width);
            ctx.beginPath();
            ctx.moveTo(x, 0);
            ctx.lineTo(x, height);
            ctx.stroke();
        });

        // Horizontal center line (0dB)
        ctx.beginPath();
        ctx.moveTo(0, height / 2);
        ctx.lineTo(width, height / 2);
        ctx.stroke();

        // Draw shaded cut regions first (behind the curve)
        // HP cut region (frequencies below HP cutoff)
        if (this.hpEnabled && this.hpFreq > 20) {
            const hpX = this.freqToX(this.hpFreq, width);
            ctx.fillStyle = 'rgba(102, 187, 106, 0.15)';
            ctx.fillRect(0, 0, hpX, height);
        }

        // LP cut region (frequencies above LP cutoff)
        if (this.lpEnabled && this.lpFreq < 20000) {
            const lpX = this.freqToX(this.lpFreq, width);
            ctx.fillStyle = 'rgba(102, 187, 106, 0.15)';
            ctx.fillRect(lpX, 0, width - lpX, height);
        }

        // Draw combined filter response curve
        ctx.beginPath();
        ctx.strokeStyle = '#66bb6a';
        ctx.lineWidth = 2;

        for (let i = 0; i < width; i++) {
            const freq = this.xToFreq(i, width);
            const hpResponse = this.hpEnabled ? this.getHPResponse(freq) : 0;
            const lpResponse = this.lpEnabled ? this.getLPResponse(freq) : 0;
            const combinedDb = hpResponse + lpResponse;

            // Map dB to y (0dB at center, +/-24dB at edges)
            const y = height / 2 - (combinedDb / 24) * (height / 2);

            if (i === 0) {
                ctx.moveTo(i, Math.max(0, Math.min(height, y)));
            } else {
                ctx.lineTo(i, Math.max(0, Math.min(height, y)));
            }
        }
        ctx.stroke();

        // Frequency labels
        ctx.fillStyle = '#444';
        ctx.font = '8px "JetBrains Mono", monospace';
        ctx.textAlign = 'center';
        ctx.fillText('100', this.freqToX(100, width), height - 2);
        ctx.fillText('1k', this.freqToX(1000, width), height - 2);
        ctx.fillText('10k', this.freqToX(10000, width), height - 2);
    }

    // Convert frequency to X position (logarithmic scale, 20Hz to 20kHz)
    freqToX(freq, width) {
        const minFreq = 20;
        const maxFreq = 20000;
        const logMin = Math.log10(minFreq);
        const logMax = Math.log10(maxFreq);
        const logFreq = Math.log10(Math.max(minFreq, Math.min(maxFreq, freq)));
        return ((logFreq - logMin) / (logMax - logMin)) * width;
    }

    // Convert X position to frequency
    xToFreq(x, width) {
        const minFreq = 20;
        const maxFreq = 20000;
        const logMin = Math.log10(minFreq);
        const logMax = Math.log10(maxFreq);
        const logFreq = logMin + (x / width) * (logMax - logMin);
        return Math.pow(10, logFreq);
    }

    // Calculate HP filter response in dB at given frequency
    getHPResponse(freq) {
        const ratio = freq / this.hpFreq;
        // Second-order high-pass: -12dB/octave below cutoff
        if (ratio < 1) {
            const octaves = Math.log2(1 / ratio);
            return -12 * octaves + (this.hpQ - 0.707) * 6 * Math.exp(-octaves);
        }
        // Resonance peak at cutoff
        if (ratio < 2) {
            const peak = (this.hpQ - 0.707) * 6;
            return peak * Math.exp(-Math.abs(ratio - 1) * 2);
        }
        return 0;
    }

    // Calculate LP filter response in dB at given frequency
    getLPResponse(freq) {
        const ratio = freq / this.lpFreq;
        // Second-order low-pass: -12dB/octave above cutoff
        if (ratio > 1) {
            const octaves = Math.log2(ratio);
            return -12 * octaves + (this.lpQ - 0.707) * 6 * Math.exp(-octaves);
        }
        // Resonance peak at cutoff
        if (ratio > 0.5) {
            const peak = (this.lpQ - 0.707) * 6;
            return peak * Math.exp(-Math.abs(ratio - 1) * 2);
        }
        return 0;
    }
}

// Audio Diagnostics Controller
class DiagnosticsController {
    constructor() {
        this.panel = document.getElementById('diag-panel');
        this.toggleBtn = document.getElementById('diag-toggle');
        this.resetBtn = document.getElementById('diag-reset');

        // Display elements
        this.preClipL = document.getElementById('diag-preclip-l');
        this.preClipR = document.getElementById('diag-preclip-r');
        this.loopL = document.getElementById('diag-loop-l');
        this.loopR = document.getElementById('diag-loop-r');
        this.clipsEl = document.getElementById('diag-clips');
        this.layerClipsContainer = document.getElementById('diag-layer-clips');

        // Native functions
        this.getDiagnosticsFn = getNativeFunction('getAudioDiagnostics');
        this.resetDiagnosticsFn = getNativeFunction('resetAudioDiagnostics');

        this.isVisible = false;
        this.pollingInterval = null;

        this.setupEvents();
    }

    setupEvents() {
        if (this.toggleBtn) {
            this.toggleBtn.addEventListener('click', () => this.toggle());
        }
        if (this.resetBtn) {
            this.resetBtn.addEventListener('click', () => this.reset());
        }
    }

    toggle() {
        this.isVisible = !this.isVisible;
        if (this.panel) {
            this.panel.classList.toggle('hidden', !this.isVisible);
        }

        if (this.isVisible) {
            this.startPolling();
        } else {
            this.stopPolling();
        }
    }

    async reset() {
        try {
            await this.resetDiagnosticsFn();
            console.log('[DIAG] Reset diagnostics');
        } catch (e) {
            console.error('[DIAG] Reset error:', e);
        }
    }

    startPolling() {
        if (this.pollingInterval) return;

        this.pollingInterval = setInterval(async () => {
            try {
                const data = await this.getDiagnosticsFn();
                if (data) {
                    this.updateDisplay(data);
                }
            } catch (e) {
                // Silently ignore
            }
        }, 100);
    }

    stopPolling() {
        if (this.pollingInterval) {
            clearInterval(this.pollingInterval);
            this.pollingInterval = null;
        }
    }

    updateDisplay(data) {
        // Format value with color coding based on level
        const formatLevel = (val) => {
            const db = 20 * Math.log10(Math.max(val, 0.0001));
            let color = 'text-green-400';
            if (val > 1.0) color = 'text-red-500 font-bold';
            else if (val > 0.9) color = 'text-yellow-400';
            else if (val > 0.7) color = 'text-orange-400';

            return { text: val.toFixed(2), color, db: db.toFixed(1) };
        };

        // Update pre-clip peaks
        if (this.preClipL) {
            const info = formatLevel(data.preClipPeakL);
            this.preClipL.textContent = `${info.text} (${info.db}dB)`;
            this.preClipL.className = `text-right ${info.color}`;
        }
        if (this.preClipR) {
            const info = formatLevel(data.preClipPeakR);
            this.preClipR.textContent = `${info.text} (${info.db}dB)`;
            this.preClipR.className = `text-right ${info.color}`;
        }

        // Update loop output peaks
        if (this.loopL) {
            const info = formatLevel(data.loopOutputPeakL);
            this.loopL.textContent = `${info.text} (${info.db}dB)`;
            this.loopL.className = `text-right ${info.color}`;
        }
        if (this.loopR) {
            const info = formatLevel(data.loopOutputPeakR);
            this.loopR.textContent = `${info.text} (${info.db}dB)`;
            this.loopR.className = `text-right ${info.color}`;
        }

        // Update clip count
        if (this.clipsEl) {
            const clips = data.clipEventCount || 0;
            this.clipsEl.textContent = clips.toLocaleString();
            this.clipsEl.className = clips > 0 ? 'text-right text-red-500 font-bold' : 'text-right text-yellow-400';
        }

        // Update per-layer clip counts
        if (this.layerClipsContainer && data.layerClipCounts) {
            const spans = this.layerClipsContainer.querySelectorAll('span');
            data.layerClipCounts.forEach((count, idx) => {
                if (spans[idx]) {
                    spans[idx].textContent = `${idx + 1}:${count}`;
                    if (count > 0) {
                        spans[idx].className = 'px-1 bg-red-900 text-red-300 rounded';
                    } else {
                        spans[idx].className = 'px-1 bg-fd-surface rounded';
                    }
                }
            });
        }
    }
}

// Crossfade Settings Controller
class CrossfadeSettingsController {
    constructor() {
        this.panel = document.getElementById('xfade-panel');
        this.openBtn = document.getElementById('xfade-settings-btn');
        this.enableToggle = document.getElementById('xfade-enable-toggle');

        // Sliders
        this.preTimeSlider = document.getElementById('xfade-pre-time');
        this.postTimeSlider = document.getElementById('xfade-post-time');
        this.volDepthSlider = document.getElementById('xfade-vol-depth');
        this.filterFreqSlider = document.getElementById('xfade-filter-freq');
        this.filterDepthSlider = document.getElementById('xfade-filter-depth');
        this.smearAmountSlider = document.getElementById('xfade-smear-amount');
        this.smearAttackSlider = document.getElementById('xfade-smear-attack');
        this.smearLengthSlider = document.getElementById('xfade-smear-length');

        // Value displays
        this.preTimeVal = document.getElementById('xfade-pre-time-val');
        this.postTimeVal = document.getElementById('xfade-post-time-val');
        this.volDepthVal = document.getElementById('xfade-vol-depth-val');
        this.filterFreqVal = document.getElementById('xfade-filter-freq-val');
        this.filterDepthVal = document.getElementById('xfade-filter-depth-val');
        this.smearAmountVal = document.getElementById('xfade-smear-amount-val');
        this.smearAttackVal = document.getElementById('xfade-smear-attack-val');
        this.smearLengthVal = document.getElementById('xfade-smear-length-val');

        // Visualizer
        this.vizCanvas = document.getElementById('xfade-viz-canvas');
        this.vizCtx = this.vizCanvas?.getContext('2d');
        this.animationId = null;

        // Native functions
        this.setCrossfadeParamsFn = getNativeFunction('setCrossfadeParams');
        this.saveCrossfadeSettingsFn = getNativeFunction('saveCrossfadeSettings');
        this.loadCrossfadeSettingsFn = getNativeFunction('loadCrossfadeSettings');

        console.log('[XFADE] Controller initialized');
        console.log('[XFADE] Panel element:', this.panel ? 'found' : 'NOT FOUND');
        console.log('[XFADE] Sliders:', {
            preTime: this.preTimeSlider ? 'found' : 'NOT FOUND',
            postTime: this.postTimeSlider ? 'found' : 'NOT FOUND',
            filterFreq: this.filterFreqSlider ? 'found' : 'NOT FOUND',
            filterDepth: this.filterDepthSlider ? 'found' : 'NOT FOUND'
        });
        console.log('[XFADE] Native fn:', this.setCrossfadeParamsFn ? 'available' : 'NOT AVAILABLE');

        // Enabled state
        this.enabled = true;

        this.setupEvents();
        this.updateEnabledUI();  // Initialize LED state before loading
        this.loadSettings();
        this.updateDisplays();

        // Also send initial settings after a short delay to ensure JUCE backend is ready
        setTimeout(() => {
            console.log('[XFADE] Delayed initial send');
            this.sendToPlugin();
        }, 500);
    }

    setupEvents() {
        // Open/close panel
        if (this.openBtn) {
            this.openBtn.addEventListener('click', (e) => {
                e.stopPropagation();
                this.toggle();
            });
        }

        // Click outside to close
        document.addEventListener('click', (e) => {
            if (this.panel && !this.panel.classList.contains('hidden')) {
                if (!this.panel.contains(e.target) && e.target !== this.openBtn) {
                    this.close();
                }
            }
        });

        // Reposition panel on window resize
        window.addEventListener('resize', () => {
            if (this.panel && !this.panel.classList.contains('hidden')) {
                this.repositionPanel();
            }
        });

        // LED toggle for enable/disable
        if (this.enableToggle) {
            this.enableToggle.addEventListener('click', () => this.toggleEnabled());
        }

        // Slider changes
        const sliders = [
            this.preTimeSlider,
            this.postTimeSlider,
            this.volDepthSlider,
            this.filterFreqSlider,
            this.filterDepthSlider,
            this.smearAmountSlider,
            this.smearAttackSlider,
            this.smearLengthSlider
        ];
        sliders.forEach(slider => {
            if (slider) {
                slider.addEventListener('input', () => {
                    this.updateDisplays();
                    this.sendToPlugin();
                    this.drawVisualizer();
                });
            }
        });
    }

    toggle() {
        if (this.panel?.classList.contains('hidden')) {
            this.open();
        } else {
            this.close();
        }
    }

    repositionPanel() {
        if (!this.panel || !this.openBtn) return;

        const btnRect = this.openBtn.getBoundingClientRect();
        const panelRect = this.panel.getBoundingClientRect();
        const viewportHeight = window.innerHeight;
        const viewportWidth = window.innerWidth;

        // Default position: right of button, vertically centered above it
        let left = btnRect.right + 8;
        let top = btnRect.top - panelRect.height + 30;  // Anchor near bottom of panel to button

        // Adjust if panel goes off right edge
        if (left + panelRect.width > viewportWidth) {
            left = btnRect.left - panelRect.width - 8;  // Show on left instead
        }

        // Adjust if panel goes off bottom
        if (top + panelRect.height > viewportHeight - 10) {
            top = viewportHeight - panelRect.height - 10;
        }

        // Adjust if panel goes off top
        if (top < 10) {
            top = 10;
        }

        this.panel.style.left = `${left}px`;
        this.panel.style.top = `${top}px`;
    }

    open() {
        if (this.panel && this.openBtn) {
            // First unhide to get actual dimensions
            this.panel.classList.remove('hidden');
            this.repositionPanel();
            this.openBtn.classList.add('active');
            this.startVisualizerAnimation();
        }
    }

    close() {
        if (this.panel) {
            this.panel.classList.add('hidden');
            this.openBtn?.classList.remove('active');
            this.stopVisualizerAnimation();
        }
    }

    toggleEnabled() {
        this.enabled = !this.enabled;
        this.updateEnabledUI();
        this.sendToPlugin();
        this.saveSettings();
    }

    updateEnabledUI() {
        // Update the toggle button in the panel
        if (this.enableToggle) {
            const label = this.enableToggle.querySelector('.led-label');
            if (this.enabled) {
                this.enableToggle.classList.add('active');
                if (label) label.textContent = 'ON';
            } else {
                this.enableToggle.classList.remove('active');
                if (label) label.textContent = 'OFF';
            }
        }
        // Also update the main crossfade icon to illuminate when enabled
        if (this.openBtn) {
            this.openBtn.classList.toggle('enabled', this.enabled);
        }
    }

    updateDisplays() {
        // Pre-fade time
        if (this.preTimeVal && this.preTimeSlider) {
            this.preTimeVal.textContent = this.preTimeSlider.value;
        }

        // Post-fade time
        if (this.postTimeVal && this.postTimeSlider) {
            this.postTimeVal.textContent = this.postTimeSlider.value;
        }

        // Volume depth (0 = full mute at boundary, 100 = no ducking)
        if (this.volDepthVal && this.volDepthSlider) {
            this.volDepthVal.textContent = this.volDepthSlider.value;
        }

        // Filter frequency (logarithmic: 0 = OFF, 100 = 100Hz to 0 = 20kHz)
        if (this.filterFreqVal && this.filterFreqSlider) {
            const val = parseInt(this.filterFreqSlider.value);
            if (val === 0) {
                this.filterFreqVal.textContent = 'OFF';
            } else {
                // Map 1-100 to 100Hz - 20kHz (logarithmic)
                const hz = 100 * Math.pow(200, val / 100);  // 100Hz to 20kHz
                this.filterFreqVal.textContent = hz >= 1000 ? `${(hz/1000).toFixed(1)}k` : `${Math.round(hz)}`;
            }
        }

        // Filter depth
        if (this.filterDepthVal && this.filterDepthSlider) {
            this.filterDepthVal.textContent = this.filterDepthSlider.value;
        }

        // Smear amount
        if (this.smearAmountVal && this.smearAmountSlider) {
            this.smearAmountVal.textContent = this.smearAmountSlider.value;
        }

        // Smear attack
        if (this.smearAttackVal && this.smearAttackSlider) {
            this.smearAttackVal.textContent = this.smearAttackSlider.value;
        }

        // Smear length
        if (this.smearLengthVal && this.smearLengthSlider) {
            this.smearLengthVal.textContent = this.smearLengthSlider.value;
        }
    }

    // Visualizer animation
    startVisualizerAnimation() {
        this.drawVisualizer();
        const animate = () => {
            this.drawVisualizer();
            this.animationId = requestAnimationFrame(animate);
        };
        this.animationId = requestAnimationFrame(animate);
    }

    stopVisualizerAnimation() {
        if (this.animationId) {
            cancelAnimationFrame(this.animationId);
            this.animationId = null;
        }
    }

    drawVisualizer() {
        if (!this.vizCtx || !this.vizCanvas) return;

        const ctx = this.vizCtx;
        const width = this.vizCanvas.width;
        const height = this.vizCanvas.height;

        // Clear with subtle gradient
        const bgGrad = ctx.createLinearGradient(0, 0, 0, height);
        bgGrad.addColorStop(0, '#080808');
        bgGrad.addColorStop(1, '#040404');
        ctx.fillStyle = bgGrad;
        ctx.fillRect(0, 0, width, height);

        // Get values
        const preTime = parseInt(this.preTimeSlider?.value || 80);
        const postTime = parseInt(this.postTimeSlider?.value || 100);
        const volDepth = parseInt(this.volDepthSlider?.value || 100) / 100;  // 0 = full duck, 1 = no duck (default: no duck)
        const filterFreqRaw = parseInt(this.filterFreqSlider?.value || 0);
        const filterDepth = parseInt(this.filterDepthSlider?.value || 0) / 100;
        const smearAmount = parseInt(this.smearAmountSlider?.value || 0) / 100;

        const maxTime = 500;
        const centerX = width / 2;
        const preWidth = (preTime / maxTime) * (width / 2);
        const postWidth = (postTime / maxTime) * (width / 2);
        const margin = 4;

        // Draw subtle grid lines (horizontal)
        ctx.strokeStyle = '#151515';
        ctx.lineWidth = 1;
        for (let i = 1; i < 4; i++) {
            const y = (height / 4) * i;
            ctx.beginPath();
            ctx.moveTo(margin, y);
            ctx.lineTo(width - margin, y);
            ctx.stroke();
        }

        // Draw subtle grid lines (vertical time markers)
        ctx.strokeStyle = '#151515';
        const timeMarks = [0.25, 0.5, 0.75];
        for (const mark of timeMarks) {
            const x1 = centerX - (width / 2 - margin) * mark;
            const x2 = centerX + (width / 2 - margin) * mark;
            ctx.beginPath();
            ctx.moveTo(x1, margin);
            ctx.lineTo(x1, height - margin);
            ctx.moveTo(x2, margin);
            ctx.lineTo(x2, height - margin);
            ctx.stroke();
        }

        // Draw baseline (0 dB line)
        const baselineY = height * 0.15;
        ctx.strokeStyle = '#252525';
        ctx.lineWidth = 1;
        ctx.setLineDash([2, 2]);
        ctx.beginPath();
        ctx.moveTo(margin, baselineY);
        ctx.lineTo(width - margin, baselineY);
        ctx.stroke();
        ctx.setLineDash([]);

        // Draw LP filter envelope (main effect visualization)
        if (this.enabled && filterFreqRaw > 0 && filterDepth > 0) {
            const filterBaseY = height * 0.2;
            const filterMaxY = filterBaseY + (height * 0.6 * filterDepth);

            // Fill area with gradient
            const filterGrad = ctx.createLinearGradient(0, filterBaseY, 0, filterMaxY);
            filterGrad.addColorStop(0, 'rgba(255, 152, 0, 0.05)');
            filterGrad.addColorStop(1, 'rgba(255, 152, 0, 0.2)');
            ctx.fillStyle = filterGrad;

            ctx.beginPath();
            ctx.moveTo(centerX - preWidth, filterBaseY);
            ctx.bezierCurveTo(
                centerX - preWidth * 0.5, filterBaseY,
                centerX - preWidth * 0.2, filterMaxY,
                centerX, filterMaxY
            );
            ctx.bezierCurveTo(
                centerX + postWidth * 0.2, filterMaxY,
                centerX + postWidth * 0.5, filterBaseY,
                centerX + postWidth, filterBaseY
            );
            ctx.lineTo(centerX + postWidth, height - margin);
            ctx.lineTo(centerX - preWidth, height - margin);
            ctx.closePath();
            ctx.fill();

            // Stroke line with glow
            ctx.shadowColor = 'rgba(255, 152, 0, 0.5)';
            ctx.shadowBlur = 4;
            ctx.strokeStyle = '#ff9800';
            ctx.lineWidth = 1.5;
            ctx.beginPath();
            ctx.moveTo(centerX - preWidth, filterBaseY);
            ctx.bezierCurveTo(
                centerX - preWidth * 0.5, filterBaseY,
                centerX - preWidth * 0.2, filterMaxY,
                centerX, filterMaxY
            );
            ctx.bezierCurveTo(
                centerX + postWidth * 0.2, filterMaxY,
                centerX + postWidth * 0.5, filterBaseY,
                centerX + postWidth, filterBaseY
            );
            ctx.stroke();
            ctx.shadowBlur = 0;

            // Filter label
            ctx.fillStyle = 'rgba(255, 152, 0, 0.6)';
            ctx.font = '7px JetBrains Mono';
            ctx.textAlign = 'left';
            ctx.fillText('LP FILTER', margin + 2, height - 6);
        }

        // Note: Volume duck visualization removed (feature hidden, LP filter is more effective)

        // Draw smear fill visualization (shows audio being captured and blended)
        if (this.enabled && smearAmount > 0) {
            const smearY = height * 0.4;
            const smearHeight = height * 0.3 * smearAmount;

            // Gradient fill showing captured audio zone
            const smearGrad = ctx.createLinearGradient(centerX - preWidth, 0, centerX + postWidth, 0);
            smearGrad.addColorStop(0, 'rgba(156, 39, 176, 0.0)');      // Transparent at start
            smearGrad.addColorStop(0.3, 'rgba(156, 39, 176, 0.15)');   // Capture zone (purple)
            smearGrad.addColorStop(0.5, 'rgba(156, 39, 176, 0.3)');    // Peak at boundary
            smearGrad.addColorStop(0.7, 'rgba(156, 39, 176, 0.15)');   // Blend zone
            smearGrad.addColorStop(1, 'rgba(156, 39, 176, 0.0)');      // Transparent at end

            ctx.fillStyle = smearGrad;
            ctx.fillRect(centerX - preWidth, smearY, preWidth + postWidth, smearHeight);

            // Horizontal lines showing smear texture
            ctx.strokeStyle = 'rgba(156, 39, 176, 0.4)';
            ctx.lineWidth = 1;
            const numLines = 3;
            for (let i = 0; i < numLines; i++) {
                const y = smearY + (smearHeight / (numLines + 1)) * (i + 1);
                ctx.beginPath();
                ctx.moveTo(centerX - preWidth * 0.8, y);
                ctx.lineTo(centerX + postWidth * 0.8, y);
                ctx.stroke();
            }

            // Label
            ctx.fillStyle = 'rgba(156, 39, 176, 0.6)';
            ctx.font = '7px JetBrains Mono';
            ctx.textAlign = 'right';
            ctx.fillText('SMEAR', width - margin - 2, height - 6);
        }

        // Draw center boundary line (on top)
        ctx.shadowColor = 'rgba(79, 195, 247, 0.8)';
        ctx.shadowBlur = 6;
        ctx.strokeStyle = '#4fc3f7';
        ctx.lineWidth = 2;
        ctx.beginPath();
        ctx.moveTo(centerX, margin);
        ctx.lineTo(centerX, height - margin);
        ctx.stroke();
        ctx.shadowBlur = 0;

        // Draw time zone indicators
        if (this.enabled) {
            // Pre zone bracket
            ctx.strokeStyle = 'rgba(79, 195, 247, 0.3)';
            ctx.lineWidth = 1;
            ctx.beginPath();
            ctx.moveTo(centerX - preWidth, margin + 2);
            ctx.lineTo(centerX - preWidth, margin + 8);
            ctx.stroke();

            // Post zone bracket
            ctx.beginPath();
            ctx.moveTo(centerX + postWidth, margin + 2);
            ctx.lineTo(centerX + postWidth, margin + 8);
            ctx.stroke();
        }

        // Draw disabled overlay
        if (!this.enabled) {
            ctx.fillStyle = 'rgba(0, 0, 0, 0.6)';
            ctx.fillRect(0, 0, width, height);

            // Strikethrough line
            ctx.strokeStyle = '#333';
            ctx.lineWidth = 1;
            ctx.beginPath();
            ctx.moveTo(margin, height / 2);
            ctx.lineTo(width - margin, height / 2);
            ctx.stroke();

            ctx.fillStyle = '#444';
            ctx.font = '9px Orbitron';
            ctx.textAlign = 'center';
            ctx.fillText('OFF', centerX, height / 2 + 3);
        }
    }

    async loadSettings() {
        if (this.loadCrossfadeSettingsFn) {
            try {
                const settings = await this.loadCrossfadeSettingsFn();
                if (settings) {
                    if (this.preTimeSlider) this.preTimeSlider.value = settings.preTime || 80;
                    if (this.postTimeSlider) this.postTimeSlider.value = settings.postTime || 100;
                    if (this.volDepthSlider) this.volDepthSlider.value = settings.volDepth !== undefined ? settings.volDepth : 100;
                    if (this.filterFreqSlider) this.filterFreqSlider.value = settings.filterFreq || 0;
                    if (this.filterDepthSlider) this.filterDepthSlider.value = settings.filterDepth || 0;
                    if (this.smearAmountSlider) this.smearAmountSlider.value = settings.smearAmount || 0;
                    if (this.smearAttackSlider) this.smearAttackSlider.value = settings.smearAttack || 10;
                    if (this.smearLengthSlider) this.smearLengthSlider.value = settings.smearLength || 100;
                    this.enabled = settings.enabled !== false;  // Default to true
                    this.updateEnabledUI();
                    this.updateDisplays();
                }
            } catch (e) {
                console.log('[XFADE] No saved settings found, using defaults');
            }
        }
        // Always send current settings to plugin on load (even with defaults)
        this.sendToPlugin();
    }

    async saveSettings() {
        if (this.saveCrossfadeSettingsFn) {
            try {
                await this.saveCrossfadeSettingsFn({
                    preTime: parseInt(this.preTimeSlider?.value || 80),
                    postTime: parseInt(this.postTimeSlider?.value || 100),
                    volDepth: parseInt(this.volDepthSlider?.value || 100),
                    filterFreq: parseInt(this.filterFreqSlider?.value || 0),
                    filterDepth: parseInt(this.filterDepthSlider?.value || 0),
                    smearAmount: parseInt(this.smearAmountSlider?.value || 0),
                    smearAttack: parseInt(this.smearAttackSlider?.value || 10),
                    smearLength: parseInt(this.smearLengthSlider?.value || 100),
                    enabled: this.enabled
                });
            } catch (e) {
                console.error('[XFADE] Error saving settings:', e);
            }
        }
    }

    async sendToPlugin() {
        console.log('[XFADE] sendToPlugin called, enabled=' + this.enabled);
        if (!this.setCrossfadeParamsFn) {
            console.warn('[XFADE] setCrossfadeParamsFn not available yet');
            return;
        }

        // If disabled, send zeros
        if (!this.enabled) {
            console.log('[XFADE] Disabled - sending minimal params');
            try {
                await this.setCrossfadeParamsFn(5, 5, 1.0, 0, 0, 0, 0.1, 1.0);  // Minimal effect
            } catch (e) {
                console.error('[XFADE] Error sending params:', e);
            }
            return;
        }

        const preTimeMs = parseInt(this.preTimeSlider?.value || 80);
        const postTimeMs = parseInt(this.postTimeSlider?.value || 100);
        const volDepth = parseInt(this.volDepthSlider?.value || 100) / 100.0;  // 0-1 (default: 1.0 = no duck)

        // Filter freq: 0 = off, else map to Hz
        const filterFreqRaw = parseInt(this.filterFreqSlider?.value || 0);
        const filterFreq = filterFreqRaw === 0 ? 0 : 100 * Math.pow(200, filterFreqRaw / 100);

        const filterDepth = parseInt(this.filterDepthSlider?.value || 0) / 100.0;  // 0-1
        const smearAmount = parseInt(this.smearAmountSlider?.value || 0) / 100.0;  // 0-1
        const smearAttack = parseInt(this.smearAttackSlider?.value || 10) / 100.0;  // 1-50% -> 0.01-0.5
        const smearLength = parseInt(this.smearLengthSlider?.value || 100) / 100.0;  // 25-200% -> 0.25-2.0

        console.log(`[XFADE] Sending: pre=${preTimeMs}ms post=${postTimeMs}ms vol=${volDepth} filterFreq=${filterFreq}Hz filterDepth=${filterDepth}`);

        try {
            await this.setCrossfadeParamsFn(preTimeMs, postTimeMs, volDepth, filterFreq, filterDepth, smearAmount, smearAttack, smearLength);
            this.saveSettings();  // Save on every change
        } catch (e) {
            console.error('[XFADE] Error sending params:', e);
        }
    }
}

// ============================================
// EFFECTS RACK CONTROLLER
// ============================================

class EffectsRackController {
    constructor() {
        this.rackOverview = document.getElementById('rack-overview');
        this.rackDetail = document.getElementById('rack-detail');
        this.backBtn = document.getElementById('rack-back-btn');
        this.currentEffect = null;

        // Pedal cards
        this.pedalCards = document.querySelectorAll('.pedal-card');

        // Effect detail views
        this.effectDetails = document.querySelectorAll('.effect-detail');

        this.setupEventListeners();
        console.log('[EffectsRack] Initialized');
    }

    setupEventListeners() {
        // Expand buttons on pedal cards -> show detail view
        const expandBtns = document.querySelectorAll('.pedal-expand-btn');
        expandBtns.forEach(btn => {
            btn.addEventListener('click', (e) => {
                e.stopPropagation();
                const effect = btn.dataset.effect;
                this.showDetail(effect);
            });
        });

        // Also make pedal names clickable to enter detail view
        const pedalCards = document.querySelectorAll('.pedal-card');
        pedalCards.forEach(card => {
            const nameEl = card.querySelector('.pedal-name');
            if (nameEl) {
                nameEl.style.cursor = 'pointer';
                nameEl.addEventListener('click', (e) => {
                    e.stopPropagation();
                    const effect = card.dataset.effect;
                    if (effect) {
                        this.showDetail(effect);
                    }
                });
            }
        });

        // Back button -> show overview
        if (this.backBtn) {
            this.backBtn.addEventListener('click', () => {
                this.showOverview();
            });
        }

        // LED clicks for bypass toggle
        this.setupBypassLEDs();
    }

    setupBypassLEDs() {
        // Use the same getNativeFunction wrapper that LofiController uses
        const setSaturationEnabledFn = getNativeFunction('setSaturationEnabled');
        const setDelayEnabledFn = getNativeFunction('setDelayEnabled');

        // Saturation LED toggle - both card and detail LEDs
        const satLeds = [
            document.getElementById('saturation-led'),
            document.getElementById('saturation-detail-led')
        ];
        satLeds.forEach(led => {
            if (led) {
                led.addEventListener('click', async (e) => {
                    e.stopPropagation();
                    // Toggle both LEDs in sync
                    const isActive = !led.classList.contains('active');
                    satLeds.forEach(l => l?.classList.toggle('active', isActive));
                    try {
                        await setSaturationEnabledFn(isActive);
                        console.log(`[SATURATION] ${isActive ? 'enabled' : 'disabled'}`);
                    } catch (err) {
                        console.error('Error toggling saturation:', err);
                    }
                });
            }
        });

        // Delay LED toggle - both card and detail LEDs
        const delayLeds = [
            document.getElementById('delay-led'),
            document.getElementById('delay-detail-led')
        ];
        delayLeds.forEach(led => {
            if (led) {
                led.addEventListener('click', async (e) => {
                    e.stopPropagation();
                    // Toggle both LEDs in sync
                    const isActive = !led.classList.contains('active');
                    delayLeds.forEach(l => l?.classList.toggle('active', isActive));
                    try {
                        await setDelayEnabledFn(isActive);
                        console.log(`[DELAY] ${isActive ? 'enabled' : 'disabled'}`);
                    } catch (err) {
                        console.error('Error toggling delay:', err);
                    }
                });
            }
        });

        // Lofi LED already handled by LofiController
    }

    showDetail(effect) {
        console.log(`[EffectsRack] Showing detail for: ${effect}`);
        this.currentEffect = effect;

        // Hide overview, show detail container
        if (this.rackOverview) this.rackOverview.classList.add('hidden');
        if (this.rackDetail) this.rackDetail.classList.remove('hidden');
        if (this.backBtn) this.backBtn.classList.remove('hidden');

        // Show only the selected effect detail
        this.effectDetails.forEach(detail => {
            if (detail.dataset.effect === effect) {
                detail.classList.remove('hidden');
            } else {
                detail.classList.add('hidden');
            }
        });

        // Reinitialize filter canvas when lofi detail is shown
        // (canvas needs to be visible to get correct dimensions)
        if (effect === 'lofi' && window.lofiController) {
            // Use setTimeout to ensure DOM has updated
            setTimeout(() => {
                window.lofiController.setupCanvas();
            }, 10);
        }

        // Reinitialize micro looper canvas when granular detail is shown
        if (effect === 'granular' && window.microLooperController) {
            setTimeout(() => {
                window.microLooperController.setupCanvas();
            }, 10);
        }
    }

    showOverview() {
        console.log('[EffectsRack] Showing overview');
        this.currentEffect = null;

        // Show overview, hide detail container
        if (this.rackOverview) this.rackOverview.classList.remove('hidden');
        if (this.rackDetail) this.rackDetail.classList.add('hidden');
        if (this.backBtn) this.backBtn.classList.add('hidden');
    }
}

// ============================================
// SUB BASS CONTROLLER
// ============================================

class SubBassController {
    constructor() {
        this.led = document.getElementById('subbass-led');
        this.enabled = false;

        this.setSubBassEnabledFn = getNativeFunction('setSubBassEnabled');
        this.getSubBassStateFn = getNativeFunction('getSubBassState');

        this.setupEvents();
        this.fetchInitialState();

        console.log('[SUBBASS] Controller initialized');
    }

    setupEvents() {
        if (this.led) {
            this.led.addEventListener('click', (e) => {
                e.stopPropagation();
                this.toggleEnabled();
            });
        }
    }

    async fetchInitialState() {
        try {
            const state = await this.getSubBassStateFn();
            if (state) {
                this.enabled = state.enabled === true;
                this.updateUI();
            }
        } catch (e) {
            console.log('Could not fetch initial sub bass state');
        }
    }

    async toggleEnabled() {
        this.enabled = !this.enabled;
        this.updateUI();
        try {
            await this.setSubBassEnabledFn(this.enabled);
            console.log(`[SUBBASS] ${this.enabled ? 'enabled' : 'disabled'}`);
        } catch (e) {
            console.error('Error toggling sub bass:', e);
            this.enabled = !this.enabled;
            this.updateUI();
        }
    }

    updateUI() {
        if (this.led) this.led.classList.toggle('active', this.enabled);
    }
}

// ============================================
// GRANULAR CONTROLLER (Micro Looper)
// ============================================

class GranularController {
    constructor() {
        // LEDs (card and detail)
        this.cardLed = document.getElementById('granular-led');
        this.detailLed = document.getElementById('granular-detail-led');

        // Visualization elements
        this.canvas = document.getElementById('granular-card-canvas');
        this.statusEl = document.getElementById('granular-status');
        this.ctx = this.canvas ? this.canvas.getContext('2d') : null;

        // State
        this.enabled = false;
        this.isPlaying = false;
        this.isOverdubbing = false;
        this.isFrozen = false;
        this.playheadPos = 0;
        this.recordPos = 0;
        this.bufferFill = 0;
        this.waveformData = [];

        // Native functions (micro looper)
        this.setMicroLooperEnabledFn = getNativeFunction('setMicroLooperEnabled');
        this.getMicroLooperStateFn = getNativeFunction('getMicroLooperState');
        this.getMicroLooperWaveformFn = getNativeFunction('getMicroLooperWaveform');
        this.microLooperPlayFn = getNativeFunction('microLooperPlay');

        // Visualization polling
        this.vizInterval = null;
        this.lastWaveformFetch = 0;

        this.setupEvents();
        this.fetchInitialState();
        this.startVisualization();

        console.log('[GRANULAR/MICRO] Controller initialized with visualization');
    }

    setupEvents() {
        // LED toggles
        const toggleEnabled = (e) => {
            e.stopPropagation();
            this.toggleEnabled();
        };

        if (this.cardLed) this.cardLed.addEventListener('click', toggleEnabled);
        if (this.detailLed) this.detailLed.addEventListener('click', toggleEnabled);
    }

    async fetchInitialState() {
        try {
            const state = await this.getMicroLooperStateFn();
            if (state) {
                this.enabled = state.enabled === true;
                this.isPlaying = state.isPlaying === true;
                this.isOverdubbing = state.isOverdubbing === true;
                this.isFrozen = state.isFrozen === true;
                this.playheadPos = state.playheadPos || 0;
                this.recordPos = state.recordPos || 0;
                this.bufferFill = state.bufferFill || 0;
                this.updateUI();
            }
        } catch (e) {
            console.log('Could not fetch initial micro looper state');
        }
    }

    startVisualization() {
        // Poll state and waveform periodically
        this.vizInterval = setInterval(() => this.updateVisualization(), 50);
    }

    async updateVisualization() {
        if (!this.enabled) {
            this.renderEmpty();
            return;
        }

        try {
            // Fetch state every frame
            const state = await this.getMicroLooperStateFn();
            if (state) {
                this.isPlaying = state.isPlaying === true;
                this.isOverdubbing = state.isOverdubbing === true;
                this.isFrozen = state.isFrozen === true;
                this.playheadPos = state.playheadPos || 0;
                this.recordPos = state.recordPos || 0;
                this.bufferFill = state.bufferFill || 0;
            }

            // Fetch waveform less frequently (every 200ms)
            const now = Date.now();
            if (now - this.lastWaveformFetch > 200) {
                const waveform = await this.getMicroLooperWaveformFn(48);
                if (waveform && waveform.length > 0) {
                    this.waveformData = waveform;
                }
                this.lastWaveformFetch = now;
            }

            this.renderVisualization();
            this.updateStatusText();
        } catch (e) {
            // Silently handle errors during visualization updates
        }
    }

    renderEmpty() {
        if (!this.ctx || !this.canvas) return;

        const dpr = window.devicePixelRatio || 1;
        const rect = this.canvas.getBoundingClientRect();
        this.canvas.width = rect.width * dpr;
        this.canvas.height = rect.height * dpr;
        this.ctx.scale(dpr, dpr);

        const w = rect.width;
        const h = rect.height;

        // Clear with dark background
        this.ctx.fillStyle = 'rgba(5, 5, 5, 1)';
        this.ctx.fillRect(0, 0, w, h);

        // Draw subtle grid lines
        this.ctx.strokeStyle = 'rgba(255, 255, 255, 0.05)';
        this.ctx.lineWidth = 1;
        for (let i = 0; i < 4; i++) {
            const x = (w / 4) * i;
            this.ctx.beginPath();
            this.ctx.moveTo(x, 0);
            this.ctx.lineTo(x, h);
            this.ctx.stroke();
        }
    }

    renderVisualization() {
        if (!this.ctx || !this.canvas) return;

        const dpr = window.devicePixelRatio || 1;
        const rect = this.canvas.getBoundingClientRect();
        this.canvas.width = rect.width * dpr;
        this.canvas.height = rect.height * dpr;
        this.ctx.scale(dpr, dpr);

        const w = rect.width;
        const h = rect.height;
        const centerY = h / 2;

        // Clear
        this.ctx.fillStyle = 'rgba(5, 5, 5, 1)';
        this.ctx.fillRect(0, 0, w, h);

        // Draw waveform if we have data and buffer has content
        if (this.waveformData.length > 0 && this.bufferFill > 0) {
            const numPoints = this.waveformData.length;
            const fillRatio = this.bufferFill;

            // Waveform gradient based on state
            let waveColor;
            if (this.isFrozen) {
                waveColor = 'rgba(96, 165, 250, 0.7)';  // Blue for frozen
            } else if (this.isOverdubbing) {
                waveColor = 'rgba(236, 72, 153, 0.8)';  // Pink for overdub
            } else if (this.isPlaying) {
                waveColor = 'rgba(74, 222, 128, 0.7)';  // Green for playing
            } else {
                waveColor = 'rgba(168, 85, 247, 0.6)';  // Purple (granular accent) default
            }

            // Draw filled waveform
            this.ctx.beginPath();
            this.ctx.moveTo(0, centerY);

            for (let i = 0; i < numPoints; i++) {
                const x = (i / numPoints) * w * fillRatio;
                const amplitude = this.waveformData[i] * (h * 0.8);
                this.ctx.lineTo(x, centerY - amplitude / 2);
            }

            // Mirror bottom half
            for (let i = numPoints - 1; i >= 0; i--) {
                const x = (i / numPoints) * w * fillRatio;
                const amplitude = this.waveformData[i] * (h * 0.8);
                this.ctx.lineTo(x, centerY + amplitude / 2);
            }

            this.ctx.closePath();
            this.ctx.fillStyle = waveColor;
            this.ctx.fill();

            // Draw center line
            this.ctx.strokeStyle = 'rgba(255, 255, 255, 0.1)';
            this.ctx.lineWidth = 1;
            this.ctx.beginPath();
            this.ctx.moveTo(0, centerY);
            this.ctx.lineTo(w * fillRatio, centerY);
            this.ctx.stroke();
        }

        // Draw playhead position
        if (this.bufferFill > 0) {
            const playX = this.playheadPos * w * this.bufferFill;

            // Playhead glow
            const gradient = this.ctx.createLinearGradient(playX - 4, 0, playX + 4, 0);
            gradient.addColorStop(0, 'rgba(255, 255, 255, 0)');
            gradient.addColorStop(0.5, 'rgba(255, 255, 255, 0.8)');
            gradient.addColorStop(1, 'rgba(255, 255, 255, 0)');

            this.ctx.fillStyle = gradient;
            this.ctx.fillRect(playX - 4, 0, 8, h);

            // Playhead line
            this.ctx.strokeStyle = '#ffffff';
            this.ctx.lineWidth = 2;
            this.ctx.beginPath();
            this.ctx.moveTo(playX, 0);
            this.ctx.lineTo(playX, h);
            this.ctx.stroke();
        }

        // Draw record position if overdubbing
        if (this.isOverdubbing && this.bufferFill > 0) {
            const recX = this.recordPos * w * this.bufferFill;

            this.ctx.strokeStyle = 'rgba(236, 72, 153, 0.9)';
            this.ctx.lineWidth = 2;
            this.ctx.setLineDash([2, 2]);
            this.ctx.beginPath();
            this.ctx.moveTo(recX, 0);
            this.ctx.lineTo(recX, h);
            this.ctx.stroke();
            this.ctx.setLineDash([]);
        }
    }

    updateStatusText() {
        if (!this.statusEl) return;

        // Clear all state classes
        this.statusEl.classList.remove('recording', 'playing', 'frozen');

        if (!this.enabled) {
            this.statusEl.textContent = 'OFF';
            return;
        }

        if (this.isFrozen) {
            this.statusEl.textContent = 'FROZEN';
            this.statusEl.classList.add('frozen');
        } else if (this.isOverdubbing) {
            this.statusEl.textContent = 'OVERDUB';
            this.statusEl.classList.add('recording');
        } else if (this.isPlaying) {
            this.statusEl.textContent = 'PLAY';
            this.statusEl.classList.add('playing');
        } else if (this.bufferFill > 0) {
            this.statusEl.textContent = 'READY';
        } else {
            this.statusEl.textContent = 'EMPTY';
        }
    }

    async toggleEnabled() {
        this.enabled = !this.enabled;
        this.updateUI();
        try {
            await this.setMicroLooperEnabledFn(this.enabled);
            console.log(`[GRANULAR/MICRO] ${this.enabled ? 'enabled' : 'disabled'}`);

            // When enabling via card LED, also start playback/recording immediately
            if (this.enabled) {
                await this.microLooperPlayFn();
                console.log('[GRANULAR/MICRO] Auto-started recording');
            }
        } catch (e) {
            console.error('Error toggling micro looper:', e);
            this.enabled = !this.enabled;
            this.updateUI();
        }
    }

    updateUI() {
        // Update LEDs
        if (this.cardLed) this.cardLed.classList.toggle('active', this.enabled);
        if (this.detailLed) this.detailLed.classList.toggle('active', this.enabled);

        // Update status when toggled off
        if (!this.enabled) {
            this.updateStatusText();
            this.renderEmpty();
        }
    }

    destroy() {
        if (this.vizInterval) {
            clearInterval(this.vizInterval);
            this.vizInterval = null;
        }
    }
}

// ============================================
// REVERB CONTROLLER
// ============================================

class ReverbController {
    constructor() {
        // Algorithm type: 0=SPRING, 1=PLATE, 2=HALL
        this.currentType = 0;
        this.typeNames = ['SPRING', 'PLATE', 'HALL'];

        // LEDs (card and detail)
        this.cardLed = document.getElementById('reverb-led');
        this.detailLed = document.getElementById('reverb-detail-led');

        // Type selector buttons
        this.typeButtons = document.querySelectorAll('.reverb-type-btn');

        // Card type indicator
        this.cardTypeIndicator = document.getElementById('reverb-type-card');

        // State
        this.enabled = false;

        // Native functions
        this.setReverbEnabledFn = getNativeFunction('setReverbEnabled');
        this.setReverbTypeFn = getNativeFunction('setReverbType');
        this.getReverbStateFn = getNativeFunction('getReverbState');

        this.setupEvents();
        this.setupKnobs();
        this.fetchInitialState();

        console.log('[REVERB] Controller initialized');
    }

    setupEvents() {
        // LED toggles
        const toggleEnabled = (e) => {
            e.stopPropagation();
            this.toggleEnabled();
        };

        if (this.cardLed) this.cardLed.addEventListener('click', toggleEnabled);
        if (this.detailLed) this.detailLed.addEventListener('click', toggleEnabled);

        // Type selector buttons
        this.typeButtons.forEach(btn => {
            btn.addEventListener('click', () => {
                const type = parseInt(btn.dataset.type);
                this.setType(type);
            });
        });
    }

    setupKnobs() {
        // Mix knobs (card and detail synced)
        new KnobController('reverbMix-knob', 'reverbMix', {
            formatValue: (v) => `${Math.round(v * 100)}%`
        });
        new KnobController('reverbMix-detail-knob', 'reverbMix', {
            formatValue: (v) => `${Math.round(v * 100)}%`,
            valueElementId: 'reverbMix-detail-value'
        });

        // Card knobs (no value display)
        new KnobController('reverbDecay-card-knob', 'reverbDecay', {
            formatValue: () => ''
        });
        new KnobController('reverbSize-card-knob', 'reverbSize', {
            formatValue: () => ''
        });

        // Detail panel knobs
        new KnobController('reverbSize-knob', 'reverbSize', {
            formatValue: (v) => `${Math.round(v * 100)}%`
        });
        new KnobController('reverbDecay-knob', 'reverbDecay', {
            formatValue: (v) => `${Math.round(v * 100)}%`
        });
        new KnobController('reverbDamp-knob', 'reverbDamp', {
            formatValue: (v) => `${Math.round(v * 100)}%`
        });
        new KnobController('reverbPreDelay-knob', 'reverbPreDelay', {
            formatValue: (v) => `${Math.round(v * 100)}%`
        });
        new KnobController('reverbWidth-knob', 'reverbWidth', {
            formatValue: (v) => `${Math.round(v * 100)}%`
        });
        new KnobController('reverbModRate-knob', 'reverbModRate', {
            formatValue: (v) => `${Math.round(v * 100)}%`
        });
        new KnobController('reverbModDepth-knob', 'reverbModDepth', {
            formatValue: (v) => `${Math.round(v * 100)}%`
        });
    }

    async fetchInitialState() {
        try {
            const state = await this.getReverbStateFn();
            if (state) {
                this.enabled = state.enabled === true;
                this.currentType = state.type || 0;
                this.updateUI();
            }
        } catch (e) {
            console.log('Could not fetch initial reverb state');
        }
    }

    async toggleEnabled() {
        this.enabled = !this.enabled;
        this.updateUI();
        try {
            await this.setReverbEnabledFn(this.enabled);
            console.log(`[REVERB] ${this.enabled ? 'enabled' : 'disabled'}`);
        } catch (e) {
            console.error('Error toggling reverb:', e);
            this.enabled = !this.enabled;
            this.updateUI();
        }
    }

    async setType(type) {
        this.currentType = type;
        this.updateTypeUI();
        try {
            await this.setReverbTypeFn(type);
            console.log(`[REVERB] Type set to ${this.typeNames[type]}`);
        } catch (e) {
            console.error('Error setting reverb type:', e);
        }
    }

    updateUI() {
        // Update LEDs
        if (this.cardLed) this.cardLed.classList.toggle('active', this.enabled);
        if (this.detailLed) this.detailLed.classList.toggle('active', this.enabled);
        this.updateTypeUI();
    }

    updateTypeUI() {
        // Update type buttons
        this.typeButtons.forEach(btn => {
            const type = parseInt(btn.dataset.type);
            btn.classList.toggle('active', type === this.currentType);
        });

        // Update card indicator
        if (this.cardTypeIndicator) {
            this.cardTypeIndicator.textContent = this.typeNames[this.currentType];
        }
    }
}

// ============================================
// SATURATION CONTROLLER
// ============================================

class SaturationController {
    constructor() {
        this.currentType = 0;  // 0=SOFT, 1=TAPE, 2=TUBE, 3=FUZZ
        this.typeNames = ['SOFT', 'TAPE', 'TUBE', 'FUZZ'];

        // Type selector buttons
        this.typeButtons = document.querySelectorAll('.sat-type-btn');

        // Type params containers
        this.typeParams = {
            soft: document.getElementById('sat-soft-params'),
            tape: document.getElementById('sat-tape-params'),
            tube: document.getElementById('sat-tube-params'),
            fuzz: document.getElementById('sat-fuzz-params')
        };

        // Card type indicator
        this.cardTypeIndicator = document.getElementById('sat-type-card');

        // Get native functions
        this.getSaturationStateFn = null;
        this.setSaturationTypeFn = null;

        this.setupNativeFunctions();
        this.setupEventListeners();
        this.setupKnobs();
        this.pollState();

        console.log('[Saturation] Initialized');
    }

    setupNativeFunctions() {
        // Use the same getNativeFunction wrapper that works for Lofi
        this.getSaturationStateFn = getNativeFunction('getSaturationState');
        this.setSaturationTypeFn = getNativeFunction('setSaturationType');
    }

    setupEventListeners() {
        // Type selector buttons
        this.typeButtons.forEach(btn => {
            btn.addEventListener('click', () => {
                const type = parseInt(btn.dataset.type);
                this.setType(type);
            });
        });
    }

    setupKnobs() {
        // Auto-enable saturation when any card knob is turned
        const autoEnableSat = () => this.enableSaturation();

        // Saturation Mix - card micro knob
        this.satMixCardKnob = new KnobController('satMix-knob', 'satMix', {
            formatValue: (v) => `${Math.round(v * 100)}%`,
            onTurn: autoEnableSat
        });

        // Saturation Mix - detail knob (synced with card)
        this.satMixDetailKnob = new KnobController('satMix-detail-knob', 'satMix', {
            formatValue: (v) => `${Math.round(v * 100)}%`,
            valueElementId: 'satMix-detail-value'
        });

        // Card drive knob - controls whichever type is active
        // This is a "virtual" knob that maps to the current type's drive
        this.setupCardDriveKnob();

        // SOFT type knobs
        new KnobController('satSoftDrive-knob', 'satSoftDrive', {
            formatValue: (v) => `${Math.round(v * 100)}%`
        });
        new KnobController('satSoftTone-knob', 'satSoftTone', {
            formatValue: (v) => `${Math.round(v * 100)}%`
        });
        new KnobController('satSoftCurve-knob', 'satSoftCurve', {
            formatValue: (v) => `${Math.round(v * 100)}%`
        });

        // TAPE type knobs
        new KnobController('satTapeDrive-knob', 'satTapeDrive', {
            formatValue: (v) => `${Math.round(v * 100)}%`
        });
        new KnobController('satTapeBias-knob', 'satTapeBias', {
            formatValue: (v) => `${Math.round(v * 100)}%`
        });
        new KnobController('satTapeFlutter-knob', 'satTapeFlutter', {
            formatValue: (v) => `${Math.round(v * 100)}%`
        });
        new KnobController('satTapeTone-knob', 'satTapeTone', {
            formatValue: (v) => `${Math.round(v * 100)}%`
        });

        // TUBE type knobs
        new KnobController('satTubeDrive-knob', 'satTubeDrive', {
            formatValue: (v) => `${Math.round(v * 100)}%`
        });
        new KnobController('satTubeBias-knob', 'satTubeBias', {
            formatValue: (v) => `${Math.round(v * 100)}%`
        });
        new KnobController('satTubeWarmth-knob', 'satTubeWarmth', {
            formatValue: (v) => `${Math.round(v * 100)}%`
        });
        new KnobController('satTubeSag-knob', 'satTubeSag', {
            formatValue: (v) => `${Math.round(v * 100)}%`
        });

        // FUZZ type knobs
        new KnobController('satFuzzDrive-knob', 'satFuzzDrive', {
            formatValue: (v) => `${Math.round(v * 100)}%`
        });
        new KnobController('satFuzzGate-knob', 'satFuzzGate', {
            formatValue: (v) => `${Math.round(v * 100)}%`
        });
        new KnobController('satFuzzOctave-knob', 'satFuzzOctave', {
            formatValue: (v) => `${Math.round(v * 100)}%`
        });
        new KnobController('satFuzzTone-knob', 'satFuzzTone', {
            formatValue: (v) => `${Math.round(v * 100)}%`
        });
    }

    async setType(type) {
        this.currentType = type;
        console.log(`[Saturation] Setting type to ${this.typeNames[type]} (${type})`);

        // Update UI
        this.updateTypeUI();

        // Send to plugin
        if (this.setSaturationTypeFn) {
            console.log(`[Saturation] Calling setSaturationTypeFn with type=${type}`);
            try {
                await this.setSaturationTypeFn(type);
                console.log(`[Saturation] setSaturationTypeFn completed successfully`);
            } catch (err) {
                console.error(`[Saturation] setSaturationTypeFn failed:`, err);
            }
        } else {
            console.error(`[Saturation] setSaturationTypeFn is not defined!`);
        }
    }

    updateTypeUI() {
        // Update type buttons
        this.typeButtons.forEach(btn => {
            const btnType = parseInt(btn.dataset.type);
            btn.classList.toggle('active', btnType === this.currentType);
        });

        // Show/hide type params
        Object.keys(this.typeParams).forEach((key, index) => {
            if (this.typeParams[key]) {
                this.typeParams[key].classList.toggle('hidden', index !== this.currentType);
            }
        });

        // Update card type indicator
        if (this.cardTypeIndicator) {
            this.cardTypeIndicator.textContent = this.typeNames[this.currentType];
        }

        // Update card drive knob to match current type's drive value
        this.updateCardDriveKnob();
    }

    // Enable saturation effect (called when card knobs are turned)
    async enableSaturation() {
        const satLeds = [
            document.getElementById('saturation-led'),
            document.getElementById('saturation-detail-led')
        ];
        // Check if already enabled
        if (satLeds[0]?.classList.contains('active')) return;

        // Enable it
        satLeds.forEach(l => l?.classList.add('active'));
        if (window.__JUCE__?.backend?.getNativeFunction) {
            const setSatEnabled = window.__JUCE__.backend.getNativeFunction('setSaturationEnabled');
            if (setSatEnabled) await setSatEnabled(true);
        }
    }

    // Setup the card drive knob (maps to current type's drive)
    setupCardDriveKnob() {
        const driveKnob = document.getElementById('satDrive-card-knob');
        if (!driveKnob) return;

        const indicator = driveKnob.querySelector('.knob-dot');
        let isDragging = false;
        let lastY = 0;

        const updateIndicator = (value) => {
            const angle = -135 + 270 * value;
            if (indicator) {
                indicator.style.transform = `translateX(-50%) rotate(${angle}deg)`;
            }
        };

        driveKnob.addEventListener('mousedown', (e) => {
            e.stopPropagation();  // Prevent card click
            isDragging = true;
            lastY = e.clientY;
            driveKnob.classList.add('active');
        });

        document.addEventListener('mousemove', (e) => {
            if (!isDragging) return;
            const deltaY = lastY - e.clientY;
            const sensitivity = 0.005;

            // Get current drive param based on type
            const driveParam = this.getDriveParamName();
            const sliderState = getSliderState(driveParam);
            if (sliderState) {
                let newValue = sliderState.getNormalisedValue() + deltaY * sensitivity;
                newValue = Math.max(0, Math.min(1, newValue));
                sliderState.setNormalisedValue(newValue);
                updateIndicator(newValue);
            }

            // Auto-enable saturation
            this.enableSaturation();

            lastY = e.clientY;
        });

        document.addEventListener('mouseup', () => {
            if (isDragging) {
                isDragging = false;
                driveKnob.classList.remove('active');
            }
        });

        // Prevent click propagation after drag
        driveKnob.addEventListener('click', (e) => {
            e.stopPropagation();
        });

        // Store reference for updating
        this.cardDriveKnob = driveKnob;
        this.cardDriveIndicator = indicator;
    }

    // Get the drive parameter name for the current type
    getDriveParamName() {
        const driveParams = ['satSoftDrive', 'satTapeDrive', 'satTubeDrive', 'satFuzzDrive'];
        return driveParams[this.currentType] || 'satSoftDrive';
    }

    // Update card drive knob to match current type's value
    updateCardDriveKnob() {
        if (!this.cardDriveIndicator) return;
        const driveParam = this.getDriveParamName();
        const sliderState = getSliderState(driveParam);
        if (sliderState) {
            const value = sliderState.getNormalisedValue();
            const angle = -135 + 270 * value;
            this.cardDriveIndicator.style.transform = `translateX(-50%) rotate(${angle}deg)`;
        }
    }

    async pollState() {
        if (!this.getSaturationStateFn) return;

        try {
            const state = await this.getSaturationStateFn();
            if (state) {
                // Update enabled state
                const satLed = document.getElementById('saturation-led');
                const satDetailLed = document.getElementById('saturation-detail-led');
                if (satLed) satLed.classList.toggle('active', state.enabled);
                if (satDetailLed) satDetailLed.classList.toggle('active', state.enabled);

                // Update type if changed externally
                if (state.type !== this.currentType) {
                    this.currentType = state.type;
                    this.updateTypeUI();
                }
            }
        } catch (e) {
            console.error('[Saturation] Error polling state:', e);
        }

        // Poll periodically
        setTimeout(() => this.pollState(), 200);
    }
}

// ============================================
// MIXER VIEW CONTROLLER
// Vintage-style mixing console for layer mixing
// ============================================
class MixerViewController {
    constructor() {
        // View toggle elements
        this.waveformViewBtn = document.getElementById('waveform-view-btn');
        this.mixerViewBtn = document.getElementById('mixer-view-btn');
        this.waveformView = document.getElementById('waveform-view');
        this.mixerView = document.getElementById('mixer-view');
        this.viewLabel = document.getElementById('view-label');

        // Current view state
        this.currentView = 'waveform';

        // Layer colors for visual consistency
        this.layerColors = [
            '#4fc3f7',  // Layer 1: Cyan/Light blue
            '#ff7043',  // Layer 2: Deep orange
            '#66bb6a',  // Layer 3: Green
            '#ab47bc',  // Layer 4: Purple
            '#ffa726',  // Layer 5: Orange
            '#26c6da',  // Layer 6: Teal
            '#ec407a',  // Layer 7: Pink
            '#9ccc65',  // Layer 8: Light green
        ];

        // Per-channel state
        this.channels = [];
        for (let i = 1; i <= 8; i++) {
            this.channels.push({
                layer: i,
                fader: document.getElementById(`fader-${i}`),
                faderMeter: document.getElementById(`fader-meter-${i}`),
                panKnob: document.getElementById(`pan-knob-${i}`),
                panIndicator: document.querySelector(`#pan-knob-${i} .pan-indicator`),
                muteBtn: document.getElementById(`mute-${i}`),
                soloBtn: document.getElementById(`solo-${i}`),
                vuNeedle: document.getElementById(`vu-needle-${i}`),
                channelEl: document.querySelector(`.mixer-channel[data-layer="${i}"]`),
                // EQ knobs
                eqHighKnob: document.getElementById(`eq-high-${i}`),
                eqMidKnob: document.getElementById(`eq-mid-${i}`),
                eqLowKnob: document.getElementById(`eq-low-${i}`),
                eqHighIndicator: document.querySelector(`#eq-high-${i} .eq-indicator`),
                eqMidIndicator: document.querySelector(`#eq-mid-${i} .eq-indicator`),
                eqLowIndicator: document.querySelector(`#eq-low-${i} .eq-indicator`),
                // Loop controls
                loopStartSlider: document.getElementById(`loop-start-${i}`),
                loopEndSlider: document.getElementById(`loop-end-${i}`),
                reverseBtn: document.getElementById(`reverse-${i}`),
                // State
                volume: 1.0,
                pan: 0,
                muted: false,
                solo: false,
                hasContent: false,
                panDragging: false,
                panLastY: 0,
                // EQ state (in dB, -12 to +12)
                eqLow: 0,
                eqMid: 0,
                eqHigh: 0,
                eqLowDragging: false,
                eqMidDragging: false,
                eqHighDragging: false,
                eqLastY: 0,
                // Loop state
                loopStart: 0,
                loopEnd: 1,
                reversed: false
            });
        }

        // BUS channel VU meter
        this.busVuNeedle = document.getElementById('vu-needle-bus');
        this.busFaderMeter = document.getElementById('fader-meter-bus');

        // Native functions
        this.setLayerVolumeFn = getNativeFunction('setLayerVolume');
        this.setLayerPanFn = getNativeFunction('setLayerPan');
        this.setLayerMutedFn = getNativeFunction('setLayerMuted');
        this.getLayerContentFn = getNativeFunction('getLayerContentStates');
        this.getLayerLevelsFn = getNativeFunction('getLayerLevels');
        // Per-layer EQ, loop bounds, reverse
        this.setLayerEQLowFn = getNativeFunction('setLayerEQLow');
        this.setLayerEQMidFn = getNativeFunction('setLayerEQMid');
        this.setLayerEQHighFn = getNativeFunction('setLayerEQHigh');
        this.setLayerLoopStartFn = getNativeFunction('setLayerLoopStart');
        this.setLayerLoopEndFn = getNativeFunction('setLayerLoopEnd');
        this.setLayerReverseFn = getNativeFunction('setLayerReverse');

        // Soloed layer tracking
        this.soloedLayer = null;

        this.setupViewToggle();
        this.setupChannelControls();
        this.startMeterPolling();
        this.setupFaderSizing();

        console.log('[Mixer] Initialized');
    }

    // Dynamically size faders based on container height
    setupFaderSizing() {
        const resizeFaders = () => {
            const faderContainers = document.querySelectorAll('.channel-fader');
            faderContainers.forEach(container => {
                const height = container.clientHeight;
                // Set the fader length (width before rotation) to match container height minus padding
                const faderLength = Math.max(100, height - 20);
                container.style.setProperty('--fader-length', `${faderLength}px`);
            });
        };

        // Initial sizing after a short delay to ensure layout is complete
        setTimeout(resizeFaders, 100);

        // Resize on window resize
        window.addEventListener('resize', resizeFaders);
    }

    setupViewToggle() {
        if (this.waveformViewBtn) {
            this.waveformViewBtn.addEventListener('click', () => this.showWaveformView());
        }
        if (this.mixerViewBtn) {
            this.mixerViewBtn.addEventListener('click', () => this.showMixerView());
        }
    }

    showWaveformView() {
        this.currentView = 'waveform';
        if (this.waveformView) this.waveformView.classList.remove('hidden');
        if (this.mixerView) this.mixerView.classList.add('hidden');
        if (this.waveformViewBtn) this.waveformViewBtn.classList.add('active');
        if (this.mixerViewBtn) this.mixerViewBtn.classList.remove('active');
        if (this.viewLabel) this.viewLabel.textContent = 'WAVEFORM';

        // Trigger waveform canvas resize
        window.dispatchEvent(new Event('resize'));
    }

    showMixerView() {
        this.currentView = 'mixer';
        if (this.waveformView) this.waveformView.classList.add('hidden');
        if (this.mixerView) this.mixerView.classList.remove('hidden');
        if (this.waveformViewBtn) this.waveformViewBtn.classList.remove('active');
        if (this.mixerViewBtn) this.mixerViewBtn.classList.add('active');
        if (this.viewLabel) this.viewLabel.textContent = 'MIXER';

        // Hide layer control panel in mixer view
        const layerPanel = document.getElementById('layer-control-panel');
        if (layerPanel) layerPanel.classList.add('hidden');

        // Sync mixer state from looper controller layer buttons
        this.syncFromLayerButtons();

        // Resize faders after view becomes visible
        setTimeout(() => {
            const faderContainers = document.querySelectorAll('.channel-fader');
            faderContainers.forEach(container => {
                const height = container.clientHeight;
                const faderLength = Math.max(100, height - 20);
                container.style.setProperty('--fader-length', `${faderLength}px`);
            });
        }, 50);
    }

    // Sync mixer channel states from the layer indicator buttons
    syncFromLayerButtons() {
        const layerBtns = document.querySelectorAll('.layer-btn');
        layerBtns.forEach((btn, idx) => {
            const channel = this.channels[idx];
            if (channel && channel.channelEl) {
                const hasContent = btn.classList.contains('has-content');
                const isMuted = btn.classList.contains('muted');
                const isOverride = btn.classList.contains('override-layer');

                channel.hasContent = hasContent;
                channel.muted = isMuted;
                channel.isOverride = isOverride;
                channel.channelEl.classList.toggle('has-content', hasContent);
                channel.channelEl.classList.toggle('muted', isMuted);
                channel.channelEl.classList.toggle('override-layer', isOverride);
                if (channel.muteBtn) {
                    channel.muteBtn.classList.toggle('active', isMuted);
                }
            }
        });
    }

    setupChannelControls() {
        this.channels.forEach((channel, idx) => {
            const layerNum = idx + 1;

            // Fader control
            // Fader range: 0-127 where 100 = 0dB (unity), 127 = +3dB, 0 = -inf
            if (channel.fader) {
                channel.fader.addEventListener('input', async (e) => {
                    const faderValue = parseInt(e.target.value);
                    const vol = this.faderToVolume(faderValue);
                    channel.volume = vol;
                    try {
                        await this.setLayerVolumeFn(layerNum, vol);
                    } catch (err) {
                        console.error(`[Mixer] Error setting layer ${layerNum} volume:`, err);
                    }
                });
            }

            // Pan knob control
            if (channel.panKnob) {
                channel.panKnob.addEventListener('mousedown', (e) => {
                    channel.panDragging = true;
                    channel.panLastY = e.clientY;
                    channel.panKnob.classList.add('active');
                    e.preventDefault();
                });
            }

            // Mute button
            if (channel.muteBtn) {
                channel.muteBtn.addEventListener('click', async () => {
                    channel.muted = !channel.muted;
                    channel.muteBtn.classList.toggle('active', channel.muted);
                    if (channel.channelEl) {
                        channel.channelEl.classList.toggle('muted', channel.muted);
                    }
                    try {
                        await this.setLayerMutedFn(layerNum, channel.muted);
                        // Sync to layer buttons
                        this.syncMuteToLayerButton(layerNum, channel.muted);
                    } catch (err) {
                        console.error(`[Mixer] Error setting layer ${layerNum} mute:`, err);
                    }
                });
            }

            // Solo button
            if (channel.soloBtn) {
                channel.soloBtn.addEventListener('click', async () => {
                    await this.toggleSolo(layerNum);
                });
            }

            // EQ knobs (drag up/down)
            ['Low', 'Mid', 'High'].forEach(band => {
                const knob = channel[`eq${band}Knob`];
                if (knob) {
                    knob.addEventListener('mousedown', (e) => {
                        channel[`eq${band}Dragging`] = true;
                        channel.eqLastY = e.clientY;
                        e.preventDefault();
                    });
                    // Double-click to reset
                    knob.addEventListener('dblclick', async () => {
                        channel[`eq${band.toLowerCase()}`] = 0;
                        this.updateEQKnobUI(channel, band.toLowerCase());
                        try {
                            await this[`setLayerEQ${band}Fn`](layerNum, 0);
                        } catch (err) {
                            console.error(`[Mixer] Error resetting EQ ${band}:`, err);
                        }
                    });
                }
            });

            // Loop start/end sliders
            if (channel.loopStartSlider) {
                channel.loopStartSlider.addEventListener('input', async (e) => {
                    const val = parseInt(e.target.value) / 100;
                    channel.loopStart = val;
                    try {
                        await this.setLayerLoopStartFn(layerNum, val);
                    } catch (err) {
                        console.error(`[Mixer] Error setting loop start:`, err);
                    }
                });
            }

            if (channel.loopEndSlider) {
                channel.loopEndSlider.addEventListener('input', async (e) => {
                    const val = parseInt(e.target.value) / 100;
                    channel.loopEnd = val;
                    try {
                        await this.setLayerLoopEndFn(layerNum, val);
                    } catch (err) {
                        console.error(`[Mixer] Error setting loop end:`, err);
                    }
                });
            }

            // Reverse button
            if (channel.reverseBtn) {
                channel.reverseBtn.addEventListener('click', async () => {
                    channel.reversed = !channel.reversed;
                    channel.reverseBtn.classList.toggle('active', channel.reversed);
                    try {
                        await this.setLayerReverseFn(layerNum, channel.reversed);
                    } catch (err) {
                        console.error(`[Mixer] Error setting reverse:`, err);
                    }
                });
            }
        });

        // Global mouse events for pan knobs and EQ knobs
        document.addEventListener('mousemove', (e) => {
            this.channels.forEach((channel, idx) => {
                // Pan knob dragging
                if (channel.panDragging) {
                    const deltaY = channel.panLastY - e.clientY;
                    const sensitivity = 2;
                    channel.pan = Math.max(-100, Math.min(100, channel.pan + deltaY * sensitivity));
                    this.updatePanKnobUI(channel);
                    this.sendPanToBackend(idx + 1, channel.pan);
                    channel.panLastY = e.clientY;
                }

                // EQ knob dragging
                ['low', 'mid', 'high'].forEach(band => {
                    const bandCap = band.charAt(0).toUpperCase() + band.slice(1);
                    if (channel[`eq${bandCap}Dragging`]) {
                        const deltaY = channel.eqLastY - e.clientY;
                        const sensitivity = 0.2;  // dB per pixel
                        channel[`eq${band}`] = Math.max(-12, Math.min(12, channel[`eq${band}`] + deltaY * sensitivity));
                        this.updateEQKnobUI(channel, band);
                        this.sendEQToBackend(idx + 1, band, channel[`eq${band}`]);
                        channel.eqLastY = e.clientY;
                    }
                });
            });
        });

        document.addEventListener('mouseup', () => {
            this.channels.forEach(channel => {
                if (channel.panDragging) {
                    channel.panDragging = false;
                    if (channel.panKnob) channel.panKnob.classList.remove('active');
                }
                // Reset EQ dragging
                channel.eqLowDragging = false;
                channel.eqMidDragging = false;
                channel.eqHighDragging = false;
            });
        });
    }

    updatePanKnobUI(channel) {
        if (!channel.panIndicator) return;
        // Pan goes from -100 to 100, map to -60 to 60 degrees (smaller range for mini knob)
        const angle = (channel.pan / 100) * 60;
        channel.panIndicator.style.transform = `translateX(-50%) rotate(${angle}deg)`;
    }

    async sendPanToBackend(layerNum, panValue) {
        const panNorm = panValue / 100;  // -1 to 1
        try {
            await this.setLayerPanFn(layerNum, panNorm);
        } catch (err) {
            console.error(`[Mixer] Error setting layer ${layerNum} pan:`, err);
        }
    }

    updateEQKnobUI(channel, band) {
        const indicator = channel[`eq${band.charAt(0).toUpperCase() + band.slice(1)}Indicator`];
        if (!indicator) return;
        // EQ goes from -12 to +12 dB, map to -135 to +135 degrees
        const angle = (channel[`eq${band}`] / 12) * 135;
        indicator.style.transform = `translateX(-50%) rotate(${angle}deg)`;
    }

    async sendEQToBackend(layerNum, band, valueDB) {
        const bandCap = band.charAt(0).toUpperCase() + band.slice(1);
        const fn = this[`setLayerEQ${bandCap}Fn`];
        if (!fn) return;
        try {
            await fn(layerNum, valueDB);
        } catch (err) {
            console.error(`[Mixer] Error setting layer ${layerNum} EQ ${band}:`, err);
        }
    }

    async toggleSolo(targetLayer) {
        const channel = this.channels[targetLayer - 1];
        const wasAlreadySolo = this.soloedLayer === targetLayer;

        if (wasAlreadySolo) {
            // Unsolo: unmute all layers
            this.soloedLayer = null;
            for (let i = 0; i < this.channels.length; i++) {
                const ch = this.channels[i];
                ch.muted = false;
                ch.solo = false;
                if (ch.muteBtn) ch.muteBtn.classList.remove('active');
                if (ch.soloBtn) ch.soloBtn.classList.remove('active');
                if (ch.channelEl) {
                    ch.channelEl.classList.remove('muted');
                    ch.channelEl.classList.remove('solo');
                }
                try {
                    await this.setLayerMutedFn(i + 1, false);
                    this.syncMuteToLayerButton(i + 1, false);
                    this.syncSoloToLayerButton(i + 1, false);
                } catch (err) { /* ignore */ }
            }
        } else {
            // Solo: mute all except target
            this.soloedLayer = targetLayer;
            for (let i = 0; i < this.channels.length; i++) {
                const ch = this.channels[i];
                const layerNum = i + 1;

                if (layerNum === targetLayer) {
                    ch.muted = false;
                    ch.solo = true;
                    if (ch.muteBtn) ch.muteBtn.classList.remove('active');
                    if (ch.soloBtn) ch.soloBtn.classList.add('active');
                    if (ch.channelEl) {
                        ch.channelEl.classList.remove('muted');
                        ch.channelEl.classList.add('solo');
                    }
                } else {
                    ch.muted = true;
                    ch.solo = false;
                    if (ch.muteBtn) ch.muteBtn.classList.add('active');
                    if (ch.soloBtn) ch.soloBtn.classList.remove('active');
                    if (ch.channelEl) {
                        ch.channelEl.classList.add('muted');
                        ch.channelEl.classList.remove('solo');
                    }
                }
                try {
                    await this.setLayerMutedFn(layerNum, ch.muted);
                    this.syncMuteToLayerButton(layerNum, ch.muted);
                    this.syncSoloToLayerButton(layerNum, ch.solo);
                } catch (err) { /* ignore */ }
            }
        }
    }

    syncMuteToLayerButton(layerNum, muted) {
        const layerBtn = document.querySelector(`.layer-btn[data-layer="${layerNum}"]`);
        if (layerBtn) {
            layerBtn.classList.toggle('muted', muted);
        }
    }

    syncSoloToLayerButton(layerNum, soloed) {
        const layerBtn = document.querySelector(`.layer-btn[data-layer="${layerNum}"]`);
        if (layerBtn) {
            layerBtn.classList.toggle('soloed', soloed);
        }
    }

    startMeterPolling() {
        // Poll for layer levels to update VU meters
        setInterval(async () => {
            if (this.currentView !== 'mixer') return;

            try {
                // Check layer content states
                if (this.getLayerContentFn) {
                    const contentStates = await this.getLayerContentFn();
                    if (contentStates && Array.isArray(contentStates)) {
                        contentStates.forEach((hasContent, idx) => {
                            const channel = this.channels[idx];
                            if (channel && channel.channelEl) {
                                channel.hasContent = hasContent;
                                channel.channelEl.classList.toggle('has-content', hasContent);
                            }
                        });
                    }
                }

                // Get layer audio levels for VU meters
                if (this.getLayerLevelsFn) {
                    const levels = await this.getLayerLevelsFn();
                    if (levels && Array.isArray(levels)) {
                        levels.forEach((level, idx) => {
                            this.updateVUMeter(idx, level);
                        });
                    }
                }

            } catch (e) {
                // Silently ignore polling errors
            }
        }, 50);  // 50ms for smooth VU meter animation
    }

    updateVUMeter(channelIdx, level) {
        const channel = this.channels[channelIdx];
        if (!channel || !channel.vuNeedle) return;

        // VU meter behavior:
        // - Needle rests at -45deg (left) when silent
        // - Swings right as level increases
        // - 0dB (level=1.0) is at approximately +5deg
        // - Into red (+3dB) at +25deg
        // - Max angle around +30deg

        // Convert linear level to dB
        const minDb = -48;  // Silence floor
        const maxDb = 6;    // Clip/hot zone
        let db = minDb;
        if (level > 0.00001) {
            db = 20 * Math.log10(level);
            db = Math.max(minDb, Math.min(maxDb, db));
        }

        // Map dB to angle using VU-style ballistics
        // VU meters have a logarithmic-ish response
        // -48dB -> -45deg (fully left)
        // -20dB -> -30deg
        // -10dB -> -15deg
        // -3dB  -> 0deg (unity reference)
        // 0dB   -> +5deg
        // +6dB  -> +25deg (into red)

        // Simple piecewise linear mapping
        let angle;
        if (db <= -48) {
            angle = -45;
        } else if (db <= -20) {
            // -48dB to -20dB: -45deg to -25deg
            angle = -45 + (db + 48) * (20 / 28);
        } else if (db <= -3) {
            // -20dB to -3dB: -25deg to 0deg
            angle = -25 + (db + 20) * (25 / 17);
        } else if (db <= 0) {
            // -3dB to 0dB: 0deg to +8deg
            angle = (db + 3) * (8 / 3);
        } else {
            // 0dB to +6dB: +8deg to +25deg
            angle = 8 + db * (17 / 6);
        }

        channel.vuNeedle.style.transform = `translateX(-50%) rotate(${angle}deg)`;

        // Also update fader meter bar
        // Map level to percentage height (0-100%)
        // Use a similar dB scale: -48dB = 0%, 0dB = ~79% (fader pos 100), +3dB = 100%
        if (channel.faderMeter) {
            let meterHeight;
            if (level <= 0.00001) {
                meterHeight = 0;
            } else {
                // Convert linear to percentage based on fader scale
                // Our fader: 0-100 = -inf to 0dB, 100-127 = 0dB to +3dB
                // So 0dB (level=1.0) is at ~79% height (100/127)
                // +3dB (level=1.41) is at 100%
                const dbLevel = 20 * Math.log10(level);

                if (dbLevel <= -48) {
                    meterHeight = 0;
                } else if (dbLevel <= 0) {
                    // -48dB to 0dB maps to 0% to 79%
                    meterHeight = ((dbLevel + 48) / 48) * 79;
                } else {
                    // 0dB to +6dB maps to 79% to 100%
                    meterHeight = 79 + Math.min(dbLevel / 6, 1) * 21;
                }
            }

            channel.faderMeter.style.height = `${meterHeight}%`;

            // Add clipping class if in red zone (above 0dB)
            channel.faderMeter.classList.toggle('clipping', level > 1.0);
        }
    }


    // Convert fader position (0-127) to linear volume
    // 0 = -inf (0.0), 100 = 0dB (1.0), 127 = +3dB (~1.41)
    faderToVolume(faderValue) {
        if (faderValue <= 0) return 0;
        if (faderValue >= 127) return Math.pow(10, 3/20);  // +3dB

        if (faderValue <= 100) {
            // 0-100: logarithmic curve from -inf to 0dB
            // Use an exponential curve for natural fader feel
            // fader 100 = 1.0, fader 0 = 0
            const normalized = faderValue / 100;
            // Apply a curve: vol = normalized^2 gives a nice taper
            // But we want more resolution at higher levels, so use a gentler curve
            return Math.pow(normalized, 1.5);
        } else {
            // 100-127: linear from 0dB to +3dB
            // 100 = 1.0 (0dB), 127 = 1.41 (+3dB)
            const boost = (faderValue - 100) / 27;  // 0 to 1
            const maxBoost = Math.pow(10, 3/20);    // ~1.41
            return 1.0 + boost * (maxBoost - 1.0);
        }
    }

    // Convert linear volume to fader position (0-127)
    volumeToFader(volume) {
        if (volume <= 0) return 0;

        const maxBoost = Math.pow(10, 3/20);  // ~1.41 (+3dB)
        if (volume >= maxBoost) return 127;

        if (volume <= 1.0) {
            // Inverse of the curve: fader = vol^(1/1.5) * 100
            return Math.round(Math.pow(volume, 1/1.5) * 100);
        } else {
            // Above unity: linear mapping
            const boost = (volume - 1.0) / (maxBoost - 1.0);
            return Math.round(100 + boost * 27);
        }
    }

    // Reset all mixer state and visuals (called on CLR)
    resetAll() {
        this.soloedLayer = null;

        this.channels.forEach((channel, idx) => {
            // Reset state
            channel.volume = 1.0;
            channel.pan = 0;
            channel.muted = false;
            channel.solo = false;
            channel.hasContent = false;
            channel.eqLow = 0;
            channel.eqMid = 0;
            channel.eqHigh = 0;
            channel.loopStart = 0;
            channel.loopEnd = 1;
            channel.reversed = false;

            // Reset fader visual to unity (position 100)
            if (channel.fader) {
                channel.fader.value = 100;
            }
            if (channel.faderMeter) {
                channel.faderMeter.style.height = '0%';
            }

            // Reset pan knob visual
            if (channel.panIndicator) {
                channel.panIndicator.style.transform = 'translateX(-50%) rotate(0deg)';
            }

            // Reset mute/solo buttons
            if (channel.muteBtn) channel.muteBtn.classList.remove('active');
            if (channel.soloBtn) channel.soloBtn.classList.remove('active');

            // Reset channel element states
            if (channel.channelEl) {
                channel.channelEl.classList.remove('has-content', 'muted', 'solo', 'override-layer');
            }

            // Reset EQ knob visuals
            ['Low', 'Mid', 'High'].forEach(band => {
                const indicator = channel[`eq${band}Indicator`];
                if (indicator) {
                    indicator.style.transform = 'translateX(-50%) rotate(0deg)';
                }
            });

            // Reset reverse button
            if (channel.reverseBtn) {
                channel.reverseBtn.classList.remove('active');
            }

            // Reset VU meter
            if (channel.vuNeedle) {
                channel.vuNeedle.style.transform = 'translateX(-50%) rotate(-45deg)';
            }

            // Sync to layer buttons
            this.syncMuteToLayerButton(idx + 1, false);
            this.syncSoloToLayerButton(idx + 1, false);
        });

        console.log('[Mixer] All channels reset');
    }
}

// ============================================
// LayerPanelController - Per-layer control panel
// ============================================

class LayerPanelController {
    constructor() {
        this.panel = document.getElementById('layer-control-panel');
        this.layerTitleEl = document.getElementById('layer-panel-title');
        this.layerNumEl = document.getElementById('layer-panel-num');
        this.closeBtn = document.getElementById('layer-panel-close');

        // Channel controls
        this.muteBtn = document.getElementById('layer-mute-btn');
        this.soloBtn = document.getElementById('layer-solo-btn');
        this.panKnob = document.getElementById('layer-pan-knob');
        this.panValueEl = document.getElementById('layer-pan-value');
        this.deleteBtn = document.getElementById('layer-delete-btn');

        // EQ knobs
        this.eqLowKnob = document.getElementById('layer-eq-low');
        this.eqMidKnob = document.getElementById('layer-eq-mid');
        this.eqHighKnob = document.getElementById('layer-eq-high');
        this.eqLowValue = document.getElementById('layer-eq-low-value');
        this.eqMidValue = document.getElementById('layer-eq-mid-value');
        this.eqHighValue = document.getElementById('layer-eq-high-value');

        // Reverse button
        this.reverseBtn = document.getElementById('layer-reverse-btn');

        // State
        this.selectedLayer = 0;  // 1-indexed
        this.muted = false;
        this.soloed = false;
        this.pan = 0;  // -1 to +1
        this.eqLow = 0;  // dB (-12 to +12)
        this.eqMid = 0;
        this.eqHigh = 0;
        this.reversed = false;

        // Drag state
        this.draggingKnob = null;
        this.dragStartY = 0;
        this.dragStartValue = 0;

        // Native functions - use correct names from C++ backend
        this.setLayerMutedFn = getNativeFunction('setLayerMuted');
        this.getLayerMutedFn = getNativeFunction('getLayerMuted');
        this.setLayerSoloFn = getNativeFunction('setLayerSolo');
        this.getLayerSoloFn = getNativeFunction('getLayerSolo');
        this.setLayerPanFn = getNativeFunction('setLayerPan');
        this.getLayerPanFn = getNativeFunction('getLayerPan');
        this.clearLayerFn = getNativeFunction('clearLayer');
        this.setLayerEQLowFn = getNativeFunction('setLayerEQLow');
        this.setLayerEQMidFn = getNativeFunction('setLayerEQMid');
        this.setLayerEQHighFn = getNativeFunction('setLayerEQHigh');
        this.getLayerEQFn = getNativeFunction('getLayerEQ');
        this.setLayerReverseFn = getNativeFunction('setLayerReverse');
        this.getLayerReverseFn = getNativeFunction('getLayerReverse');

        // Layer colors
        this.layerColors = [
            '#4fc3f7', '#ff7043', '#66bb6a', '#ab47bc',
            '#ffa726', '#26c6da', '#ec407a', '#9ccc65'
        ];

        this.setupEventListeners();
        console.log('[LayerPanel] Controller initialized');
    }

    setupEventListeners() {
        // Close button
        if (this.closeBtn) {
            this.closeBtn.addEventListener('click', () => this.hidePanel());
        }

        // Layer indicator button clicks to open panel
        document.querySelectorAll('.layer-indicators .layer-btn').forEach(btn => {
            const layer = parseInt(btn.dataset.layer);
            if (layer >= 1 && layer <= 8) {
                btn.addEventListener('click', () => {
                    console.log('[LayerPanel] Layer button clicked:', layer);
                    this.showPanel(layer);
                });
            }
        });

        // Mute button - use mixer's infrastructure
        if (this.muteBtn) {
            this.muteBtn.addEventListener('click', async () => {
                if (!this.selectedLayer || !window.mixerController) return;
                const mixer = window.mixerController;
                const channel = mixer.channels[this.selectedLayer - 1];
                if (channel) {
                    channel.muted = !channel.muted;
                    this.muted = channel.muted;
                    this.updateMuteUI();
                    channel.muteBtn?.classList.toggle('active', channel.muted);
                    channel.channelEl?.classList.toggle('muted', channel.muted);
                    try {
                        await mixer.setLayerMutedFn(this.selectedLayer, channel.muted);
                        mixer.syncMuteToLayerButton(this.selectedLayer, channel.muted);
                    } catch (e) { console.error('[LayerPanel] Mute error:', e); }
                }
            });
        }

        // Solo button - use mixer's toggleSolo
        if (this.soloBtn) {
            this.soloBtn.addEventListener('click', async () => {
                if (!this.selectedLayer || !window.mixerController) return;
                await window.mixerController.toggleSolo(this.selectedLayer);
                // Update our UI to reflect solo state
                const mixer = window.mixerController;
                this.soloed = mixer.soloedLayer === this.selectedLayer;
                this.muted = mixer.channels[this.selectedLayer - 1]?.muted || false;
                this.updateSoloUI();
                this.updateMuteUI();
            });
        }

        // Pan knob - use mixer's pan infrastructure
        if (this.panKnob) {
            this.panKnob.addEventListener('mousedown', (e) => {
                this.draggingKnob = 'pan';
                this.dragStartY = e.clientY;
                this.dragStartValue = this.pan;
                e.preventDefault();
            });
            this.panKnob.addEventListener('dblclick', async () => {
                this.pan = 0;
                this.updatePanUI();
                await this.sendPan();
            });
        }

        // Delete button
        if (this.deleteBtn) {
            this.deleteBtn.addEventListener('click', async () => {
                if (this.selectedLayer && this.clearLayerFn) {
                    await this.clearLayerFn(this.selectedLayer);
                    console.log('[LayerPanel] Cleared layer:', this.selectedLayer);
                }
            });
        }

        // EQ knob dragging - use mousedown on each knob
        [this.eqLowKnob, this.eqMidKnob, this.eqHighKnob].forEach(knob => {
            if (knob) {
                knob.addEventListener('mousedown', (e) => {
                    const band = knob.dataset.band;
                    this.draggingKnob = band;
                    this.dragStartY = e.clientY;
                    this.dragStartValue = this[`eq${band.charAt(0).toUpperCase() + band.slice(1)}`];
                    e.preventDefault();
                });

                // Double-click to reset
                knob.addEventListener('dblclick', () => {
                    const band = knob.dataset.band;
                    const propName = `eq${band.charAt(0).toUpperCase() + band.slice(1)}`;
                    this[propName] = 0;
                    this.updateEQUI();
                    this.sendEQ(band, 0);
                });
            }
        });

        // Reverse button
        if (this.reverseBtn) {
            this.reverseBtn.addEventListener('click', () => {
                this.reversed = !this.reversed;
                this.updateReverseUI();
                this.sendReverse();
                console.log('[LayerPanel] Reverse toggled:', this.reversed);
            });
        }

        // Global mouse events for dragging
        document.addEventListener('mousemove', (e) => this.handleMouseMove(e));
        document.addEventListener('mouseup', () => this.handleMouseUp());
    }

    handleMouseMove(e) {
        if (!this.draggingKnob) return;

        const deltaY = this.dragStartY - e.clientY;

        if (this.draggingKnob === 'pan') {
            // Pan: -1 to +1
            const sensitivity = 0.01;
            this.pan = Math.max(-1, Math.min(1, this.dragStartValue + deltaY * sensitivity));
            this.updatePanUI();
            this.sendPan();
        } else {
            // EQ bands
            const sensitivity = 0.2;  // dB per pixel
            const band = this.draggingKnob;
            const propName = `eq${band.charAt(0).toUpperCase() + band.slice(1)}`;
            const newValue = Math.max(-12, Math.min(12, this.dragStartValue + deltaY * sensitivity));
            this[propName] = newValue;
            this.updateEQUI();
            this.sendEQ(band, newValue);
        }
    }

    handleMouseUp() {
        this.draggingKnob = null;
    }

    showPanel(layer) {
        // Only show in waveform view, not mixer view
        if (window.mixerController && window.mixerController.currentView === 'mixer') {
            return;  // Don't show panel in mixer view
        }

        this.selectedLayer = layer;
        const layerColor = this.layerColors[layer - 1];

        // Color the entire "LAYER X" title in the layer color
        if (this.layerTitleEl) {
            this.layerTitleEl.style.color = layerColor;
        }
        if (this.layerNumEl) {
            this.layerNumEl.textContent = layer;
        }

        // Load layer state from engine
        this.loadLayerState(layer);

        if (this.panel) {
            this.panel.classList.remove('hidden');
        }

        console.log('[LayerPanel] Showing panel for layer:', layer);
    }

    hidePanel() {
        if (this.panel) {
            this.panel.classList.add('hidden');
        }
        this.selectedLayer = 0;
    }

    async loadLayerState(layer) {
        // Get mute/solo/pan from mixer controller if available
        if (window.mixerController) {
            const mixer = window.mixerController;
            const channel = mixer.channels[layer - 1];
            if (channel) {
                this.muted = channel.muted || false;
                this.soloed = mixer.soloedLayer === layer;
                this.pan = channel.pan || 0;
            }
        } else {
            // Fallback to native functions
            if (this.getLayerMutedFn) {
                try { this.muted = await this.getLayerMutedFn(layer) || false; } catch (e) { }
            }
            if (this.getLayerPanFn) {
                try { this.pan = await this.getLayerPanFn(layer) || 0; } catch (e) { }
            }
        }

        // Get EQ state
        if (this.getLayerEQFn) {
            try {
                const eq = await this.getLayerEQFn(layer);
                if (eq) {
                    this.eqLow = eq.low || 0;
                    this.eqMid = eq.mid || 0;
                    this.eqHigh = eq.high || 0;
                }
            } catch (e) {
                console.error('[LayerPanel] Error loading EQ:', e);
            }
        }

        // Get reverse state
        if (this.getLayerReverseFn) {
            try {
                const rev = await this.getLayerReverseFn(layer);
                this.reversed = rev || false;
            } catch (e) {
                console.error('[LayerPanel] Error loading reverse:', e);
            }
        }

        this.updateMuteUI();
        this.updateSoloUI();
        this.updatePanUI();
        this.updateEQUI();
        this.updateReverseUI();
    }

    updateMuteUI() {
        if (this.muteBtn) {
            this.muteBtn.classList.toggle('active', this.muted);
        }
    }

    updateSoloUI() {
        if (this.soloBtn) {
            this.soloBtn.classList.toggle('active', this.soloed);
        }
    }

    updatePanUI() {
        if (this.panKnob) {
            const indicator = this.panKnob.querySelector('.layer-pan-indicator');
            if (indicator) {
                // Map -1 to +1 to -135 to +135 degrees
                const angle = this.pan * 135;
                indicator.style.transform = `translateX(-50%) rotate(${angle}deg)`;
            }
        }
        if (this.panValueEl) {
            if (Math.abs(this.pan) < 0.05) {
                this.panValueEl.textContent = 'C';
            } else if (this.pan < 0) {
                this.panValueEl.textContent = `L${Math.round(Math.abs(this.pan) * 100)}`;
            } else {
                this.panValueEl.textContent = `R${Math.round(this.pan * 100)}`;
            }
        }
    }

    updateEQUI() {
        const bands = [
            { name: 'low', knob: this.eqLowKnob, valueEl: this.eqLowValue, value: this.eqLow },
            { name: 'mid', knob: this.eqMidKnob, valueEl: this.eqMidValue, value: this.eqMid },
            { name: 'high', knob: this.eqHighKnob, valueEl: this.eqHighValue, value: this.eqHigh }
        ];

        bands.forEach(({ knob, valueEl, value }) => {
            if (knob) {
                const indicator = knob.querySelector('.layer-knob-indicator');
                if (indicator) {
                    // Map -12 to +12 dB to -135 to +135 degrees
                    const angle = (value / 12) * 135;
                    indicator.style.transform = `translateX(-50%) rotate(${angle}deg)`;
                }
            }
            if (valueEl) {
                const sign = value >= 0 ? '+' : '';
                valueEl.textContent = `${sign}${value.toFixed(1)}dB`;
            }
        });
    }

    updateReverseUI() {
        if (this.reverseBtn) {
            this.reverseBtn.classList.toggle('active', this.reversed);
        }
    }

    async sendMute() {
        if (!this.selectedLayer || !this.setLayerMutedFn) return;
        try {
            await this.setLayerMutedFn(this.selectedLayer, this.muted);
        } catch (e) {
            console.error('[LayerPanel] Error setting mute:', e);
        }
    }

    async sendSolo() {
        if (!this.selectedLayer || !this.setLayerSoloFn) return;
        try {
            await this.setLayerSoloFn(this.selectedLayer, this.soloed);
        } catch (e) {
            console.error('[LayerPanel] Error setting solo:', e);
        }
    }

    async sendPan() {
        if (!this.selectedLayer) return;
        // Sync with mixer
        if (window.mixerController) {
            const mixer = window.mixerController;
            const channel = mixer.channels[this.selectedLayer - 1];
            if (channel) {
                channel.pan = this.pan;
                mixer.updatePanKnobUI(channel);
            }
        }
        // Send to backend
        if (this.setLayerPanFn) {
            try {
                await this.setLayerPanFn(this.selectedLayer, this.pan);
            } catch (e) {
                console.error('[LayerPanel] Error setting pan:', e);
            }
        }
    }

    async clearLayer() {
        if (!this.selectedLayer || !this.clearLayerFn) return;
        try {
            await this.clearLayerFn(this.selectedLayer);
            console.log('[LayerPanel] Cleared layer:', this.selectedLayer);
        } catch (e) {
            console.error('[LayerPanel] Error clearing layer:', e);
        }
    }

    async sendEQ(band, value) {
        if (!this.selectedLayer) return;

        const fnName = `setLayerEQ${band.charAt(0).toUpperCase() + band.slice(1)}Fn`;
        const fn = this[fnName];

        if (fn) {
            try {
                await fn(this.selectedLayer, value);
            } catch (e) {
                console.error(`[LayerPanel] Error setting EQ ${band}:`, e);
            }
        }
    }

    async sendReverse() {
        if (!this.selectedLayer || !this.setLayerReverseFn) return;
        try {
            await this.setLayerReverseFn(this.selectedLayer, this.reversed);
            console.log('[LayerPanel] Sent reverse:', this.reversed, 'to layer', this.selectedLayer);
        } catch (e) {
            console.error('[LayerPanel] Error setting reverse:', e);
        }
    }
}

// Global BPM update function called from C++ timer
window.updateBpmDisplay = function(bpm) {
    const bpmEl = document.getElementById('bpm-display');
    if (bpmEl) {
        bpmEl.textContent = bpm.toFixed(1);
    }
};

// Prevent text selection and drag globally
document.addEventListener('selectstart', (e) => e.preventDefault());
document.addEventListener('dragstart', (e) => e.preventDefault());

// Hide loading screen and show main content
function hideLoadingScreen() {
    const loadingScreen = document.getElementById('loading-screen');
    const mainContent = document.getElementById('main-content');

    if (loadingScreen) {
        loadingScreen.classList.add('hidden');
    }
    if (mainContent) {
        mainContent.style.opacity = '1';
    }
}

// Initialize when DOM is ready
document.addEventListener('DOMContentLoaded', () => {
    console.log('Initializing Loop Engine UI...');
    console.log('JUCE available:', typeof window.__JUCE__ !== 'undefined');

    // Hide loading screen after a short delay to allow fonts to load
    setTimeout(hideLoadingScreen, 500);

    // Tab Controller
    new TabController();

    // Looper Controller
    looperController = new LooperController();

    // Looper Knobs - with default values for reset
    // Note: Loop Start/End knobs removed - now using drag handles on waveform

    // Loop Speed: 0.5x - 2.0x (center = 1.0x), default 1.0x
    // Speed maps to semitones: -12st (0.5x) to +12st (2.0x), where speed = 2^(semitones/12)
    // Chromatic scale values for speed dropdown
    const speedToSemitone = (speed) => 12 * Math.log2(speed);
    const semitoneToSpeed = (st) => Math.pow(2, st / 12);

    // Speed value display element (combined % + chromatic)
    const speedValueEl = document.getElementById('loopSpeed-value');

    // Helper to update speed display (combined format: "100% 0st")
    const updateSpeedDisplay = (normalized) => {
        const speed = 0.5 * Math.pow(4, normalized);  // 0.5 * 4^v gives 0.5 to 2.0
        const semitones = speedToSemitone(speed);
        const roundedSt = Math.round(semitones);
        const sign = roundedSt >= 0 ? '+' : '';
        const pct = Math.round(speed * 100);
        if (speedValueEl) {
            speedValueEl.innerHTML = `${pct}% <span class="text-fd-text-dim">${sign}${roundedSt}st</span>`;
        }
    };

    loopSpeedKnob = new KnobController('loopSpeed-knob', 'loopSpeed', {
        formatValue: (v) => {
            updateSpeedDisplay(v);
            return '';
        },
        defaultValue: 0.5  // Default to 1.0x speed (0 semitones)
    });

    // Initialize speed display immediately
    updateSpeedDisplay(0.5);

    // Speed preset dropdown - fixed position menu
    const speedPresetSelect = document.getElementById('speed-preset-select');
    const speedState = getSliderState('loopSpeed');
    const speedKnobEl = document.getElementById('loopSpeed-knob');

    // Helper to show chromatic menu at fixed position
    const showChromaticMenu = (selectEl, anchorEl) => {
        const rect = anchorEl.getBoundingClientRect();
        selectEl.style.top = `${rect.bottom + 4}px`;
        selectEl.style.left = `${rect.right - selectEl.offsetWidth}px`;
        selectEl.classList.add('open');
        selectEl.focus();
        selectEl.size = Math.min(selectEl.options.length, 12);  // Show multiple options
    };

    const hideChromaticMenu = (selectEl) => {
        selectEl.classList.remove('open');
        selectEl.size = 1;
    };

    if (speedPresetSelect) {
        speedPresetSelect.addEventListener('change', (e) => {
            const speedValue = parseFloat(e.target.value);
            if (!isNaN(speedValue) && speedValue > 0) {
                const normalized = Math.log2(speedValue / 0.5) / 2;
                const clamped = Math.max(0, Math.min(1, normalized));
                if (loopSpeedKnob) {
                    loopSpeedKnob.setValue(clamped);
                    loopSpeedKnob.sendToJuce();
                }
            }
            hideChromaticMenu(speedPresetSelect);
        });

        speedState.valueChangedEvent.addListener(() => {
            const normalized = speedState.getNormalisedValue();
            updateSpeedDisplay(normalized);
            const speed = 0.5 * Math.pow(4, normalized);
            let closestOption = speedPresetSelect.options[0];
            let closestDiff = Infinity;
            for (let opt of speedPresetSelect.options) {
                const optSpeed = parseFloat(opt.value);
                const diff = Math.abs(optSpeed - speed);
                if (diff < closestDiff) {
                    closestDiff = diff;
                    closestOption = opt;
                }
            }
            if (closestDiff < 0.02) {
                speedPresetSelect.value = closestOption.value;
            }
        });

        // Cmd/Ctrl+click on knob shows fixed dropdown
        if (speedKnobEl) {
            speedKnobEl.addEventListener('click', (e) => {
                if (e.ctrlKey || e.metaKey) {
                    e.preventDefault();
                    e.stopPropagation();
                    showChromaticMenu(speedPresetSelect, speedKnobEl);
                }
            });
        }

        speedPresetSelect.addEventListener('blur', () => {
            setTimeout(() => hideChromaticMenu(speedPresetSelect), 150);
        });
    }

    // Pitch value display element (combined % + chromatic)
    const pitchValueEl = document.getElementById('loopPitch-value');

    // Helper to update pitch display (combined format: "100% 0st")
    // Shows pitch ratio as percentage (100% = no shift, 200% = octave up, 50% = octave down)
    const updatePitchDisplay = (normalized) => {
        const semitones = normalized * 24 - 12;
        const roundedSt = Math.round(semitones);
        const sign = roundedSt >= 0 ? '+' : '';
        // Calculate pitch ratio: 2^(semitones/12) gives the frequency ratio
        const pitchRatio = Math.pow(2, semitones / 12);
        const pct = Math.round(pitchRatio * 100);
        if (pitchValueEl) {
            pitchValueEl.innerHTML = `${pct}% <span class="text-fd-text-dim">${sign}${roundedSt}st</span>`;
        }
    };

    // Loop Pitch: -12 to +12 semitones (center = 0)
    loopPitchKnob = new KnobController('loopPitch-knob', 'loopPitch', {
        formatValue: (v) => {
            updatePitchDisplay(v);
            return '';
        },
        defaultValue: 0.5,
        shiftStep: 1,
        stepRange: { start: -12, end: 12 }
    });

    // Initialize pitch display immediately
    updatePitchDisplay(0.5);

    // Pitch dropdown - fixed position menu
    const pitchSelect = document.getElementById('pitch-select');
    const pitchState = getSliderState('loopPitch');
    const pitchKnobEl = document.getElementById('loopPitch-knob');

    if (pitchSelect) {
        pitchSelect.addEventListener('change', (e) => {
            const semitones = parseInt(e.target.value);
            const normalized = (semitones + 12) / 24;
            loopPitchKnob.setValue(normalized);
            loopPitchKnob.sendToJuce();
            hideChromaticMenu(pitchSelect);
        });

        pitchState.valueChangedEvent.addListener(() => {
            const normalized = pitchState.getNormalisedValue();
            updatePitchDisplay(normalized);
            const semitones = Math.round(normalized * 24 - 12);
            pitchSelect.value = semitones.toString();
        });

        // Cmd/Ctrl+click on knob shows fixed dropdown
        if (pitchKnobEl) {
            pitchKnobEl.addEventListener('click', (e) => {
                if (e.ctrlKey || e.metaKey) {
                    e.preventDefault();
                    e.stopPropagation();
                    showChromaticMenu(pitchSelect, pitchKnobEl);
                }
            });
        }

        pitchSelect.addEventListener('blur', () => {
            setTimeout(() => hideChromaticMenu(pitchSelect), 150);
        });
    }

    // Loop Fade: 0% (play once) to 100% (infinite loop)
    const fadeValueEl = document.getElementById('loopFade-value');
    loopFadeKnob = new KnobController('loopFade-knob', 'loopFade', {
        formatValue: (v) => {
            const pct = `${Math.round(v * 100)}%`;
            if (fadeValueEl) fadeValueEl.textContent = pct;
            return '';
        },
        defaultValue: 1.0  // Default to 100% (no fade - infinite loop)
    });

    // Delay Tab Knobs

    // Delay Time: 1ms - 2000ms (skewed range)
    // Delay Time - detail knob
    new KnobController('delayTime-knob', 'delayTime', {
        formatValue: (v) => {
            // The parameter uses a skewed range, so we approximate
            const ms = 1 + Math.pow(v, 2) * 1999;
            if (ms >= 1000) {
                return `${(ms / 1000).toFixed(2)} s`;
            }
            return `${Math.round(ms)} ms`;
        }
    });

    // Delay Time - card micro knob (no value display needed)
    // Auto-enable delay helper
    const autoEnableDelay = async () => {
        const delayLeds = [
            document.getElementById('delay-led'),
            document.getElementById('delay-detail-led')
        ];
        if (delayLeds[0]?.classList.contains('active')) return;
        delayLeds.forEach(l => l?.classList.add('active'));
        if (window.__JUCE__?.backend?.getNativeFunction) {
            const setDelayEnabled = window.__JUCE__.backend.getNativeFunction('setDelayEnabled');
            if (setDelayEnabled) await setDelayEnabled(true);
        }
    };

    new KnobController('delayTime-card-knob', 'delayTime', {
        formatValue: () => '',  // No value display for card knobs
        onTurn: autoEnableDelay
    });

    // Feedback: 0% - 95% - detail knob
    new KnobController('feedback-knob', 'feedback', {
        formatValue: (v) => `${Math.round(v * 95)}%`
    });

    // Feedback - card micro knob
    new KnobController('feedback-card-knob', 'feedback', {
        formatValue: () => '',
        onTurn: autoEnableDelay
    });

    // Mix - card micro knob
    new KnobController('mix-knob', 'mix', {
        formatValue: (v) => `${Math.round(v * 100)}%`,
        onTurn: autoEnableDelay
    });

    // Tone: 200Hz - 12kHz (skewed range)
    new KnobController('tone-knob', 'tone', {
        formatValue: (v) => {
            const hz = 200 + Math.pow(v, 0.3) * 11800;
            if (hz >= 1000) {
                return `${(hz / 1000).toFixed(1)} kHz`;
            }
            return `${Math.round(hz)} Hz`;
        }
    });

    // Mix: 0% - 100% (detail knob)
    new KnobController('mix-detail-knob', 'mix', {
        formatValue: (v) => `${Math.round(v * 100)}%`,
        valueElementId: 'mix-detail-value'
    });

    // BBD Character Controls

    // Age: 0% - 100%
    new KnobController('age-knob', 'age', {
        formatValue: (v) => `${Math.round(v * 100)}%`
    });

    // Mod Rate: 0.1Hz - 5Hz (skewed)
    new KnobController('modRate-knob', 'modRate', {
        formatValue: (v) => {
            const hz = 0.1 + Math.pow(v, 2) * 4.9;
            return `${hz.toFixed(1)} Hz`;
        }
    });

    // Mod Depth: 0ms - 20ms
    new KnobController('modDepth-knob', 'modDepth', {
        formatValue: (v) => {
            const ms = v * 20;
            return `${ms.toFixed(1)} ms`;
        }
    });

    // Warmth: 0% - 100%
    new KnobController('warmth-knob', 'warmth', {
        formatValue: (v) => `${Math.round(v * 100)}%`
    });

    // ============================================
    // LOFI TAB CONTROLLERS
    // ============================================

    // High-pass frequency: 20Hz - 2000Hz (logarithmic)
    new KnobController('lofiHP-knob', 'degradeHP', {
        valueElementId: 'lofiHP-value',
        formatValue: (v) => {
            const hz = 20 + Math.pow(v, 0.3) * 1980;
            return hz < 1000 ? `${Math.round(hz)}Hz` : `${(hz/1000).toFixed(1)}kHz`;
        }
    });

    // High-pass Q: 0.5 - 10
    new KnobController('lofiHPQ-knob', 'degradeHPQ', {
        valueElementId: 'lofiHPQ-value',
        formatValue: (v) => (0.5 + v * 9.5).toFixed(1)
    });

    // Low-pass frequency: 200Hz - 20kHz (logarithmic)
    new KnobController('lofiLP-knob', 'degradeLP', {
        valueElementId: 'lofiLP-value',
        formatValue: (v) => {
            const hz = 200 + Math.pow(v, 0.3) * 19800;
            return hz < 1000 ? `${Math.round(hz)}Hz` : `${(hz/1000).toFixed(1)}kHz`;
        }
    });

    // Low-pass Q: 0.5 - 10
    new KnobController('lofiLPQ-knob', 'degradeLPQ', {
        valueElementId: 'lofiLPQ-value',
        formatValue: (v) => (0.5 + v * 9.5).toFixed(1)
    });

    // Bit depth: 1 - 16 bits
    new KnobController('lofiBit-knob', 'degradeBit', {
        valueElementId: 'lofiBit-value',
        formatValue: (v) => `${Math.round(1 + v * 15)}`
    });

    // Sample rate: 1kHz - 48kHz (logarithmic)
    new KnobController('lofiSR-knob', 'degradeSR', {
        valueElementId: 'lofiSR-value',
        formatValue: (v) => {
            const khz = 1 + Math.pow(v, 0.4) * 47;
            return khz < 10 ? `${khz.toFixed(1)}k` : `${Math.round(khz)}k`;
        }
    });

    // Wobble: 0% - 100%
    new KnobController('lofiWobble-knob', 'degradeWobble', {
        valueElementId: 'lofiWobble-value',
        formatValue: (v) => `${Math.round(v * 100)}%`
    });

    // Vinyl: 0% - 100% (hiss + crackle)
    new KnobController('lofiVinyl-knob', 'degradeVinyl', {
        valueElementId: 'lofiVinyl-value',
        formatValue: (v) => `${Math.round(v * 100)}%`
    });

    // Lofi card micro knobs (no value display)
    new KnobController('lofiBit-card-knob', 'degradeBit', {
        formatValue: () => ''
    });
    new KnobController('lofiWobble-card-knob', 'degradeWobble', {
        formatValue: () => ''
    });
    new KnobController('lofiVinyl-card-knob', 'degradeVinyl', {
        formatValue: () => ''
    });
    new KnobController('lofiLP-card-knob', 'degradeLP', {
        formatValue: () => ''
    });

    // =========== MICRO LOOPER CONTROLS (MOOD-inspired) ===========

    // Micro Clock: controls buffer length (0% = 16s, 100% = 0.5s)
    new KnobController('microClock-knob', 'microClock', {
        formatValue: (v) => {
            // v is 0-1, maps to 0.5s (at 1) to 16s (at 0)
            const seconds = 0.5 + (1 - v) * 15.5;
            return seconds < 1 ? `${(seconds * 1000).toFixed(0)}ms` : `${seconds.toFixed(1)}s`;
        }
    });

    // Micro Length: subset of loop (5% - 100%)
    new KnobController('microLength-knob', 'microLength', {
        formatValue: (v) => `${Math.round(v * 100)}%`
    });

    // Micro Modify: mode-specific control (0% - 100%)
    new KnobController('microModify-knob', 'microModify', {
        formatValue: (v) => `${Math.round(v * 100)}%`
    });

    // Micro Speed: playback speed (0% = -2x, 50% = 1x, 100% = 2x)
    new KnobController('microSpeed-knob', 'microSpeed', {
        formatValue: (v) => {
            // v is 0-1, maps to -2x to +2x with 0.5 = 1x
            const speed = (v - 0.5) * 4;
            if (Math.abs(speed) < 0.1) return '1.0x';
            if (speed < 0) return `${(1 + speed).toFixed(1)}x`;
            return `${(1 + speed).toFixed(1)}x`;
        }
    });

    // Micro Mix: 0% - 100%
    new KnobController('microMix-knob', 'microMix', {
        formatValue: (v) => `${Math.round(v * 100)}%`
    });

    // Micro Looper controller (handles transport buttons, modes, etc.)
    // Stored globally so EffectsRack can reinitialize canvas when panel opens
    window.microLooperController = new MicroLooperController();

    // Lofi mix: 0% - 100%
    new KnobController('lofiMix-knob', 'degradeMix', {
        formatValue: (v) => `${Math.round(v * 100)}%`
    });

    // Lofi section controller (stored globally for canvas reinit)
    window.lofiController = new LofiController();

    // ============================================
    // GRANULAR/MICRO LOOPER CARD CONTROLS
    // ============================================

    // Card knobs for micro looper
    new KnobController('microMix-card-knob', 'microMix', {
        formatValue: (v) => `${Math.round(v * 100)}%`,
        valueElementId: 'microMix-card-value'
    });
    new KnobController('microSpeed-card-knob', 'microSpeed', {
        formatValue: (v) => {
            const speed = 0.25 + v * 1.75;
            return `${speed.toFixed(1)}x`;
        }
    });
    new KnobController('microModify-card-knob', 'microModify', {
        formatValue: (v) => `${Math.round(v * 100)}%`
    });

    // Granular/Micro Looper controller (LED toggle)
    new GranularController();

    // Reverb controller
    new ReverbController();

    // ============================================
    // SUB BASS CONTROLS
    // ============================================

    // Sub Bass Frequency: 30-80Hz
    new KnobController('subBassFreq-knob', 'subBassFreq', {
        formatValue: (v) => {
            const hz = 30 + v * 50;
            return `${Math.round(hz)}Hz`;
        }
    });

    // Sub Bass Amount: 0-100%
    new KnobController('subBassAmount-knob', 'subBassAmount', {
        formatValue: (v) => `${Math.round(v * 100)}%`
    });

    // Sub Bass controller
    new SubBassController();

    // BPM display and tempo sync
    new BpmDisplayController();

    // Host transport sync
    new HostSyncController();

    // Audio diagnostics panel
    new DiagnosticsController();

    // Crossfade settings modal
    new CrossfadeSettingsController();

    // Effects Rack controller
    new EffectsRackController();

    // Saturation controller
    new SaturationController();

    // Mixer view controller
    window.mixerController = new MixerViewController();

    // Layer panel controller (per-layer EQ, loop bounds, reverse)
    window.layerPanelController = new LayerPanelController();

    // Reverse toggle - setup handled in LooperController
    looperController.setupReverseButton();

    console.log('Loop Engine UI initialized');
});
