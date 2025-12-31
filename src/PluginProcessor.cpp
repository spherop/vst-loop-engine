#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>

LoopEngineProcessor::LoopEngineProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "Parameters", createParameterLayout())
{
    // Cache parameter pointers for efficient access in processBlock
    delayTimeParam = apvts.getRawParameterValue("delayTime");
    feedbackParam = apvts.getRawParameterValue("feedback");
    mixParam = apvts.getRawParameterValue("mix");
    toneParam = apvts.getRawParameterValue("tone");

    // BBD character parameters
    ageParam = apvts.getRawParameterValue("age");
    modRateParam = apvts.getRawParameterValue("modRate");
    modDepthParam = apvts.getRawParameterValue("modDepth");
    warmthParam = apvts.getRawParameterValue("warmth");

    // Degrade parameters
    degradeHPParam = apvts.getRawParameterValue("degradeHP");
    degradeHPQParam = apvts.getRawParameterValue("degradeHPQ");
    degradeLPParam = apvts.getRawParameterValue("degradeLP");
    degradeLPQParam = apvts.getRawParameterValue("degradeLPQ");
    degradeBitParam = apvts.getRawParameterValue("degradeBit");
    degradeSRParam = apvts.getRawParameterValue("degradeSR");
    degradeWobbleParam = apvts.getRawParameterValue("degradeWobble");
    degradeVinylParam = apvts.getRawParameterValue("degradeVinyl");
    degradeMixParam = apvts.getRawParameterValue("degradeMix");

    // Micro looper parameters
    microClockParam = apvts.getRawParameterValue("microClock");
    microLengthParam = apvts.getRawParameterValue("microLength");
    microModifyParam = apvts.getRawParameterValue("microModify");
    microSpeedParam = apvts.getRawParameterValue("microSpeed");
    microMixParam = apvts.getRawParameterValue("microMix");

    // Saturation parameters
    satMixParam = apvts.getRawParameterValue("satMix");
    // Soft type
    satSoftDriveParam = apvts.getRawParameterValue("satSoftDrive");
    satSoftToneParam = apvts.getRawParameterValue("satSoftTone");
    satSoftCurveParam = apvts.getRawParameterValue("satSoftCurve");
    // Tape type
    satTapeDriveParam = apvts.getRawParameterValue("satTapeDrive");
    satTapeBiasParam = apvts.getRawParameterValue("satTapeBias");
    satTapeFlutterParam = apvts.getRawParameterValue("satTapeFlutter");
    satTapeToneParam = apvts.getRawParameterValue("satTapeTone");
    // Tube type
    satTubeDriveParam = apvts.getRawParameterValue("satTubeDrive");
    satTubeBiasParam = apvts.getRawParameterValue("satTubeBias");
    satTubeWarmthParam = apvts.getRawParameterValue("satTubeWarmth");
    satTubeSagParam = apvts.getRawParameterValue("satTubeSag");
    // Fuzz type
    satFuzzDriveParam = apvts.getRawParameterValue("satFuzzDrive");
    satFuzzGateParam = apvts.getRawParameterValue("satFuzzGate");
    satFuzzOctaveParam = apvts.getRawParameterValue("satFuzzOctave");
    satFuzzToneParam = apvts.getRawParameterValue("satFuzzTone");

    // Sub Bass parameters
    subBassFreqParam = apvts.getRawParameterValue("subBassFreq");
    subBassAmountParam = apvts.getRawParameterValue("subBassAmount");

    // Reverb parameters
    reverbSizeParam = apvts.getRawParameterValue("reverbSize");
    reverbDecayParam = apvts.getRawParameterValue("reverbDecay");
    reverbDampParam = apvts.getRawParameterValue("reverbDamp");
    reverbMixParam = apvts.getRawParameterValue("reverbMix");
    reverbWidthParam = apvts.getRawParameterValue("reverbWidth");
    reverbPreDelayParam = apvts.getRawParameterValue("reverbPreDelay");
    reverbModRateParam = apvts.getRawParameterValue("reverbModRate");
    reverbModDepthParam = apvts.getRawParameterValue("reverbModDepth");
}

