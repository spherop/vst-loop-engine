#include "PluginEditor.h"
#include "BinaryData.h"

FuzzDelayEditor::FuzzDelayEditor(FuzzDelayProcessor& p)
    : AudioProcessorEditor(&p),
      processorRef(p),
      webView(juce::WebBrowserComponent::Options()
                  .withBackend(juce::WebBrowserComponent::Options::Backend::webview2)
                  .withWinWebView2Options(
                      juce::WebBrowserComponent::Options::WinWebView2()
                          .withUserDataFolder(juce::File::getSpecialLocation(
                              juce::File::tempDirectory)))
                  .withKeepPageLoadedWhenBrowserIsHidden()
                  .withNativeIntegrationEnabled()
                  .withOptionsFrom(delayTimeRelay)
                  .withOptionsFrom(feedbackRelay)
                  .withOptionsFrom(mixRelay)
                  .withOptionsFrom(toneRelay)
                  .withResourceProvider(
                      [this](const auto& url) { return getResource(url); },
                      juce::URL("http://localhost/").getOrigin())),
      delayTimeAttachment(*processorRef.getAPVTS().getParameter("delayTime"),
                          delayTimeRelay,
                          processorRef.getAPVTS().undoManager),
      feedbackAttachment(*processorRef.getAPVTS().getParameter("feedback"),
                         feedbackRelay,
                         processorRef.getAPVTS().undoManager),
      mixAttachment(*processorRef.getAPVTS().getParameter("mix"),
                    mixRelay,
                    processorRef.getAPVTS().undoManager),
      toneAttachment(*processorRef.getAPVTS().getParameter("tone"),
                     toneRelay,
                     processorRef.getAPVTS().undoManager)
{
    addAndMakeVisible(webView);
    webView.goToURL(juce::WebBrowserComponent::getResourceProviderRoot());

    setSize(600, 400);
    setResizable(true, true);
    setResizeLimits(400, 300, 1200, 800);
}

FuzzDelayEditor::~FuzzDelayEditor()
{
}

void FuzzDelayEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black);
}

void FuzzDelayEditor::resized()
{
    webView.setBounds(getLocalBounds());
}

std::optional<juce::WebBrowserComponent::Resource> FuzzDelayEditor::getResource(const juce::String& url)
{
    const auto urlToRetrieve = url == "/" ? juce::String("index.html") : url.fromFirstOccurrenceOf("/", false, false);

    // Map URL paths to BinaryData resources
    static const std::map<juce::String, std::pair<const char*, int>> resourceMap = {
        {"index.html", {BinaryData::index_html, BinaryData::index_htmlSize}},
        {"styles.css", {BinaryData::styles_css, BinaryData::styles_cssSize}},
        {"main.js", {BinaryData::main_js, BinaryData::main_jsSize}}
    };

    auto it = resourceMap.find(urlToRetrieve);
    if (it == resourceMap.end())
        return std::nullopt;

    const auto& [data, size] = it->second;

    // Determine MIME type
    juce::String mimeType = "application/octet-stream";
    if (urlToRetrieve.endsWith(".html"))
        mimeType = "text/html";
    else if (urlToRetrieve.endsWith(".css"))
        mimeType = "text/css";
    else if (urlToRetrieve.endsWith(".js"))
        mimeType = "text/javascript";

    return juce::WebBrowserComponent::Resource{
        std::vector<std::byte>(reinterpret_cast<const std::byte*>(data),
                               reinterpret_cast<const std::byte*>(data) + size),
        mimeType.toStdString()
    };
}
