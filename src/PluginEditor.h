#pragma once

#include "PluginProcessor.h"
#include <juce_gui_extra/juce_gui_extra.h>

class LoopEngineEditor : public juce::AudioProcessorEditor,
                        private juce::Timer
{
public:
    explicit LoopEngineEditor(LoopEngineProcessor&);
    ~LoopEngineEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    LoopEngineProcessor& processorRef;

    // Parameter relays for C++ <-> JavaScript communication
    // Must be declared before webView so they exist when webView is constructed
    juce::WebSliderRelay delayTimeRelay { "delayTime" };
    juce::WebSliderRelay feedbackRelay { "feedback" };
    juce::WebSliderRelay mixRelay { "mix" };
    juce::WebSliderRelay toneRelay { "tone" };

    // BBD Character relays
    juce::WebSliderRelay ageRelay { "age" };
    juce::WebSliderRelay modRateRelay { "modRate" };
    juce::WebSliderRelay modDepthRelay { "modDepth" };
    juce::WebSliderRelay warmthRelay { "warmth" };

    // Loop parameter relays
    juce::WebSliderRelay loopStartRelay { "loopStart" };
    juce::WebSliderRelay loopEndRelay { "loopEnd" };
    juce::WebSliderRelay loopSpeedRelay { "loopSpeed" };
    juce::WebSliderRelay loopPitchRelay { "loopPitch" };
    juce::WebSliderRelay loopFadeRelay { "loopFade" };

    // Degrade parameter relays
    juce::WebSliderRelay degradeHPRelay { "degradeHP" };
    juce::WebSliderRelay degradeHPQRelay { "degradeHPQ" };
    juce::WebSliderRelay degradeLPRelay { "degradeLP" };
    juce::WebSliderRelay degradeLPQRelay { "degradeLPQ" };
    juce::WebSliderRelay degradeBitRelay { "degradeBit" };
    juce::WebSliderRelay degradeSRRelay { "degradeSR" };
    juce::WebSliderRelay degradeWobbleRelay { "degradeWobble" };
    juce::WebSliderRelay degradeVinylRelay { "degradeVinyl" };
    juce::WebSliderRelay degradeMixRelay { "degradeMix" };

    // Micro looper parameter relays
    juce::WebSliderRelay microClockRelay { "microClock" };
    juce::WebSliderRelay microLengthRelay { "microLength" };
    juce::WebSliderRelay microModifyRelay { "microModify" };
    juce::WebSliderRelay microSpeedRelay { "microSpeed" };
    juce::WebSliderRelay microMixRelay { "microMix" };

    // Saturation parameter relays
    juce::WebSliderRelay satMixRelay { "satMix" };
    // Soft type
    juce::WebSliderRelay satSoftDriveRelay { "satSoftDrive" };
    juce::WebSliderRelay satSoftToneRelay { "satSoftTone" };
    juce::WebSliderRelay satSoftCurveRelay { "satSoftCurve" };
    // Tape type
    juce::WebSliderRelay satTapeDriveRelay { "satTapeDrive" };
    juce::WebSliderRelay satTapeBiasRelay { "satTapeBias" };
    juce::WebSliderRelay satTapeFlutterRelay { "satTapeFlutter" };
    juce::WebSliderRelay satTapeToneRelay { "satTapeTone" };
    // Tube type
    juce::WebSliderRelay satTubeDriveRelay { "satTubeDrive" };
    juce::WebSliderRelay satTubeBiasRelay { "satTubeBias" };
    juce::WebSliderRelay satTubeWarmthRelay { "satTubeWarmth" };
    juce::WebSliderRelay satTubeSagRelay { "satTubeSag" };
    // Fuzz type
    juce::WebSliderRelay satFuzzDriveRelay { "satFuzzDrive" };
    juce::WebSliderRelay satFuzzGateRelay { "satFuzzGate" };
    juce::WebSliderRelay satFuzzOctaveRelay { "satFuzzOctave" };
    juce::WebSliderRelay satFuzzToneRelay { "satFuzzTone" };

    // Sub Bass parameter relays
    juce::WebSliderRelay subBassFreqRelay { "subBassFreq" };
    juce::WebSliderRelay subBassAmountRelay { "subBassAmount" };

    // Reverb parameter relays
    juce::WebSliderRelay reverbSizeRelay { "reverbSize" };
    juce::WebSliderRelay reverbDecayRelay { "reverbDecay" };
    juce::WebSliderRelay reverbDampRelay { "reverbDamp" };
    juce::WebSliderRelay reverbMixRelay { "reverbMix" };
    juce::WebSliderRelay reverbWidthRelay { "reverbWidth" };
    juce::WebSliderRelay reverbPreDelayRelay { "reverbPreDelay" };
    juce::WebSliderRelay reverbModRateRelay { "reverbModRate" };
    juce::WebSliderRelay reverbModDepthRelay { "reverbModDepth" };

    juce::WebBrowserComponent webView;

    // Parameter attachments
    juce::WebSliderParameterAttachment delayTimeAttachment;
    juce::WebSliderParameterAttachment feedbackAttachment;
    juce::WebSliderParameterAttachment mixAttachment;
    juce::WebSliderParameterAttachment toneAttachment;

    // BBD Character attachments
    juce::WebSliderParameterAttachment ageAttachment;
    juce::WebSliderParameterAttachment modRateAttachment;
    juce::WebSliderParameterAttachment modDepthAttachment;
    juce::WebSliderParameterAttachment warmthAttachment;

    // Loop parameter attachments
    juce::WebSliderParameterAttachment loopStartAttachment;
    juce::WebSliderParameterAttachment loopEndAttachment;
    juce::WebSliderParameterAttachment loopSpeedAttachment;
    juce::WebSliderParameterAttachment loopPitchAttachment;
    juce::WebSliderParameterAttachment loopFadeAttachment;

    // Degrade parameter attachments
    juce::WebSliderParameterAttachment degradeHPAttachment;
    juce::WebSliderParameterAttachment degradeHPQAttachment;
    juce::WebSliderParameterAttachment degradeLPAttachment;
    juce::WebSliderParameterAttachment degradeLPQAttachment;
    juce::WebSliderParameterAttachment degradeBitAttachment;
    juce::WebSliderParameterAttachment degradeSRAttachment;
    juce::WebSliderParameterAttachment degradeWobbleAttachment;
    juce::WebSliderParameterAttachment degradeVinylAttachment;
    juce::WebSliderParameterAttachment degradeMixAttachment;

    // Micro looper parameter attachments
    juce::WebSliderParameterAttachment microClockAttachment;
    juce::WebSliderParameterAttachment microLengthAttachment;
    juce::WebSliderParameterAttachment microModifyAttachment;
    juce::WebSliderParameterAttachment microSpeedAttachment;
    juce::WebSliderParameterAttachment microMixAttachment;

    // Saturation parameter attachments
    juce::WebSliderParameterAttachment satMixAttachment;
    // Soft type
    juce::WebSliderParameterAttachment satSoftDriveAttachment;
    juce::WebSliderParameterAttachment satSoftToneAttachment;
    juce::WebSliderParameterAttachment satSoftCurveAttachment;
    // Tape type
    juce::WebSliderParameterAttachment satTapeDriveAttachment;
    juce::WebSliderParameterAttachment satTapeBiasAttachment;
    juce::WebSliderParameterAttachment satTapeFlutterAttachment;
    juce::WebSliderParameterAttachment satTapeToneAttachment;
    // Tube type
    juce::WebSliderParameterAttachment satTubeDriveAttachment;
    juce::WebSliderParameterAttachment satTubeBiasAttachment;
    juce::WebSliderParameterAttachment satTubeWarmthAttachment;
    juce::WebSliderParameterAttachment satTubeSagAttachment;
    // Fuzz type
    juce::WebSliderParameterAttachment satFuzzDriveAttachment;
    juce::WebSliderParameterAttachment satFuzzGateAttachment;
    juce::WebSliderParameterAttachment satFuzzOctaveAttachment;
    juce::WebSliderParameterAttachment satFuzzToneAttachment;

    // Sub Bass parameter attachments
    juce::WebSliderParameterAttachment subBassFreqAttachment;
    juce::WebSliderParameterAttachment subBassAmountAttachment;

    // Reverb parameter attachments
    juce::WebSliderParameterAttachment reverbSizeAttachment;
    juce::WebSliderParameterAttachment reverbDecayAttachment;
    juce::WebSliderParameterAttachment reverbDampAttachment;
    juce::WebSliderParameterAttachment reverbMixAttachment;
    juce::WebSliderParameterAttachment reverbWidthAttachment;
    juce::WebSliderParameterAttachment reverbPreDelayAttachment;
    juce::WebSliderParameterAttachment reverbModRateAttachment;
    juce::WebSliderParameterAttachment reverbModDepthAttachment;

    std::optional<juce::WebBrowserComponent::Resource> getResource(const juce::String& url);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LoopEngineEditor)
};