LoopEngineProcessor::~LoopEngineProcessor()
{
}

juce::AudioProcessorValueTreeState::ParameterLayout LoopEngineProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // Delay parameters
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"delayTime", 1},
        "Delay Time",
        juce::NormalisableRange<float>(1.0f, 2000.0f, 0.1f, 0.5f),
        300.0f,
        juce::AudioParameterFloatAttributes().withLabel("ms")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"feedback", 1},
        "Feedback",
        juce::NormalisableRange<float>(0.0f, 95.0f, 0.1f),
        40.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"mix", 1},
        "Mix",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        50.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"tone", 1},
        "Tone",
        juce::NormalisableRange<float>(200.0f, 12000.0f, 1.0f, 0.3f),
        4000.0f,
        juce::AudioParameterFloatAttributes().withLabel("Hz")));

    // BBD Character parameters
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"age", 1},
        "Age",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        25.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"modRate", 1},
        "Mod Rate",
        juce::NormalisableRange<float>(0.1f, 5.0f, 0.01f, 0.5f),
        0.5f,
        juce::AudioParameterFloatAttributes().withLabel("Hz")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"modDepth", 1},
        "Mod Depth",
        juce::NormalisableRange<float>(0.0f, 20.0f, 0.1f),
        3.0f,
        juce::AudioParameterFloatAttributes().withLabel("ms")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"warmth", 1},
        "Warmth",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        30.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    // Loop parameters
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"loopStart", 1},
        "Loop Start",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f),
        0.0f));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"loopEnd", 1},
        "Loop End",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f),
        1.0f));

    // Loop Speed: 0.25x to 4.0x, with 1.0x at center (normalized 0.5)
    // For skew: we need 1.0 = 0.25 + (4.0-0.25) * pow(0.5, 1/skew)
    // Solving: (1.0-0.25)/(4.0-0.25) = pow(0.5, 1/skew)
    // 0.2 = pow(0.5, 1/skew) => log(0.2)/log(0.5) = 1/skew => skew = log(0.5)/log(0.2) ≈ 0.431
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"loopSpeed", 1},
        "Loop Speed",
        juce::NormalisableRange<float>(0.25f, 4.0f, 0.01f, 0.431f),
        1.0f,
        juce::AudioParameterFloatAttributes().withLabel("x")));

    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"loopReverse", 1},
        "Loop Reverse",
        false));

    // Pitch shift: -24 to +24 semitones (2 octaves), with 0 at center
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"loopPitch", 1},
        "Loop Pitch",
        juce::NormalisableRange<float>(-24.0f, 24.0f, 0.1f),
        0.0f,
        juce::AudioParameterFloatAttributes().withLabel("st")));

    // Loop fade/decay: 0% (play once) to 100% (infinite loop)
    // This is like feedback for a delay - controls how much signal remains per loop cycle
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"loopFade", 1},
        "Loop Fade",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        100.0f,  // Default to 100% (no fade - infinite loop)
        juce::AudioParameterFloatAttributes().withLabel("%")));

    // =========== DEGRADE PARAMETERS ===========

    // High-pass filter frequency
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"degradeHP", 1},
        "Degrade HP",
        juce::NormalisableRange<float>(20.0f, 2000.0f, 1.0f, 0.3f),
        20.0f,
        juce::AudioParameterFloatAttributes().withLabel("Hz")));

    // High-pass filter resonance (Q)
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"degradeHPQ", 1},
        "Degrade HP Q",
        juce::NormalisableRange<float>(0.5f, 10.0f, 0.1f, 0.5f),
        0.707f));

    // Low-pass filter frequency
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"degradeLP", 1},
        "Degrade LP",
        juce::NormalisableRange<float>(200.0f, 20000.0f, 1.0f, 0.3f),
        20000.0f,
        juce::AudioParameterFloatAttributes().withLabel("Hz")));

    // Low-pass filter resonance (Q)
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"degradeLPQ", 1},
        "Degrade LP Q",
        juce::NormalisableRange<float>(0.5f, 10.0f, 0.1f, 0.5f),
        0.707f));

    // Bit crusher depth
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"degradeBit", 1},
        "Bit Depth",
        juce::NormalisableRange<float>(1.0f, 16.0f, 0.1f),
        16.0f,
        juce::AudioParameterFloatAttributes().withLabel("bit")));

    // Sample rate reduction
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"degradeSR", 1},
        "Sample Rate",
        juce::NormalisableRange<float>(1000.0f, 48000.0f, 1.0f, 0.4f),
        48000.0f,
        juce::AudioParameterFloatAttributes().withLabel("Hz")));

    // Wobble amount
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"degradeWobble", 1},
        "Wobble",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        0.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    // Vinyl degradation (hiss + crackle)
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"degradeVinyl", 1},
        "Vinyl",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        0.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    // Degrade mix (dry/wet)
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"degradeMix", 1},
        "Degrade Mix",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        100.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    // =========== MICRO LOOPER PARAMETERS ===========
    // MOOD-inspired always-listening micro-looper

    // Clock: controls buffer length (like MOOD's sample rate control)
    // 0% = 16 seconds (long, low pitch); 100% = 0.5 seconds (short, high pitch)
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"microClock", 1},
        "Micro Clock",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        50.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    // Length: subset of loop to play (0-100%)
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"microLength", 1},
        "Micro Length",
        juce::NormalisableRange<float>(5.0f, 100.0f, 0.1f),
        100.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    // Modify: mode-specific control
    // ENV: envelope sensitivity; TAPE: scrub position; STRETCH: grain size
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"microModify", 1},
        "Micro Modify",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        50.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    // Speed: playback speed control (for TAPE/STRETCH modes)
    // 0% = -2x (reverse fast); 50% = 1x (normal); 100% = 2x (fast forward)
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"microSpeed", 1},
        "Micro Speed",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        50.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    // Mix: dry/wet for micro looper
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"microMix", 1},
        "Micro Mix",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        50.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    // =========== SATURATION PARAMETERS ===========

    // Master saturation mix
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"satMix", 1},
        "Saturation Mix",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        100.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    // --- SOFT TYPE ---
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"satSoftDrive", 1},
        "Soft Drive",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        30.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"satSoftTone", 1},
        "Soft Tone",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        50.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"satSoftCurve", 1},
        "Soft Curve",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        30.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    // --- TAPE TYPE ---
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"satTapeDrive", 1},
        "Tape Drive",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        40.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"satTapeBias", 1},
        "Tape Bias",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        50.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"satTapeFlutter", 1},
        "Tape Flutter",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        20.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"satTapeTone", 1},
        "Tape Tone",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        60.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    // --- TUBE TYPE ---
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"satTubeDrive", 1},
        "Tube Drive",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        35.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"satTubeBias", 1},
        "Tube Bias",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        50.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"satTubeWarmth", 1},
        "Tube Warmth",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        50.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"satTubeSag", 1},
        "Tube Sag",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        20.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    // --- FUZZ TYPE ---
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"satFuzzDrive", 1},
        "Fuzz Drive",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        60.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"satFuzzGate", 1},
        "Fuzz Gate",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        0.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"satFuzzOctave", 1},
        "Fuzz Octave",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        0.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"satFuzzTone", 1},
        "Fuzz Tone",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        50.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    // =======================
    // SUB BASS
    // =======================
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"subBassFreq", 1},
        "Sub Bass Frequency",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        50.0f,  // Default ~55Hz (30 + 0.5*50)
        juce::AudioParameterFloatAttributes().withLabel("%")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"subBassAmount", 1},
        "Sub Bass Amount",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        0.0f,  // Default off
        juce::AudioParameterFloatAttributes().withLabel("%")));

    // =======================
    // REVERB
    // =======================
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"reverbSize", 1},
        "Reverb Size",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        50.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"reverbDecay", 1},
        "Reverb Decay",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        50.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"reverbDamp", 1},
        "Reverb Damping",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        50.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"reverbMix", 1},
        "Reverb Mix",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        30.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"reverbWidth", 1},
        "Reverb Width",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        100.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"reverbPreDelay", 1},
        "Reverb Pre-Delay",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        10.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"reverbModRate", 1},
        "Reverb Mod Rate",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        30.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"reverbModDepth", 1},
        "Reverb Mod Depth",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f),
        20.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));

    return { params.begin(), params.end() };
}

