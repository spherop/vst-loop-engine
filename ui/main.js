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
            if (this.sliderState) {
                this.sliderState.sliderDragEnded();
            }
        }
    }

    setValue(normalizedValue) {
        this.value = normalizedValue;
        const angle = this.minAngle + (this.maxAngle - this.minAngle) * normalizedValue;
        if (this.indicator) {
            this.indicator.style.transform = `translateX(-50%) rotate(${angle}deg)`;
        }
        if (this.valueDisplay) {
            this.valueDisplay.textContent = this.formatValue(normalizedValue);
        }
    }

    sendToJuce() {
        if (this.sliderState) {
            this.sliderState.setNormalisedValue(this.value);
        }
    }
}

// Test Sound Controller
class TestSoundController {
    constructor() {
        this.activeButton = null;
        this.triggerTestSoundFn = getNativeFunction("triggerTestSound");
        this.stopTestSoundFn = getNativeFunction("stopTestSound");
        this.setupButtons();
    }

    setupButtons() {
        document.querySelectorAll('.sound-btn[data-sound]').forEach(btn => {
            btn.addEventListener('click', () => {
                const soundType = parseInt(btn.dataset.sound);
                this.triggerSound(soundType, btn);
            });
        });

        const stopBtn = document.getElementById('btn-stop');
        if (stopBtn) {
            stopBtn.addEventListener('click', () => this.stopSound());
        }
    }

    async triggerSound(soundType, button) {
        if (this.activeButton) {
            this.activeButton.classList.remove('active');
        }

        button.classList.add('active');
        this.activeButton = button;

        try {
            await this.triggerTestSoundFn(soundType);
        } catch (e) {
            console.error('Error triggering sound:', e);
        }
    }

    async stopSound() {
        if (this.activeButton) {
            this.activeButton.classList.remove('active');
            this.activeButton = null;
        }

        try {
            await this.stopTestSoundFn();
        } catch (e) {
            console.error('Error stopping sound:', e);
        }
    }
}

// Prevent text selection and drag globally
document.addEventListener('selectstart', (e) => e.preventDefault());
document.addEventListener('dragstart', (e) => e.preventDefault());

// Initialize when DOM is ready
document.addEventListener('DOMContentLoaded', () => {
    console.log('Initializing Fuzz Delay UI...');
    console.log('JUCE available:', typeof window.__JUCE__ !== 'undefined');

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

    // Test sounds
    new TestSoundController();

    console.log('Fuzz Delay UI initialized');
});
