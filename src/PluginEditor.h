#pragma once

#include "PluginProcessor.h"
#include <juce_gui_extra/juce_gui_extra.h>

class FuzzDelayEditor : public juce::AudioProcessorEditor
{
public:
    explicit FuzzDelayEditor(FuzzDelayProcessor&);
    ~FuzzDelayEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    FuzzDelayProcessor& processorRef;

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

    std::optional<juce::WebBrowserComponent::Resource> getResource(const juce::String& url);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FuzzDelayEditor)
};
