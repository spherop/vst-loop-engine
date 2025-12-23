// Fuzz Delay Plugin UI Controller
// Implements JUCE WebView bindings without npm dependencies

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
            window.__JUCE__.backend.emitEvent("__juce__invoke", {
                name: name,
                params: Array.prototype.slice.call(arguments),
                resultId: promiseId
            });
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

        this.isDragging = false;
        this.lastY = 0;

        this.setupEvents();
        this.setupJuceBinding();
    }

    setupEvents() {
        this.element.addEventListener('mousedown', (e) => this.startDrag(e));
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
    }

    setupJuceBinding() {
        if (typeof window.__JUCE__ !== 'undefined') {
            this.sliderState = getSliderState(this.paramName);

            this.sliderState.valueChangedEvent.addListener(() => {
                this.setValue(this.sliderState.getNormalisedValue());
            });

            // Request initial value after a short delay to ensure JUCE is ready
            setTimeout(() => {
                this.setValue(this.sliderState.getNormalisedValue());
            }, 100);
        }
    }

    startDrag(e) {
        this.isDragging = true;
        this.lastY = e.clientY;
        this.element.classList.add('active');
        this.element.classList.add('adjusting');
        if (this.sliderState) {
            this.sliderState.sliderDragStarted();
        }
    }

    drag(e) {
        if (!this.isDragging) return;

        const deltaY = this.lastY - e.clientY;
        const sensitivity = 0.005;
        const newValue = Math.max(0, Math.min(1, this.value + deltaY * sensitivity));

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
            this.sliderState.setNormalisedValue(this.value);
        }
    }
}

// Test Sound Controller - with dynamic dropdown from loaded samples
class TestSoundController {
    constructor() {
        this.isPlaying = false;
        this.triggerTestSoundFn = getNativeFunction("triggerTestSound");
        this.stopTestSoundFn = getNativeFunction("stopTestSound");
        this.getTestSoundsFn = getNativeFunction("getTestSounds");
        this.reloadSamplesFn = getNativeFunction("reloadSamples");
        this.chooseFolderFn = getNativeFunction("chooseSampleFolder");
        this.setupControls();
        this.loadSoundList();
    }

    setupControls() {
        this.soundSelect = document.getElementById('sound-select');
        this.playBtn = document.getElementById('play-btn');
        this.stopBtn = document.getElementById('stop-btn');
        this.reloadBtn = document.getElementById('reload-btn');
        this.folderBtn = document.getElementById('folder-btn');
        this.folderPath = document.getElementById('folder-path');
        this.sampleIndicator = document.getElementById('sample-indicator');

        if (this.playBtn) {
            this.playBtn.addEventListener('click', () => this.play());
        }

        if (this.stopBtn) {
            this.stopBtn.addEventListener('click', () => this.stop());
        }

        if (this.reloadBtn) {
            this.reloadBtn.addEventListener('click', () => this.reloadSamples());
        }

        if (this.folderBtn) {
            this.folderBtn.addEventListener('click', () => this.chooseFolder());
        }
    }

    async loadSoundList() {
        try {
            const result = await this.getTestSoundsFn();
            if (result && result.sounds) {
                this.populateDropdown(result.sounds);
                this.updateIndicator(result.usingSamples, result.sampleFolder);
            }
        } catch (e) {
            console.log('Could not load sound list, using defaults');
        }
    }

    populateDropdown(sounds) {
        if (!this.soundSelect) return;

        // Clear existing options
        this.soundSelect.innerHTML = '';

        // Add new options
        sounds.forEach((name, index) => {
            const option = document.createElement('option');
            option.value = index;
            option.textContent = name;
            this.soundSelect.appendChild(option);
        });
    }

    updateIndicator(usingSamples, sampleFolder) {
        if (this.sampleIndicator) {
            if (usingSamples) {
                this.sampleIndicator.textContent = 'SAMPLES';
                this.sampleIndicator.classList.add('active');
                this.sampleIndicator.title = sampleFolder || '';
            } else {
                this.sampleIndicator.textContent = 'SYNTH';
                this.sampleIndicator.classList.remove('active');
                this.sampleIndicator.title = 'No samples found - using synthesized sounds';
            }
        }

        // Update folder path display
        if (this.folderPath && sampleFolder) {
            // Shorten the path for display
            const shortPath = sampleFolder.replace(/^\/Users\/[^\/]+/, '~');
            this.folderPath.textContent = shortPath;
            this.folderPath.title = sampleFolder;
        }
    }