const juce::String LoopEngineProcessor::getName() const
{
    return JucePlugin_Name;
}

bool LoopEngineProcessor::acceptsMidi() const
{
    return false;
}

bool LoopEngineProcessor::producesMidi() const
{
    return false;
}

bool LoopEngineProcessor::isMidiEffect() const
{
    return false;
}

double LoopEngineProcessor::getTailLengthSeconds() const
{
    return 2.0;
}

int LoopEngineProcessor::getNumPrograms()
{
    return 1;
}

int LoopEngineProcessor::getCurrentProgram()
{
    return 0;
}

void LoopEngineProcessor::setCurrentProgram(int index)
{
    juce::ignoreUnused(index);
}

const juce::String LoopEngineProcessor::getProgramName(int index)
{
    juce::ignoreUnused(index);
    return {};
}

void LoopEngineProcessor::changeProgramName(int index, const juce::String& newName)
{
    juce::ignoreUnused(index, newName);
}

void LoopEngineProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    // Prepare delay lines
    delayLineL.prepare(sampleRate, 2000); // Max 2 second delay
    delayLineR.prepare(sampleRate, 2000);

    // Prepare loop engine
    loopEngine.prepare(sampleRate, samplesPerBlock);

    // Prepare degrade processor
    degradeProcessor.prepare(sampleRate, samplesPerBlock);

    // Prepare saturation processor
    saturationProcessor.prepare(sampleRate, samplesPerBlock);

    // Prepare sub bass processor
    subBassProcessor.prepare(sampleRate, samplesPerBlock);

    // Prepare reverb processor
    reverbProcessor.prepare(sampleRate, samplesPerBlock);

    // Prepare micro looper
    microLooper.prepare(sampleRate, samplesPerBlock);
}

