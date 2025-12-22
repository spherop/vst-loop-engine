// Fuzz Delay Plugin UI Controller

class KnobController {
    constructor(elementId, paramName, options = {}) {
        this.element = document.getElementById(elementId);
        this.indicator = this.element.querySelector('.knob-indicator');
        this.valueDisplay = document.getElementById(`${paramName}-value`);
        this.paramName = paramName;

        this.minAngle = -135;
        this.maxAngle = 135;
        this.value = 0.5; // Normalized 0-1

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

        // Touch support
        this.element.addEventListener('touchstart', (e) => {
            e.preventDefault();
            this.startDrag(e.touches[0]);
        });
        document.addEventListener('touchmove', (e) => {
            if (this.isDragging) {
                e.preventDefault();
                this.drag(e.touches[0]);
            }
        });
        document.addEventListener('touchend', () => this.endDrag());

        // Mouse wheel support
        this.element.addEventListener('wheel', (e) => {
            e.preventDefault();
            const delta = e.deltaY > 0 ? -0.02 : 0.02;
            this.setValue(Math.max(0, Math.min(1, this.value + delta)));
            this.sendToJuce();
        });
    }

    setupJuceBinding() {
        if (typeof window.__JUCE__ !== 'undefined') {
            const sliderState = window.__JUCE__.getSliderState(this.paramName);

            if (sliderState) {
                // Listen for changes from C++
                sliderState.valueChangedEvent.addListener(() => {
                    this.setValue(sliderState.getNormalisedValue());
                });

                // Get initial value
                this.setValue(sliderState.getNormalisedValue());

                this.sliderState = sliderState;
            }
        }
    }

    startDrag(e) {
        this.isDragging = true;
        this.lastY = e.clientY;
        this.element.classList.add('active');
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
        this.isDragging = false;
        this.element.classList.remove('active');
    }

    setValue(normalizedValue) {
        this.value = normalizedValue;
        const angle = this.minAngle + (this.maxAngle - this.minAngle) * normalizedValue;
        this.indicator.style.transform = `translateX(-50%) rotate(${angle}deg)`;

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
        this.setupButtons();
    }

    setupButtons() {
        // Sound trigger buttons
        document.querySelectorAll('.sound-btn[data-sound]').forEach(btn => {
            btn.addEventListener('click', () => {
                const soundType = parseInt(btn.dataset.sound);
                this.triggerSound(soundType, btn);
            });
        });

        // Stop button
        const stopBtn = document.getElementById('btn-stop');
        if (stopBtn) {
            stopBtn.addEventListener('click', () => this.stopSound());
        }
    }

    triggerSound(soundType, button) {
        // Update active state
        if (this.activeButton) {
            this.activeButton.classList.remove('active');
        }

        button.classList.add('active');
        this.activeButton = button;

        // Call native function
        if (typeof window.__JUCE__ !== 'undefined') {
            window.__JUCE__.backend.triggerTestSound(soundType);
        }
    }

    stopSound() {
        // Clear active state
        if (this.activeButton) {
            this.activeButton.classList.remove('active');
            this.activeButton = null;
        }

        // Call native function
        if (typeof window.__JUCE__ !== 'undefined') {
            window.__JUCE__.backend.stopTestSound();
        }
    }
}

// Initialize when DOM is ready
document.addEventListener('DOMContentLoaded', () => {
    // Delay Time: 1ms - 2000ms
    new KnobController('delayTime-knob', 'delayTime', {
        formatValue: (v) => {
            const ms = 1 + v * 1999; // 1 to 2000ms
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

    // Tone: 200Hz - 12kHz
    new KnobController('tone-knob', 'tone', {
        formatValue: (v) => {
            const hz = 200 + v * 11800; // 200 to 12000Hz
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