    async chooseFolder() {
        if (this.folderBtn) {
            this.folderBtn.classList.add('loading');
        }

        try {
            const result = await this.chooseFolderFn();
            if (result && !result.cancelled) {
                if (result.sounds) {
                    this.populateDropdown(result.sounds);
                }
                this.updateIndicator(result.usingSamples, result.sampleFolder);
            }
        } catch (e) {
            console.error('Error choosing folder:', e);
        }

        if (this.folderBtn) {
            this.folderBtn.classList.remove('loading');
        }
    }

    async reloadSamples() {
        if (this.reloadBtn) {
            this.reloadBtn.classList.add('loading');
        }

        try {
            const result = await this.reloadSamplesFn();
            if (result && result.sounds) {
                this.populateDropdown(result.sounds);
                this.updateIndicator(result.usingSamples);
            }
        } catch (e) {
            console.error('Error reloading samples:', e);
        }

        if (this.reloadBtn) {
            this.reloadBtn.classList.remove('loading');
        }
    }

    async play() {
        const soundIndex = this.soundSelect ? parseInt(this.soundSelect.value) : 0;
        this.isPlaying = true;
        if (this.playBtn) this.playBtn.classList.add('active');

        try {
            await this.triggerTestSoundFn(soundIndex);
        } catch (e) {
            console.error('Error triggering sound:', e);
        }
    }

    async stop() {
        this.isPlaying = false;
        if (this.playBtn) this.playBtn.classList.remove('active');

        try {
            await this.stopTestSoundFn();
        } catch (e) {
            console.error('Error stopping sound:', e);
        }
    }
}

// Loop Toggle Controller
class LoopToggleController {
    constructor() {
        this.element = document.getElementById('loop-toggle');
        this.isEnabled = false;
        this.setLoopFn = getNativeFunction("setLoopEnabled");

        if (this.element) {
            this.element.addEventListener('click', () => this.toggle());
        }

        // Get initial state
        this.fetchInitialState();
    }

    async fetchInitialState() {
        try {
            const getStateFn = getNativeFunction("getTempoState");
            const state = await getStateFn();
            if (state && typeof state.loopEnabled !== 'undefined') {
                this.isEnabled = state.loopEnabled;
                this.updateUI();
            }
        } catch (e) {
            console.log('Could not fetch initial loop state');
        }
    }

    async toggle() {
        this.isEnabled = !this.isEnabled;
        this.updateUI();

        try {
            await this.setLoopFn(this.isEnabled);
        } catch (e) {
            console.error('Error setting loop:', e);
        }
    }

    updateUI() {
        if (this.element) {
            this.element.classList.toggle('active', this.isEnabled);
        }
    }
}

// Tab Controller
class TabController {
    constructor() {
        this.tabs = document.querySelectorAll('.tab');
        this.contents = document.querySelectorAll('.tab-content');
        this.currentTab = 'looper';
        this.setupEvents();
    }