void LoopEngineProcessor::releaseResources()
{
    delayLineL.clear();
    delayLineR.clear();
}

bool LoopEngineProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
        && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;

    return true;
}

void LoopEngineProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused(midiMessages);

    juce::ScopedNoDenormals noDenormals;
    const auto totalNumInputChannels = getTotalNumInputChannels();
    const auto totalNumOutputChannels = getTotalNumOutputChannels();
    const auto numSamples = buffer.getNumSamples();

    // Track host tempo and transport state
    if (auto* playHead = getPlayHead())
    {
        if (auto posInfo = playHead->getPosition())
        {
            if (posInfo->getBpm())
            {
                float bpm = static_cast<float>(*posInfo->getBpm());
                lastHostBpm.store(bpm);
                loopEngine.setHostBpm(bpm);
            }

            // Track host playing state for transport sync
            bool hostPlaying = posInfo->getIsPlaying();
            bool wasPlaying = lastHostPlaying.exchange(hostPlaying);

            // If host transport sync is enabled, control looper based on host state
            if (hostTransportSyncEnabled.load())
            {
                if (hostPlaying && !wasPlaying)
                {
                    // Host started playing - start looper playback if we have content
                    if (loopEngine.hasContent() && loopEngine.getState() == LoopBuffer::State::Idle)
                    {
                        loopEngine.play();
                    }
                }
                else if (!hostPlaying && wasPlaying)
                {
                    // Host stopped - stop looper
                    if (loopEngine.getState() == LoopBuffer::State::Playing ||
                        loopEngine.getState() == LoopBuffer::State::Overdubbing)
                    {
                        loopEngine.stop();
                    }
                }
            }
        }
    }

    // Clear any output channels that don't have input data
    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear(i, 0, numSamples);

    // Update loop engine parameters
    if (auto* loopStartParam = apvts.getRawParameterValue("loopStart"))
        loopEngine.setLoopStart(loopStartParam->load());
    if (auto* loopEndParam = apvts.getRawParameterValue("loopEnd"))
        loopEngine.setLoopEnd(loopEndParam->load());
    if (auto* loopSpeedParam = apvts.getRawParameterValue("loopSpeed"))
        loopEngine.setSpeed(loopSpeedParam->load());
    if (auto* loopReverseParam = apvts.getRawParameterValue("loopReverse"))
        loopEngine.setReverse(loopReverseParam->load() > 0.5f);
    if (auto* loopPitchParam = apvts.getRawParameterValue("loopPitch"))
        loopEngine.setPitchShift(loopPitchParam->load());
    if (auto* loopFadeParam = apvts.getRawParameterValue("loopFade"))
        loopEngine.setFade(loopFadeParam->load() / 100.0f);  // Convert 0-100% to 0-1

    // Process through loop engine with separate buffers for Blooper-style degrade
    // loopPlaybackBuffer contains ONLY the loop playback (for degrade processing)
    // inputPassthroughBuffer contains ONLY the clean input (bypasses degrade)
    loopEngine.processBlock(buffer, &loopPlaybackBuffer, &inputPassthroughBuffer);

    // Update degrade processor parameters
    degradeProcessor.setHighPassFreq(degradeHPParam->load());
    degradeProcessor.setHighPassQ(degradeHPQParam->load());
    degradeProcessor.setLowPassFreq(degradeLPParam->load());
    degradeProcessor.setLowPassQ(degradeLPQParam->load());
    degradeProcessor.setBitDepth(degradeBitParam->load());
    degradeProcessor.setSampleRateReduction(degradeSRParam->load());
    degradeProcessor.setWobble(degradeWobbleParam->load() / 100.0f);  // Convert 0-100% to 0-1
    degradeProcessor.setVinyl(degradeVinylParam->load() / 100.0f);  // Convert 0-100% to 0-1
    degradeProcessor.setMix(degradeMixParam->load() / 100.0f);  // Convert 0-100% to 0-1

    // Update micro looper parameters
    microLooper.setClock(microClockParam->load() / 100.0f);  // Convert 0-100% to 0-1
    microLooper.setLength(microLengthParam->load() / 100.0f);  // Convert 5-100% to 0.05-1.0
    microLooper.setModify(microModifyParam->load() / 100.0f);  // Convert 0-100% to 0-1
    microLooper.setSpeed(microSpeedParam->load() / 100.0f);  // Convert 0-100% to 0-1
    microLooper.setMix(microMixParam->load() / 100.0f);  // Convert 0-100% to 0-1

    // Update saturation processor parameters
    saturationProcessor.setMix(satMixParam->load() / 100.0f);
    // Soft type params
    saturationProcessor.setSoftDrive(satSoftDriveParam->load() / 100.0f);
    saturationProcessor.setSoftTone(satSoftToneParam->load() / 100.0f);
    saturationProcessor.setSoftCurve(satSoftCurveParam->load() / 100.0f);
    // Tape type params
    saturationProcessor.setTapeDrive(satTapeDriveParam->load() / 100.0f);
    saturationProcessor.setTapeBias(satTapeBiasParam->load() / 100.0f);
    saturationProcessor.setTapeFlutter(satTapeFlutterParam->load() / 100.0f);
    saturationProcessor.setTapeTone(satTapeToneParam->load() / 100.0f);
    // Tube type params
    saturationProcessor.setTubeDrive(satTubeDriveParam->load() / 100.0f);
    saturationProcessor.setTubeBias(satTubeBiasParam->load() / 100.0f);
    saturationProcessor.setTubeWarmth(satTubeWarmthParam->load() / 100.0f);
    saturationProcessor.setTubeSag(satTubeSagParam->load() / 100.0f);
    // Fuzz type params
    saturationProcessor.setFuzzDrive(satFuzzDriveParam->load() / 100.0f);
    saturationProcessor.setFuzzGate(satFuzzGateParam->load() / 100.0f);
    saturationProcessor.setFuzzOctave(satFuzzOctaveParam->load() / 100.0f);
    saturationProcessor.setFuzzTone(satFuzzToneParam->load() / 100.0f);

    // Signal flow: Loop/MixBus -> Saturation -> Degrade -> Reverb -> Delay

    // Inject MixBus content into loopPlaybackBuffer BEFORE effects
    // This ensures MixBus audio goes through the full effects chain (Sat, Degrade, Reverb)
    // Where MixBus has content, it replaces layers; where empty, layers "poke through"
    loopEngine.injectMixBusToPlayback(loopPlaybackBuffer, numSamples);

    // Apply saturation ONLY to the loop playback buffer (before degrade)
    if (saturationProcessor.isEnabled())
    {
        saturationProcessor.processBlock(loopPlaybackBuffer);
    }

    // Blooper-style degrade: only affects loop playback, not input
    // Apply degrade ONLY to the loop playback buffer
    if (degradeProcessor.isEnabled())
    {
        degradeProcessor.processBlock(loopPlaybackBuffer);
    }

    // Apply reverb processing (Spring/Plate/Hall algorithms) - to loop playback only
    if (reverbProcessor.getEnabled())
    {
        // Update reverb parameters from APVTS
        if (reverbSizeParam) reverbProcessor.setSize(reverbSizeParam->load() / 100.0f);
        if (reverbDecayParam) reverbProcessor.setDecay(reverbDecayParam->load() / 100.0f);
        if (reverbDampParam) reverbProcessor.setDamping(reverbDampParam->load() / 100.0f);
        if (reverbMixParam) reverbProcessor.setMix(reverbMixParam->load() / 100.0f);
        if (reverbWidthParam) reverbProcessor.setWidth(reverbWidthParam->load() / 100.0f);
        if (reverbPreDelayParam) reverbProcessor.setPreDelay(reverbPreDelayParam->load() / 100.0f);
        if (reverbModRateParam) reverbProcessor.setModRate(reverbModRateParam->load() / 100.0f);
        if (reverbModDepthParam) reverbProcessor.setModDepth(reverbModDepthParam->load() / 100.0f);

        reverbProcessor.processBlock(loopPlaybackBuffer);
    }

    // Sub bass processing moved to after signal combination (see below)

    // Process through micro looper (always-listening buffer)
    // The micro looper processes the combined signal (after looper and degrade)
    if (microLooper.isEnabled())
    {
        // Combine loop playback with input for micro looper processing
        juce::AudioBuffer<float> microLooperInput(buffer.getNumChannels(), numSamples);
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            float* dest = microLooperInput.getWritePointer(ch);
            const float* loopData = loopPlaybackBuffer.getReadPointer(ch);
            const float* inputData = inputPassthroughBuffer.getReadPointer(ch);
            for (int i = 0; i < numSamples; ++i)
            {
                dest[i] = loopData[i] + inputData[i];
            }
        }

        // Process through micro looper
        microLooper.processBlock(microLooperInput);

        // Copy result back to main buffer
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            buffer.copyFrom(ch, 0, microLooperInput, ch, 0, numSamples);
        }
    }
    else
    {
        // No micro looper - reconstruct from loop + input
        const int numChannels = buffer.getNumChannels();
        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* outputData = buffer.getWritePointer(ch);
            const float* loopData = loopPlaybackBuffer.getReadPointer(ch);
            const float* cleanInputData = inputPassthroughBuffer.getReadPointer(ch);
            for (int i = 0; i < numSamples; ++i)
            {
                outputData[i] = loopData[i] + cleanInputData[i];
            }
        }
    }

    // NOTE: MixBus injection now happens BEFORE effects (see line ~717)
    // so MixBus audio goes through Saturation -> Degrade -> Reverb
    // This enables compound mode to accumulate effects each cycle

    // Capture for additive recording if active
    // This captures AFTER effects (sat, degrade, reverb) but BEFORE delay
    // "What you hear is what you get" - effected loop + input combined
    if (loopEngine.isAdditiveRecordingActive())
    {
        loopEngine.captureForAdditive(buffer, numSamples);
    }

    // Capture for MixBus recording if active
    // Same capture point as additive - after effects, before delay
    if (loopEngine.isMixBusRecording())
    {
        loopEngine.captureForMixBus(buffer, numSamples);
    }

    // Apply sub bass processing (octave-down generator) to the COMBINED output
    // This runs on the final mixed signal (loop + input) so it always has content
    if (subBassProcessor.getEnabled())
    {
        // Update parameters from APVTS (convert 0-100 to 0-1)
        if (subBassFreqParam) subBassProcessor.setFrequency(subBassFreqParam->load() / 100.0f);
        if (subBassAmountParam) subBassProcessor.setAmount(subBassAmountParam->load() / 100.0f);

        subBassProcessor.processBlock(buffer);
    }

    // Get delay time - either from parameter or tempo sync
    const float delayTime = tempoSyncEnabled.load()
        ? calculateSyncedDelayTime()
        : delayTimeParam->load();
    const float feedback = feedbackParam->load();
    const float mix = mixParam->load() / 100.0f; // Convert to 0-1
    const float tone = toneParam->load();

    // Get BBD character parameters
    const float age = ageParam->load();
    const float modRate = modRateParam->load();
    const float modDepth = modDepthParam->load();
    const float warmth = warmthParam->load();

    // Update delay line parameters
    delayLineL.setDelayTime(delayTime);
    delayLineL.setFeedback(feedback);
    delayLineL.setTone(tone);
    delayLineL.setAge(age);
    delayLineL.setModRate(modRate);
    delayLineL.setModDepth(modDepth);
    delayLineL.setWarmth(warmth);

    delayLineR.setDelayTime(delayTime);
    delayLineR.setFeedback(feedback);
    delayLineR.setTone(tone);
    delayLineR.setAge(age);
    delayLineR.setModRate(modRate);
    delayLineR.setModDepth(modDepth);
    delayLineR.setWarmth(warmth);

    // Process audio through delay (if enabled)
    if (delayEnabled.load())
    {
        if (totalNumInputChannels >= 1)
        {
            auto* channelL = buffer.getWritePointer(0);

            for (int sample = 0; sample < numSamples; ++sample)
            {
                const float dry = channelL[sample];
                const float wet = delayLineL.processSample(dry);
                channelL[sample] = dry * (1.0f - mix) + wet * mix;
            }
        }

        if (totalNumInputChannels >= 2)
        {
            auto* channelR = buffer.getWritePointer(1);

            for (int sample = 0; sample < numSamples; ++sample)
            {
                const float dry = channelR[sample];
                const float wet = delayLineR.processSample(dry);
                channelR[sample] = dry * (1.0f - mix) + wet * mix;
            }
        }
    }

    // Final safety pass: sanitize any NaN/Inf values and apply soft limiting
    // This prevents buzzing from bad data after state transitions (STOP, etc.)
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        float* data = buffer.getWritePointer(ch);
        for (int i = 0; i < numSamples; ++i)
        {
            float sample = data[i];

            // Check for NaN or Inf - reset to zero
            if (std::isnan(sample) || std::isinf(sample))
            {
                data[i] = 0.0f;
                continue;
            }

            // Soft limit extreme values to prevent harsh clipping
            if (std::abs(sample) > 0.95f)
            {
                data[i] = std::tanh(sample * 1.2f) * 0.95f;
            }
        }
    }
}

