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

    std::optional<juce::WebBrowserComponent::Resource> getResource(const juce::String& url);

    // File chooser (must persist during async operation)
    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LoopEngineEditor)
};