    setupEvents() {
        this.tabs.forEach(tab => {
            tab.addEventListener('click', () => {
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

        // Zoom state
        this.zoomLevel = 1.0;
        this.zoomOffset = 0; // For panning when zoomed
        this.minZoom = 1.0;
        this.maxZoom = 8.0;

        // Loop region values (0-1 normalized)
        this.loopStart = 0;
        this.loopEnd = 1;

        // Looper accent color (light blue)
        this.accentColor = '#4fc3f7';
        this.accentColorDim = '#29b6f6';

        // Native functions
        this.recordFn = getNativeFunction("loopRecord");
        this.playFn = getNativeFunction("loopPlay");
        this.stopFn = getNativeFunction("loopStop");
        this.overdubFn = getNativeFunction("loopOverdub");
        this.undoFn = getNativeFunction("loopUndo");
        this.clearFn = getNativeFunction("loopClear");
        this.getStateFn = getNativeFunction("getLoopState");
        this.jumpToLayerFn = getNativeFunction("loopJumpToLayer");

        this.setupTransport();
        this.setupLayers();
        this.setupWaveform();
        this.setupZoomControls();
        this.setupRecordingOverlay();
        this.startStatePolling();
    }

    setupTransport() {
        this.recBtn = document.getElementById('rec-btn');
        this.playBtn = document.getElementById('loop-play-btn');
        this.stopBtn = document.getElementById('loop-stop-btn');
        this.overdubBtn = document.getElementById('overdub-btn');
        this.undoBtn = document.getElementById('undo-btn');
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
        if (this.overdubBtn) {
            this.overdubBtn.addEventListener('click', () => this.overdub());
        }
        if (this.undoBtn) {
            this.undoBtn.addEventListener('click', () => this.undo());
        }
        if (this.clearBtn) {
            this.clearBtn.addEventListener('click', () => this.clear());
        }
    }

    setupLayers() {
        this.layerBtns = document.querySelectorAll('.layer-btn');
        this.layerBtns.forEach(btn => {
            btn.addEventListener('click', () => {
                const layer = parseInt(btn.dataset.layer);
                this.jumpToLayer(layer);
            });
        });
    }

    setupWaveform() {
        this.waveformCanvas = document.getElementById('waveform-canvas');
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

        // Update handle positions
        if (this.loopStartHandle && this.waveformCanvas) {
            const x = this.loopStart * this.waveformCanvas.width;
            this.loopStartHandle.style.left = `${x}px`;
        }
        if (this.loopEndHandle && this.waveformCanvas) {
            const x = this.loopEnd * this.waveformCanvas.width;
            this.loopEndHandle.style.right = `${this.waveformCanvas.width - x}px`;
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

    drawWaveform(waveformData) {
        if (!this.ctx || !waveformData || waveformData.length === 0) {
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

        // Draw waveform with zoom
        this.ctx.fillStyle = this.accentColor;
        const barWidth = zoomedWidth / waveformData.length;

        for (let i = 0; i < waveformData.length; i++) {
            const amplitude = waveformData[i] * (height * 0.4);
            const x = (i * barWidth) - offsetX;
            // Only draw visible bars
            if (x + barWidth > 0 && x < width) {
                this.ctx.fillRect(x, centerY - amplitude, Math.max(1, barWidth - 1), amplitude * 2);
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
        this.isReversed = reversed;
        // Update playhead color/style to indicate direction
        if (this.playhead) {
            this.playhead.classList.toggle('reversed', reversed);
        }
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
        [this.recBtn, this.playBtn, this.overdubBtn].forEach(btn => {
            if (btn) btn.classList.remove('active');
        });

        // Activate appropriate button
        switch (state) {
            case 'recording':
                if (this.recBtn) this.recBtn.classList.add('active');
                break;
            case 'playing':
                if (this.playBtn) this.playBtn.classList.add('active');
                break;
            case 'overdubbing':
                if (this.overdubBtn) this.overdubBtn.classList.add('active');
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

                    // Update reverse state
                    if (typeof state.isReversed !== 'undefined' && state.isReversed !== this.isReversed) {
                        this.setReversed(state.isReversed);
                        // Also update the reverse button
                        const reverseBtn = document.getElementById('reverse-btn');
                        if (reverseBtn) {
                            reverseBtn.classList.toggle('active', state.isReversed);
                        }
                    }

                    // Update waveform if provided
                    if (state.waveform) {
                        this.drawWaveform(state.waveform);
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
    console.log('Initializing Loop Engine UI...');
    console.log('JUCE available:', typeof window.__JUCE__ !== 'undefined');

    // Tab Controller
    new TabController();

    // Looper Controller
    looperController = new LooperController();

    // Looper Knobs

    // Loop Start: 0% - 100%
    new KnobController('loopStart-knob', 'loopStart', {
        formatValue: (v) => `${Math.round(v * 100)}%`,
        onValueChange: (v) => {
            if (looperController) {
                looperController.setLoopStart(v);
            }
        }
    });

    // Loop End: 0% - 100%
    new KnobController('loopEnd-knob', 'loopEnd', {
        formatValue: (v) => `${Math.round(v * 100)}%`,
        onValueChange: (v) => {
            if (looperController) {
                looperController.setLoopEnd(v);
            }
        }
    });

    // Loop Speed: 0.25x - 4.0x (center = 1.0x)
    new KnobController('loopSpeed-knob', 'loopSpeed', {
        formatValue: (v) => {
            // Map 0-1 to 0.25-4.0 with center at 1.0
            // Using exponential mapping: 0.25 * 16^v = 0.25 to 4.0
            const speed = 0.25 * Math.pow(16, v);
            return `${speed.toFixed(2)}x`;
        }
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

    // Test sounds
    new TestSoundController();

    // Loop toggle (for test audio)
    new LoopToggleController();

    // BPM display and tempo sync
    new BpmDisplayController();

    // Reverse toggle
    const reverseBtn = document.getElementById('reverse-btn');
    if (reverseBtn) {
        const setReverseFn = getNativeFunction("setLoopReverse");

        reverseBtn.addEventListener('click', async () => {
            const isReversed = !looperController.isReversed;
            looperController.setReversed(isReversed);
            reverseBtn.classList.toggle('active', isReversed);
            try {
                await setReverseFn(isReversed);
            } catch (e) {
                console.error('Error setting reverse:', e);
            }
        });
    }

    console.log('Loop Engine UI initialized');
});
