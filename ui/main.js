// Loop Engine Plugin UI Controller
// Implements JUCE WebView bindings without npm dependencies

// ============================================================
// VERSION - Increment this with each build to verify changes
// ============================================================
const UI_VERSION = "0.3.8";
console.log(`%c[Loop Engine UI] Version ${UI_VERSION} loaded`, 'color: #4fc3f7; font-weight: bold;');

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

        this.indicator = this.element.querySelector('.knob-indicator');
        this.valueDisplay = document.getElementById(`${paramName}-value`);
        this.paramName = paramName;

        this.minAngle = -135;
        this.maxAngle = 135;
        this.value = 0.5;

        this.formatValue = options.formatValue || ((v) => `${Math.round(v * 100)}%`);
        this.onValueChange = options.onValueChange || null;

        // Default value to use when resetting (optional)
        this.defaultValue = options.defaultValue !== undefined ? options.defaultValue : null;

        // Chromatic/stepped mode options (for pitch knob)
        // When shiftStep is set, Shift+drag will snap to discrete steps
        this.shiftStep = options.shiftStep || null;  // e.g., 1/24 for semitones
        this.stepRange = options.stepRange || { start: 0, end: 1 };  // Actual value range

        // Flag to ignore JUCE updates temporarily (used during reset)
        this.ignoreJuceUpdates = false;

        this.isDragging = false;
        this.isShiftDrag = false;  // Track if Shift is held during drag
        this.lastY = 0;

        this.setupEvents();
        this.setupJuceBinding();
    }

    setupEvents() {
        this.element.addEventListener('mousedown', (e) => {
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

        this.lastY = e.clientY;
    }

    endDrag() {
        if (this.isDragging) {
            this.isDragging = false;
            this.element.classList.remove('active');
            this.element.classList.remove('adjusting');
            if (this.sliderState) {
                this.sliderState.sliderDragEnded();
            }
        }
    }

    setValue(normalizedValue) {
        this.value = normalizedValue;
        const angle = this.minAngle + (this.maxAngle - this.minAngle) * normalizedValue;
        if (this.indicator) {
            // translateY(-100%) moves indicator up so bottom is at center
            // rotate() then spins around that bottom point (the knob center)
            this.indicator.style.transform = `translateY(-100%) rotate(${angle}deg)`;
        }
        if (this.valueDisplay) {
            this.valueDisplay.textContent = this.formatValue(normalizedValue);
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

        // Delay LED toggle state
        this.delayLed = document.getElementById('delay-led');
        this.delayEnabled = true;
        this.setDelayEnabledFn = getNativeFunction("setDelayEnabled");

        // Degrade LED toggle state
        this.degradeLed = document.getElementById('degrade-led');
        this.degradeEnabled = true;
        this.setDegradeEnabledFn = getNativeFunction("setDegradeEnabled");

        this.setupEvents();
        this.fetchInitialState();
    }

    async fetchInitialState() {
        try {
            // Fetch delay state
            const getTempoStateFn = getNativeFunction("getTempoState");
            const tempoState = await getTempoStateFn();
            if (tempoState && typeof tempoState.delayEnabled !== 'undefined') {
                this.delayEnabled = tempoState.delayEnabled;
                this.updateDelayLed();
            }
        } catch (e) {
            console.log('Could not fetch initial delay state');
        }

        try {
            // Fetch degrade state
            const getDegradeStateFn = getNativeFunction("getDegradeState");
            const degradeState = await getDegradeStateFn();
            if (degradeState && typeof degradeState.enabled !== 'undefined') {
                this.degradeEnabled = degradeState.enabled;
                this.updateDegradeLed();
            }
        } catch (e) {
            console.log('Could not fetch initial degrade state');
        }
    }

    setupEvents() {
        this.tabs.forEach(tab => {
            tab.addEventListener('click', (e) => {
                // Check if the click was on the LED
                if (e.target.classList.contains('tab-led')) {
                    // LED click - toggle effect bypass based on which tab
                    e.stopPropagation();
                    const tabName = tab.dataset.tab;
                    if (tabName === 'delay') {
                        this.toggleDelayEnabled();
                    } else if (tabName === 'degrade') {
                        this.toggleDegradeEnabled();
                    }
                } else {
                    // Regular tab click - switch tabs
                    const tabName = tab.dataset.tab;
                    this.switchTab(tabName);
                }
            });
        });

        // Also add direct click handler on LEDs for safety
        if (this.delayLed) {
            this.delayLed.addEventListener('click', (e) => {
                e.stopPropagation();
                this.toggleDelayEnabled();
            });
        }
        if (this.degradeLed) {
            this.degradeLed.addEventListener('click', (e) => {
                e.stopPropagation();
                this.toggleDegradeEnabled();
            });
        }
    }

    async toggleDelayEnabled() {
        this.delayEnabled = !this.delayEnabled;
        this.updateDelayLed();

        try {
            await this.setDelayEnabledFn(this.delayEnabled);
            console.log(`[DELAY] Delay ${this.delayEnabled ? 'enabled' : 'disabled'}`);
        } catch (e) {
            console.error('Error toggling delay:', e);
            // Revert on error
            this.delayEnabled = !this.delayEnabled;
            this.updateDelayLed();
        }
    }

    async toggleDegradeEnabled() {
        this.degradeEnabled = !this.degradeEnabled;
        this.updateDegradeLed();

        try {
            await this.setDegradeEnabledFn(this.degradeEnabled);
            console.log(`[DEGRADE] Degrade ${this.degradeEnabled ? 'enabled' : 'disabled'}`);
        } catch (e) {
            console.error('Error toggling degrade:', e);
            // Revert on error
            this.degradeEnabled = !this.degradeEnabled;
            this.updateDegradeLed();
        }
    }

    updateDelayLed() {
        if (this.delayLed) {
            this.delayLed.classList.toggle('active', this.delayEnabled);
        }
    }

    updateDegradeLed() {
        if (this.degradeLed) {
            this.degradeLed.classList.toggle('active', this.degradeEnabled);
        }
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

        // Loop region values (0-1 normalized)
        this.loopStart = 0;
        this.loopEnd = 1;

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
        this.getStateFn = getNativeFunction("getLoopState");
        this.jumpToLayerFn = getNativeFunction("loopJumpToLayer");
        this.resetParamsFn = getNativeFunction("resetLoopParams");

        // Loop length setting (bars + beats)
        this.loopLengthBars = 0;
        this.loopLengthBeats = 0;
        this.setLoopLengthBarsFn = getNativeFunction("setLoopLengthBars");
        this.setLoopLengthBeatsFn = getNativeFunction("setLoopLengthBeats");

        this.setupTransport();
        this.setupLoopLengthSelector();
        this.setupLayers();
        this.setupWaveform();
        this.setupZoomControls();
        this.setupRecordingOverlay();
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
            if (loopStartKnob) {
                loopStartKnob.resetToDefault();
                console.log('[SYNC] Reset loopStart knob to default');
            }
            if (loopEndKnob) {
                loopEndKnob.resetToDefault();
                console.log('[SYNC] Reset loopEnd knob to default');
            }
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
    }

    setupLoopLengthSelector() {
        this.barsSelect = document.getElementById('bars-select');
        this.beatsSelect = document.getElementById('beats-select');

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

        try {
            await this.setLoopLengthBeatsFn(beats);
        } catch (e) {
            console.error('Error setting loop length beats:', e);
        }
    }

    setupLayers() {
        this.layerBtns = document.querySelectorAll('.layer-btn');
        this.setLayerMutedFn = getNativeFunction("setLayerMuted");

        this.layerBtns.forEach(btn => {
            // Left click: jump to layer
            btn.addEventListener('click', () => {
                const layer = parseInt(btn.dataset.layer);
                this.jumpToLayer(layer);
            });

            // Right click: toggle mute
            btn.addEventListener('contextmenu', async (e) => {
                e.preventDefault();
                const layer = parseInt(btn.dataset.layer);
                const isMuted = btn.classList.contains('muted');
                const newMuted = !isMuted;
                btn.classList.toggle('muted', newMuted);
                try {
                    await this.setLayerMutedFn(layer, newMuted);
                    console.log(`Layer ${layer} muted: ${newMuted}`);
                } catch (err) {
                    console.error('Error toggling layer mute:', err);
                }
            });
        });
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

        // Handle drag movement
        document.addEventListener('mousemove', (e) => {
            if (!this.draggingHandle || !this.waveformContainer) return;

            const rect = this.waveformContainer.getBoundingClientRect();
            let normalizedPos = (e.clientX - rect.left) / rect.width;
            normalizedPos = Math.max(0, Math.min(1, normalizedPos));

            if (this.draggingHandle === 'start') {
                // Start can't go past end - 1%
                const maxStart = this.loopEnd - 0.01;
                normalizedPos = Math.min(normalizedPos, maxStart);
                this.setLoopStart(normalizedPos);
                this.sendLoopStartToJuce(normalizedPos);
            } else if (this.draggingHandle === 'end') {
                // End can't go before start + 1%
                const minEnd = this.loopStart + 0.01;
                normalizedPos = Math.max(normalizedPos, minEnd);
                this.setLoopEnd(normalizedPos);
                this.sendLoopEndToJuce(normalizedPos);
            }
        });

        // End drag
        document.addEventListener('mouseup', () => {
            if (this.draggingHandle) {
                this.draggingHandle = null;
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
        if (this.loopRegionShade) {
            this.loopRegionShade.style.setProperty('--loop-start', `${this.loopStart * 100}%`);
            this.loopRegionShade.style.setProperty('--loop-end', `${this.loopEnd * 100}%`);
        }

        // Update handle positions using percentage for responsiveness
        if (this.loopStartHandle) {
            this.loopStartHandle.style.left = `${this.loopStart * 100}%`;
        }
        if (this.loopEndHandle) {
            this.loopEndHandle.style.left = `${this.loopEnd * 100}%`;
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
            for (let layerIdx = 0; layerIdx < layerWaveforms.length; layerIdx++) {
                const layerData = layerWaveforms[layerIdx];
                if (!layerData || layerData.length === 0) continue;

                // Check if this layer is muted - draw dimmed if so
                const isMuted = layerMutes && layerMutes[layerIdx];
                const layerColor = this.layerColors[layerIdx % this.layerColors.length];

                // Set color with alpha for layering effect
                // Muted layers are drawn very dim
                if (isMuted) {
                    this.ctx.fillStyle = layerColor + '30';  // 30% opacity for muted
                } else {
                    this.ctx.fillStyle = layerColor + '99';  // 60% opacity for active
                }

                const barWidth = zoomedWidth / layerData.length;

                for (let i = 0; i < layerData.length; i++) {
                    const amplitude = layerData[i] * (height * 0.35);
                    const x = (i * barWidth) - offsetX;
                    // Only draw visible bars
                    if (x + barWidth > 0 && x < width && amplitude > 0.5) {
                        this.ctx.fillRect(x, centerY - amplitude, Math.max(1, barWidth - 1), amplitude * 2);
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
            // Position is 0-1 within the loop region (between start and end)
            // Convert to actual canvas position
            const loopRegionWidth = this.loopEnd - this.loopStart;
            const absolutePosition = this.loopStart + (position * loopRegionWidth);

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
            await this.recordFn();
            this.updateTransportUI('recording');
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

        // Update REC button label and state based on current state
        // Blooper-style: REC button shows "REC" normally, "DUB" when overdubbing
        switch (state) {
            case 'recording':
                if (this.recBtn) this.recBtn.classList.add('active');
                if (this.recLabel) this.recLabel.textContent = 'REC';
                break;
            case 'playing':
                if (this.playBtn) this.playBtn.classList.add('active');
                if (this.recLabel) this.recLabel.textContent = 'DUB';  // Show that REC will start overdub
                break;
            case 'overdubbing':
                if (this.recBtn) this.recBtn.classList.add('active');
                if (this.recLabel) this.recLabel.textContent = 'DUB';
                break;
            case 'idle':
            default:
                if (this.recLabel) this.recLabel.textContent = 'REC';
                break;
        }

        // Handle recording overlay
        if (state === 'recording' && previousState !== 'recording') {
            this.showRecordingOverlay();
        } else if (state !== 'recording' && previousState === 'recording') {
            this.hideRecordingOverlay();
        }
    }

    updateLayerUI(currentLayer, highestLayer) {
        this.currentLayer = currentLayer;
        this.highestLayer = highestLayer;

        this.layerBtns.forEach(btn => {
            const layer = parseInt(btn.dataset.layer);
            btn.classList.remove('active', 'has-content');

            if (layer === currentLayer) {
                btn.classList.add('active');
            }
            if (layer <= highestLayer) {
                btn.classList.add('has-content');
            }
        });
    }

    // Sync layer mute button UI with backend state
    syncLayerMuteUI(muteStates) {
        this.layerBtns.forEach(btn => {
            const layer = parseInt(btn.dataset.layer);
            const idx = layer - 1;  // 0-indexed
            if (idx >= 0 && idx < muteStates.length) {
                const isMuted = muteStates[idx];
                btn.classList.toggle('muted', isMuted);
            }
        });
    }

    startStatePolling() {
        // Poll loop state every 50ms for smooth playhead updates
        setInterval(async () => {
            try {
                const state = await this.getStateFn();
                if (state) {
                    // Update playhead position
                    if (typeof state.playhead !== 'undefined') {
                        this.updatePlayhead(state.playhead);
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
                        // Simulate input level based on waveform activity
                        if (state.waveform && state.waveform.length > 0) {
                            const recentLevel = state.waveform[state.waveform.length - 1] || 0;
                            this.updateInputLevel(recentLevel);
                        }
                    }

                    // Update layer UI
                    if (typeof state.layer !== 'undefined' && typeof state.highestLayer !== 'undefined') {
                        if (state.layer !== this.currentLayer || state.highestLayer !== this.highestLayer) {
                            this.updateLayerUI(state.layer, state.highestLayer);
                        }
                    }

                    // DISABLED: Don't let polling overwrite reverse state
                    // The button click is the source of truth for reverse
                    // if (typeof state.isReversed !== 'undefined' && !this.reverseButtonPending) {
                    //     if (state.isReversed !== this.isReversed) {
                    //         console.log(`[REV] State poll: C++ says ${state.isReversed}, UI says ${this.isReversed} - syncing`);
                    //         this.setReversed(state.isReversed);
                    //     }
                    // }

                    // Update waveform if provided (with per-layer colors if available)
                    if (state.layerWaveforms || state.waveform) {
                        // Debug: log layer waveform data
                        if (state.layerWaveforms && state.layerWaveforms.length > 1) {
                            console.log(`[WAVEFORM] ${state.layerWaveforms.length} layers, mutes:`, state.layerMutes);
                        }
                        this.drawWaveform(state.waveform, state.layerWaveforms, state.layerMutes);
                    }

                    // Sync layer mute UI states from backend
                    if (state.layerMutes && state.layerMutes.length > 0) {
                        this.syncLayerMuteUI(state.layerMutes);
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
let loopStartKnob = null;
let loopEndKnob = null;
let loopSpeedKnob = null;
let loopPitchKnob = null;
let loopFadeKnob = null;

// Host Transport Sync Controller
class HostSyncController {
    constructor() {
        this.led = document.getElementById('host-sync-led');
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
                    this.updateLed();
                }
                if (typeof state.hostPlaying !== 'undefined') {
                    this.isHostPlaying = state.hostPlaying;
                    this.updateLed();
                }
            }
        } catch (e) {
            console.log('Could not fetch initial host sync state');
        }
    }

    setupEvents() {
        if (this.led) {
            this.led.addEventListener('click', () => this.toggle());
        }
    }

    async toggle() {
        this.isEnabled = !this.isEnabled;
        this.updateLed();

        try {
            await this.setHostSyncFn(this.isEnabled);
            console.log(`[HOST SYNC] ${this.isEnabled ? 'Enabled' : 'Disabled'}`);
        } catch (e) {
            console.error('Error toggling host sync:', e);
            this.isEnabled = !this.isEnabled;
            this.updateLed();
        }
    }

    updateLed() {
        if (this.led) {
            this.led.classList.toggle('active', this.isEnabled);
            this.led.classList.toggle('playing', this.isEnabled && this.isHostPlaying);
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
                        this.updateLed();
                    }
                }
            } catch (e) {
                // Silently ignore
            }
        }, 200);
    }
}

// BPM Display and Tempo Sync Controller
class BpmDisplayController {
    constructor() {
        this.bpmEl = document.getElementById('bpm-display');
        this.syncBtn = document.getElementById('tempo-sync-toggle');
        this.noteSelect = document.getElementById('note-value-select');
        this.isSyncEnabled = false;

        this.setSyncFn = getNativeFunction("setTempoSync");
        this.setNoteFn = getNativeFunction("setTempoNote");

        this.setupEvents();
        this.fetchInitialState();
    }

    setupEvents() {
        if (this.syncBtn) {
            this.syncBtn.addEventListener('click', () => this.toggleSync());
        }

        if (this.noteSelect) {
            this.noteSelect.addEventListener('change', (e) => this.setNoteValue(parseInt(e.target.value)));
        }
    }

    async fetchInitialState() {
        try {
            const getStateFn = getNativeFunction("getTempoState");
            const state = await getStateFn();
            if (state) {
                if (typeof state.syncEnabled !== 'undefined') {
                    this.isSyncEnabled = state.syncEnabled;
                    this.updateSyncUI();
                }
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

    async toggleSync() {
        this.isSyncEnabled = !this.isSyncEnabled;
        this.updateSyncUI();

        try {
            await this.setSyncFn(this.isSyncEnabled);
        } catch (e) {
            console.error('Error setting tempo sync:', e);
        }
    }

    updateSyncUI() {
        if (this.syncBtn) {
            this.syncBtn.classList.toggle('active', this.isSyncEnabled);
            const label = this.syncBtn.querySelector('.toggle-label');
            if (label) {
                label.textContent = `Sync: ${this.isSyncEnabled ? 'On' : 'Off'}`;
            }
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

// Degrade section controller with LEDs and filter visualization
class DegradeController {
    constructor() {
        // Section LEDs
        this.filterLed = document.getElementById('filter-led');
        this.lofiLed = document.getElementById('lofi-led');
        this.scramblerLed = document.getElementById('scrambler-led');

        // Section containers (for dimming when disabled)
        this.filterSection = this.filterLed?.closest('.degrade-section');
        this.lofiSection = this.lofiLed?.closest('.degrade-section');
        this.scramblerSection = this.scramblerLed?.closest('.degrade-section');

        // Filter visualization canvas
        this.filterCanvas = document.getElementById('filter-viz-canvas');
        this.filterCtx = this.filterCanvas?.getContext('2d');

        // State
        this.filterEnabled = true;
        this.lofiEnabled = true;
        this.scramblerEnabled = true;

        // Native functions
        this.setFilterEnabledFn = getNativeFunction('setDegradeFilterEnabled');
        this.setLofiEnabledFn = getNativeFunction('setDegradeLofiEnabled');
        this.setScramblerEnabledFn = getNativeFunction('setDegradeScramblerEnabled');
        this.getDegradeStateFn = getNativeFunction('getDegradeState');

        // Current filter params for visualization
        this.hpFreq = 20;
        this.lpFreq = 20000;
        this.hpQ = 0.707;
        this.lpQ = 0.707;

        this.setupEvents();
        this.fetchInitialState();
        this.startPolling();
    }

    setupEvents() {
        if (this.filterLed) {
            this.filterLed.addEventListener('click', (e) => {
                e.stopPropagation();
                this.toggleFilter();
            });
        }
        if (this.lofiLed) {
            this.lofiLed.addEventListener('click', (e) => {
                e.stopPropagation();
                this.toggleLofi();
            });
        }
        if (this.scramblerLed) {
            this.scramblerLed.addEventListener('click', (e) => {
                e.stopPropagation();
                this.toggleScrambler();
            });
        }
    }

    async fetchInitialState() {
        try {
            const state = await this.getDegradeStateFn();
            if (state) {
                this.filterEnabled = state.filterEnabled !== false;
                this.lofiEnabled = state.lofiEnabled !== false;
                this.scramblerEnabled = state.scramblerEnabled !== false;
                this.updateUI();
            }
        } catch (e) {
            console.log('Could not fetch initial degrade state');
        }
    }

    async toggleFilter() {
        this.filterEnabled = !this.filterEnabled;
        this.updateUI();
        try {
            await this.setFilterEnabledFn(this.filterEnabled);
            console.log(`[DEGRADE] Filter ${this.filterEnabled ? 'enabled' : 'disabled'}`);
        } catch (e) {
            console.error('Error toggling filter:', e);
            this.filterEnabled = !this.filterEnabled;
            this.updateUI();
        }
    }

    async toggleLofi() {
        this.lofiEnabled = !this.lofiEnabled;
        this.updateUI();
        try {
            await this.setLofiEnabledFn(this.lofiEnabled);
            console.log(`[DEGRADE] Lo-Fi ${this.lofiEnabled ? 'enabled' : 'disabled'}`);
        } catch (e) {
            console.error('Error toggling lo-fi:', e);
            this.lofiEnabled = !this.lofiEnabled;
            this.updateUI();
        }
    }

    async toggleScrambler() {
        this.scramblerEnabled = !this.scramblerEnabled;
        this.updateUI();
        try {
            await this.setScramblerEnabledFn(this.scramblerEnabled);
            console.log(`[DEGRADE] Scrambler ${this.scramblerEnabled ? 'enabled' : 'disabled'}`);
        } catch (e) {
            console.error('Error toggling scrambler:', e);
            this.scramblerEnabled = !this.scramblerEnabled;
            this.updateUI();
        }
    }

    updateUI() {
        // Update LEDs
        if (this.filterLed) {
            this.filterLed.classList.toggle('active', this.filterEnabled);
        }
        if (this.lofiLed) {
            this.lofiLed.classList.toggle('active', this.lofiEnabled);
        }
        if (this.scramblerLed) {
            this.scramblerLed.classList.toggle('active', this.scramblerEnabled);
        }

        // Update section opacity (but keep clickable)
        if (this.filterSection) {
            this.filterSection.style.opacity = this.filterEnabled ? '1' : '0.5';
        }
        if (this.lofiSection) {
            this.lofiSection.style.opacity = this.lofiEnabled ? '1' : '0.5';
        }
        if (this.scramblerSection) {
            this.scramblerSection.style.opacity = this.scramblerEnabled ? '1' : '0.5';
        }
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

        const width = this.filterCanvas.width;
        const height = this.filterCanvas.height;
        const ctx = this.filterCtx;

        // Clear canvas
        ctx.fillStyle = '#0a0a0a';
        ctx.fillRect(0, 0, width, height);

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

        // Draw combined filter response curve
        ctx.beginPath();
        ctx.strokeStyle = '#66bb6a';
        ctx.lineWidth = 2;

        for (let i = 0; i < width; i++) {
            const freq = this.xToFreq(i, width);
            const hpResponse = this.getHPResponse(freq);
            const lpResponse = this.getLPResponse(freq);
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

        // Draw HP and LP cutoff markers
        ctx.fillStyle = '#66bb6a';
        ctx.font = '9px "JetBrains Mono", monospace';

        // HP marker
        const hpX = this.freqToX(this.hpFreq, width);
        ctx.fillStyle = 'rgba(102, 187, 106, 0.3)';
        ctx.fillRect(0, 0, hpX, height);

        // LP marker
        const lpX = this.freqToX(this.lpFreq, width);
        ctx.fillStyle = 'rgba(102, 187, 106, 0.3)';
        ctx.fillRect(lpX, 0, width - lpX, height);

        // Frequency labels
        ctx.fillStyle = '#444';
        ctx.textAlign = 'center';
        ctx.fillText('100', this.freqToX(100, width), height - 3);
        ctx.fillText('1k', this.freqToX(1000, width), height - 3);
        ctx.fillText('10k', this.freqToX(10000, width), height - 3);
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

// Initialize when DOM is ready
document.addEventListener('DOMContentLoaded', () => {
    console.log(`Initializing Loop Engine UI v${UI_VERSION}...`);
    console.log('JUCE available:', typeof window.__JUCE__ !== 'undefined');
    console.log('JUCE object:', window.__JUCE__);
    console.log('JUCE backend:', window.__JUCE__?.backend);
    console.log('JUCE getNativeFunction:', typeof window.__JUCE__?.backend?.getNativeFunction);

    // Set version in footer
    const versionEl = document.getElementById('version-ticker');
    if (versionEl) {
        versionEl.textContent = `v${UI_VERSION}`;
    }

    // Tab Controller
    new TabController();

    // Looper Controller
    looperController = new LooperController();

    // Looper Knobs - with default values for reset

    // Loop Start: 0% - 100%, default 0%
    loopStartKnob = new KnobController('loopStart-knob', 'loopStart', {
        formatValue: (v) => `${Math.round(v * 100)}%`,
        defaultValue: 0,  // Default to 0%
        onValueChange: (v) => {
            if (looperController) {
                looperController.setLoopStart(v);
            }
        }
    });

    // Loop End: 0% - 100%, default 100%
    loopEndKnob = new KnobController('loopEnd-knob', 'loopEnd', {
        formatValue: (v) => `${Math.round(v * 100)}%`,
        defaultValue: 1,  // Default to 100%
        onValueChange: (v) => {
            if (looperController) {
                looperController.setLoopEnd(v);
            }
        }
    });

    // Loop Speed: 0.25x - 4.0x (center = 1.0x), default 1.0x
    // At normalized 0.5: 0.25 * 16^0.5 = 0.25 * 4 = 1.0x
    loopSpeedKnob = new KnobController('loopSpeed-knob', 'loopSpeed', {
        formatValue: (v) => {
            // Map 0-1 to 0.25-4.0 with center at 1.0
            // Using exponential mapping: 0.25 * 16^v = 0.25 to 4.0
            const speed = 0.25 * Math.pow(16, v);
            return `${speed.toFixed(2)}x`;
        },
        defaultValue: 0.5  // Default to 1.0x speed
    });

    // Loop Pitch: -12 to +12 semitones (center = 0)
    // The C++ parameter sends scaled values (-12 to +12)
    // Shift+drag enables chromatic mode (1 semitone steps)
    loopPitchKnob = new KnobController('loopPitch-knob', 'loopPitch', {
        formatValue: (v) => {
            // v is normalized 0-1, but we need to show the actual semitone value
            // The SliderState will have received properties from C++ with start=-12, end=12
            const state = getSliderState('loopPitch');
            const start = state?.properties?.start ?? -12;
            const end = state?.properties?.end ?? 12;
            const semitones = v * (end - start) + start;
            const sign = semitones >= 0 ? '+' : '';
            // Show integer when value is close to a whole semitone
            const isWhole = Math.abs(semitones - Math.round(semitones)) < 0.05;
            const display = isWhole ? Math.round(semitones) : semitones.toFixed(1);
            return `${sign}${display} st`;
        },
        defaultValue: 0.5,  // Default to 0 semitones (no pitch shift)
        shiftStep: 1,       // 1 semitone step when Shift is held
        stepRange: { start: -12, end: 12 }  // Full range is 24 semitones
    });

    // Pitch dropdown for direct semitone selection
    const pitchSelect = document.getElementById('pitch-select');
    const pitchState = getSliderState('loopPitch');

    if (pitchSelect) {
        // When dropdown changes, update the knob and parameter
        pitchSelect.addEventListener('change', (e) => {
            const semitones = parseInt(e.target.value);
            // Convert semitones (-12 to +12) to normalized (0 to 1)
            const normalized = (semitones + 12) / 24;
            loopPitchKnob.setValue(normalized);
            loopPitchKnob.sendToJuce();
        });

        // When knob/parameter changes, update dropdown to nearest semitone
        pitchState.valueChangedEvent.addListener(() => {
            const normalized = pitchState.getNormalisedValue();
            const semitones = Math.round(normalized * 24 - 12);
            pitchSelect.value = semitones.toString();
        });
    }

    // Loop Fade: 0% (play once) to 100% (infinite loop)
    loopFadeKnob = new KnobController('loopFade-knob', 'loopFade', {
        formatValue: (v) => `${Math.round(v * 100)}%`,
        defaultValue: 1.0  // Default to 100% (no fade - infinite loop)
    });

    // Delay Tab Knobs

    // Delay Time: 1ms - 2000ms (skewed range)
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

    // Feedback: 0% - 95%
    new KnobController('feedback-knob', 'feedback', {
        formatValue: (v) => `${Math.round(v * 95)}%`
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

    // Mix: 0% - 100%
    new KnobController('mix-knob', 'mix', {
        formatValue: (v) => `${Math.round(v * 100)}%`
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
    // DEGRADE TAB CONTROLLERS
    // ============================================

    // High-pass frequency: 20Hz - 2000Hz (logarithmic)
    new KnobController('degradeHP-knob', 'degradeHP', {
        formatValue: (v) => {
            const hz = 20 + Math.pow(v, 0.3) * 1980;
            return hz < 1000 ? `${Math.round(hz)}Hz` : `${(hz/1000).toFixed(1)}kHz`;
        }
    });

    // High-pass Q: 0.5 - 10
    new KnobController('degradeHPQ-knob', 'degradeHPQ', {
        formatValue: (v) => (0.5 + v * 9.5).toFixed(1)
    });

    // Low-pass frequency: 200Hz - 20kHz (logarithmic)
    new KnobController('degradeLP-knob', 'degradeLP', {
        formatValue: (v) => {
            const hz = 200 + Math.pow(v, 0.3) * 19800;
            return hz < 1000 ? `${Math.round(hz)}Hz` : `${(hz/1000).toFixed(1)}kHz`;
        }
    });

    // Low-pass Q: 0.5 - 10
    new KnobController('degradeLPQ-knob', 'degradeLPQ', {
        formatValue: (v) => (0.5 + v * 9.5).toFixed(1)
    });

    // Bit depth: 1 - 16 bits
    new KnobController('degradeBit-knob', 'degradeBit', {
        formatValue: (v) => `${Math.round(1 + v * 15)}-bit`
    });

    // Sample rate: 1kHz - 48kHz (logarithmic)
    new KnobController('degradeSR-knob', 'degradeSR', {
        formatValue: (v) => {
            const khz = 1 + Math.pow(v, 0.4) * 47;
            return `${khz.toFixed(1)}kHz`;
        }
    });

    // Wobble: 0% - 100%
    new KnobController('degradeWobble-knob', 'degradeWobble', {
        formatValue: (v) => `${Math.round(v * 100)}%`
    });

    // Scramble amount: 0% - 100%
    new KnobController('degradeScrambleAmt-knob', 'degradeScrambleAmt', {
        formatValue: (v) => `${Math.round(v * 100)}%`
    });

    // Degrade mix: 0% - 100%
    new KnobController('degradeMix-knob', 'degradeMix', {
        formatValue: (v) => `${Math.round(v * 100)}%`
    });

    // Subdivision selector for scrambler
    const subdivSelect = document.getElementById('scramble-subdiv');
    if (subdivSelect) {
        const setScrambleSubdivFn = getNativeFunction('setDegradeScrambleSubdiv');
        subdivSelect.addEventListener('change', async (e) => {
            const subdiv = parseInt(e.target.value);
            console.log(`[DEGRADE] Setting scramble subdiv: ${subdiv}`);
            await setScrambleSubdivFn(subdiv);
        });
    }

    // Degrade section controller
    new DegradeController();

    // BPM display and tempo sync
    new BpmDisplayController();

    // Host transport sync
    new HostSyncController();

    // Reverse toggle - setup handled in LooperController
    looperController.setupReverseButton();

    console.log('Loop Engine UI initialized');
});