void LoopEngineProcessor::setTempoSync(bool enabled)
{
    tempoSyncEnabled.store(enabled);
}

bool LoopEngineProcessor::getTempoSyncEnabled() const
{
    return tempoSyncEnabled.load();
}

void LoopEngineProcessor::setTempoNote(int noteIndex)
{
    tempoNoteValue.store(std::clamp(noteIndex, 0, 5));
}

int LoopEngineProcessor::getTempoNoteValue() const
{
    return tempoNoteValue.load();
}

float LoopEngineProcessor::getHostBpm() const
{
    return lastHostBpm.load();
}

float LoopEngineProcessor::calculateSyncedDelayTime() const
{
    // Note value multipliers relative to quarter note
    // 0=1/4, 1=1/8, 2=1/8T, 3=1/16, 4=1/16T, 5=1/32
    static constexpr float noteMultipliers[] = {
        1.0f,      // 1/4
        0.5f,      // 1/8
        0.333333f, // 1/8T (triplet)
        0.25f,     // 1/16
        0.166667f, // 1/16T (triplet)
        0.125f     // 1/32
    };

    const float bpm = lastHostBpm.load();
    if (bpm <= 0.0f)
        return 300.0f; // Default fallback

    const int noteIdx = std::clamp(tempoNoteValue.load(), 0, 5);
    const float multiplier = noteMultipliers[noteIdx];

    // Quarter note duration in ms = 60000 / BPM
    const float quarterNoteMs = 60000.0f / bpm;

    return std::clamp(quarterNoteMs * multiplier, 1.0f, 2000.0f);
}

