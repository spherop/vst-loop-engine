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
    juce::WebSliderRelay degradeScrambleAmtRelay { "degradeScrambleAmt" };
    juce::WebSliderRelay degradeSmearRelay { "degradeSmear" };
    juce::WebSliderRelay degradeGrainSizeRelay { "degradeGrainSize" };
    juce::WebSliderRelay degradeMixRelay { "degradeMix" };

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
    juce::WebSliderParameterAttachment degradeScrambleAmtAttachment;
    juce::WebSliderParameterAttachment degradeSmearAttachment;
    juce::WebSliderParameterAttachment degradeGrainSizeAttachment;
    juce::WebSliderParameterAttachment degradeMixAttachment;

    std::optional<juce::WebBrowserComponent::Resource> getResource(const juce::String& url);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LoopEngineEditor)
};