void LoopEngineProcessor::setDelayEnabled(bool enabled)
{
    delayEnabled.store(enabled);
}

bool LoopEngineProcessor::getDelayEnabled() const
{
    return delayEnabled.load();
}

void LoopEngineProcessor::setDegradeEnabled(bool enabled)
{
    degradeProcessor.setEnabled(enabled);
}

bool LoopEngineProcessor::getDegradeEnabled() const
{
    return degradeProcessor.isEnabled();
}

void LoopEngineProcessor::setDegradeFilterEnabled(bool enabled)
{
    degradeProcessor.setFilterEnabled(enabled);
}

void LoopEngineProcessor::setDegradeLofiEnabled(bool enabled)
{
    degradeProcessor.setLofiEnabled(enabled);
}

void LoopEngineProcessor::setMicroLooperEnabled(bool enabled)
{
    microLooper.setEnabled(enabled);
}

bool LoopEngineProcessor::getDegradeFilterEnabled() const
{
    return degradeProcessor.getFilterEnabled();
}

bool LoopEngineProcessor::getDegradeLofiEnabled() const
{
    return degradeProcessor.getLofiEnabled();
}

bool LoopEngineProcessor::getMicroLooperEnabled() const
{
    return microLooper.isEnabled();
}

void LoopEngineProcessor::setDegradeHPEnabled(bool enabled)
{
    degradeProcessor.setHPEnabled(enabled);
}

void LoopEngineProcessor::setDegradeLPEnabled(bool enabled)
{
    degradeProcessor.setLPEnabled(enabled);
}

bool LoopEngineProcessor::getDegradeHPEnabled() const
{
    return degradeProcessor.getHPEnabled();
}

bool LoopEngineProcessor::getDegradeLPEnabled() const
{
    return degradeProcessor.getLPEnabled();
}

void LoopEngineProcessor::setSaturationEnabled(bool enabled)
{
    saturationProcessor.setEnabled(enabled);
}

bool LoopEngineProcessor::getSaturationEnabled() const
{
    return saturationProcessor.isEnabled();
}

void LoopEngineProcessor::setSaturationType(int type)
{
    saturationProcessor.setType(type);
}

int LoopEngineProcessor::getSaturationType() const
{
    return static_cast<int>(saturationProcessor.getType());
}

void LoopEngineProcessor::setSubBassEnabled(bool enabled)
{
    subBassProcessor.setEnabled(enabled);
}

bool LoopEngineProcessor::getSubBassEnabled() const
{
    return subBassProcessor.getEnabled();
}

void LoopEngineProcessor::setReverbEnabled(bool enabled)
{
    reverbProcessor.setEnabled(enabled);
}

bool LoopEngineProcessor::getReverbEnabled() const
{
    return reverbProcessor.getEnabled();
}

void LoopEngineProcessor::setReverbType(int type)
{
    reverbProcessor.setAlgorithm(type);
}

int LoopEngineProcessor::getReverbType() const
{
    return static_cast<int>(reverbProcessor.getAlgorithm());
}

void LoopEngineProcessor::setHostTransportSync(bool enabled)
{
    hostTransportSyncEnabled.store(enabled);
}

bool LoopEngineProcessor::getHostTransportSync() const
{
    return hostTransportSyncEnabled.load();
}

bool LoopEngineProcessor::isHostPlaying() const
{
    return lastHostPlaying.load();
}

bool LoopEngineProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* LoopEngineProcessor::createEditor()
{
    return new LoopEngineEditor(*this);
}

void LoopEngineProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

void LoopEngineProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));

    if (xmlState != nullptr && xmlState->hasTagName(apvts.state.getType()))
        apvts.replaceState(juce::ValueTree::fromXml(*xmlState));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new LoopEngineProcessor();
}
