#include "PluginEditor.h"
#include "BinaryData.h"

// Set to true to load UI files from disk (enables hot reload during development)
// Set to false for production builds (loads from BinaryData)
#define LOOP_ENGINE_DEV_MODE 1

#if LOOP_ENGINE_DEV_MODE
// Path to the ui folder - change this to match your project location
static const char* DEV_UI_PATH = "/Users/danielnewman/Documents/code/vst-loop-engine/ui/";
#endif

LoopEngineEditor::LoopEngineEditor(LoopEngineProcessor& p)
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
                  .withOptionsFrom(ageRelay)
                  .withOptionsFrom(modRateRelay)
                  .withOptionsFrom(modDepthRelay)
                  .withOptionsFrom(warmthRelay)
                  .withOptionsFrom(loopStartRelay)
                  .withOptionsFrom(loopEndRelay)
                  .withOptionsFrom(loopSpeedRelay)
                  .withNativeFunction("loopRecord", [this](const juce::Array<juce::var>&, auto complete)
                  {
                      processorRef.getLoopEngine().record();
                      complete({});
                  })
                  .withNativeFunction("loopPlay", [this](const juce::Array<juce::var>&, auto complete)
                  {
                      processorRef.getLoopEngine().play();
                      complete({});
                  })
                  .withNativeFunction("loopStop", [this](const juce::Array<juce::var>&, auto complete)
                  {
                      processorRef.getLoopEngine().stop();
                      complete({});
                  })
                  .withNativeFunction("loopOverdub", [this](const juce::Array<juce::var>&, auto complete)
                  {
                      processorRef.getLoopEngine().overdub();
                      complete({});
                  })
                  .withNativeFunction("loopUndo", [this](const juce::Array<juce::var>&, auto complete)
                  {
                      processorRef.getLoopEngine().undo();
                      complete({});
                  })
                  .withNativeFunction("loopClear", [this](const juce::Array<juce::var>&, auto complete)
                  {
                      processorRef.getLoopEngine().clear();
                      complete({});
                  })
                  .withNativeFunction("loopJumpToLayer", [this](const juce::Array<juce::var>& args, auto complete)
                  {
                      if (args.size() > 0)
                          processorRef.getLoopEngine().jumpToLayer(static_cast<int>(args[0]) - 1);  // 1-indexed from UI
                      complete({});
                  })
                  .withNativeFunction("setLoopLengthBars", [this](const juce::Array<juce::var>& args, auto complete)
                  {
                      if (args.size() > 0)
                      {
                          int bars = static_cast<int>(args[0]);
                          processorRef.getLoopEngine().setLoopLengthBars(bars);
                          DBG("setLoopLengthBars: " + juce::String(bars));
                      }
                      complete({});
                  })
                  .withNativeFunction("setLoopReverse", [this](const juce::Array<juce::var>& args, auto complete)
                  {
                      DBG("========================================");
                      DBG("setLoopReverse NATIVE FUNCTION CALLED!");
                      DBG("Number of args: " + juce::String(args.size()));
                      if (args.size() > 0)
                      {
                          bool reversed = static_cast<bool>(args[0]);
                          DBG("Parsed reversed value: " + juce::String(reversed ? "TRUE" : "FALSE"));

                          // Set directly on loop engine
                          processorRef.getLoopEngine().setReverse(reversed);
                          DBG("Called loopEngine.setReverse(" + juce::String(reversed ? "true" : "false") + ")");

                          // Verify engine state
                          DBG("Engine isReversed now: " + juce::String(processorRef.getLoopEngine().getIsReversed() ? "TRUE" : "FALSE"));

                          // Also update the APVTS parameter
                          if (auto* param = processorRef.getAPVTS().getParameter("loopReverse"))
                          {
                              float paramValue = reversed ? 1.0f : 0.0f;
                              DBG("Setting APVTS loopReverse param to: " + juce::String(paramValue));
                              param->setValueNotifyingHost(paramValue);
                          }
                          else
                          {
                              DBG("ERROR: Could not find loopReverse parameter!");
                          }
                      }
                      else
                      {
                          DBG("ERROR: No arguments provided to setLoopReverse!");
                      }
                      DBG("========================================");
                      complete({});
                  })
                  .withNativeFunction("resetLoopParams", [this](const juce::Array<juce::var>&, auto complete)
                  {
                      DBG("resetLoopParams called");
                      // Reset loop parameters to defaults (called when UI opens)
                      processorRef.getLoopEngine().resetLoopParams();

                      // Also reset the APVTS parameters to trigger UI updates
                      if (auto* param = processorRef.getAPVTS().getParameter("loopStart"))
                          param->setValueNotifyingHost(0.0f);
                      if (auto* param = processorRef.getAPVTS().getParameter("loopEnd"))
                          param->setValueNotifyingHost(1.0f);
                      if (auto* param = processorRef.getAPVTS().getParameter("loopSpeed"))
                          param->setValueNotifyingHost(param->convertTo0to1(1.0f));
                      if (auto* param = processorRef.getAPVTS().getParameter("loopReverse"))
                          param->setValueNotifyingHost(0.0f);

                      DBG("resetLoopParams complete");
                      complete({});
                  })
                  .withNativeFunction("getLoopState", [this](const juce::Array<juce::var>&, auto complete)
                  {
                      const auto& loopEngine = processorRef.getLoopEngine();

                      juce::DynamicObject::Ptr result = new juce::DynamicObject();
                      result->setProperty("state", static_cast<int>(loopEngine.getState()));
                      result->setProperty("layer", loopEngine.getCurrentLayer());
                      result->setProperty("highestLayer", loopEngine.getHighestLayer());
                      result->setProperty("playhead", loopEngine.getPlayheadPosition());
                      result->setProperty("loopLength", loopEngine.getLoopLengthSeconds());
                      result->setProperty("hasContent", loopEngine.hasContent());
                      result->setProperty("isReversed", loopEngine.getIsReversed());

                      // Get waveform data (100 points for visualization)
                      auto waveformData = loopEngine.getWaveformData(100);
                      juce::Array<juce::var> waveformArray;
                      for (float val : waveformData)
                          waveformArray.add(val);
                      result->setProperty("waveform", waveformArray);

                      complete(juce::var(result.get()));
                  })
                  .withNativeFunction("triggerTestSound", [this](const juce::Array<juce::var>& args, auto complete)
                  {
                      if (args.size() > 0)
                          processorRef.triggerTestSound(static_cast<int>(args[0]));
                      complete({});
                  })
                  .withNativeFunction("stopTestSound", [this](const juce::Array<juce::var>&, auto complete)
                  {
                      processorRef.stopTestSound();
                      complete({});
                  })
                  .withNativeFunction("setLoopEnabled", [this](const juce::Array<juce::var>& args, auto complete)
                  {
                      if (args.size() > 0)
                          processorRef.setLoopEnabled(static_cast<bool>(args[0]));
                      complete({});
                  })
                  .withNativeFunction("setTempoSync", [this](const juce::Array<juce::var>& args, auto complete)
                  {
                      if (args.size() > 0)
                          processorRef.setTempoSync(static_cast<bool>(args[0]));
                      complete({});
                  })
                  .withNativeFunction("setTempoNote", [this](const juce::Array<juce::var>& args, auto complete)
                  {
                      if (args.size() > 0)
                          processorRef.setTempoNote(static_cast<int>(args[0]));
                      complete({});
                  })
                  .withNativeFunction("getTempoState", [this](const juce::Array<juce::var>&, auto complete)
                  {
                      juce::DynamicObject::Ptr result = new juce::DynamicObject();
                      result->setProperty("bpm", processorRef.getHostBpm());
                      result->setProperty("syncEnabled", processorRef.getTempoSyncEnabled());
                      result->setProperty("noteValue", processorRef.getTempoNoteValue());
                      result->setProperty("loopEnabled", processorRef.getLoopEnabled());
                      result->setProperty("delayEnabled", processorRef.getDelayEnabled());
                      result->setProperty("hostTransportSync", processorRef.getHostTransportSync());
                      result->setProperty("hostPlaying", processorRef.isHostPlaying());
                      complete(juce::var(result.get()));
                  })
                  .withNativeFunction("setHostTransportSync", [this](const juce::Array<juce::var>& args, auto complete)
                  {
                      if (args.size() > 0)
                          processorRef.setHostTransportSync(static_cast<bool>(args[0]));
                      complete({});
                  })
                  .withNativeFunction("setDelayEnabled", [this](const juce::Array<juce::var>& args, auto complete)
                  {
                      if (args.size() > 0)
                          processorRef.setDelayEnabled(static_cast<bool>(args[0]));
                      complete({});
                  })
                  .withNativeFunction("getTestSounds", [this](const juce::Array<juce::var>&, auto complete)
                  {
                      juce::DynamicObject::Ptr result = new juce::DynamicObject();
                      result->setProperty("usingSamples", processorRef.usingSamplesFromDisk());
                      result->setProperty("sampleFolder", processorRef.getSampleFolderPath());

                      juce::Array<juce::var> names;
                      const auto allNames = processorRef.getAllTestSoundNames();
                      for (const auto& name : allNames)
                          names.add(name);
                      result->setProperty("sounds", names);

                      complete(juce::var(result.get()));
                  })
                  .withNativeFunction("reloadSamples", [this](const juce::Array<juce::var>&, auto complete)
                  {
                      processorRef.reloadSamples();

                      // Return updated list
                      juce::DynamicObject::Ptr result = new juce::DynamicObject();
                      result->setProperty("usingSamples", processorRef.usingSamplesFromDisk());

                      juce::Array<juce::var> names;
                      const auto allNames = processorRef.getAllTestSoundNames();
                      for (const auto& name : allNames)
                          names.add(name);
                      result->setProperty("sounds", names);

                      complete(juce::var(result.get()));
                  })
                  .withNativeFunction("chooseSampleFolder", [this](const juce::Array<juce::var>&, auto complete)
                  {
                      // Create file chooser (must persist during async operation)
                      fileChooser = std::make_unique<juce::FileChooser>(
                          "Select Sample Folder",
                          juce::File(processorRef.getSampleFolderPath()),
                          "",
                          true);

                      fileChooser->launchAsync(
                          juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this, complete](const juce::FileChooser& fc)
                          {
                              auto results = fc.getResults();
                              if (results.size() > 0)
                              {
                                  juce::File folder = results[0];
                                  processorRef.setSampleFolder(folder.getFullPathName());

                                  // Return updated state
                                  juce::DynamicObject::Ptr result = new juce::DynamicObject();
                                  result->setProperty("usingSamples", processorRef.usingSamplesFromDisk());
                                  result->setProperty("sampleFolder", processorRef.getSampleFolderPath());

                                  juce::Array<juce::var> names;
                                  const auto allNames = processorRef.getAllTestSoundNames();
                                  for (const auto& name : allNames)
                                      names.add(name);
                                  result->setProperty("sounds", names);

                                  complete(juce::var(result.get()));
                              }
                              else
                              {
                                  // User cancelled - return current state
                                  juce::DynamicObject::Ptr result = new juce::DynamicObject();
                                  result->setProperty("cancelled", true);
                                  complete(juce::var(result.get()));
                              }
                          });
                  })
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
                     processorRef.getAPVTS().undoManager),
      ageAttachment(*processorRef.getAPVTS().getParameter("age"),
                    ageRelay,
                    processorRef.getAPVTS().undoManager),
      modRateAttachment(*processorRef.getAPVTS().getParameter("modRate"),
                        modRateRelay,
                        processorRef.getAPVTS().undoManager),
      modDepthAttachment(*processorRef.getAPVTS().getParameter("modDepth"),
                         modDepthRelay,
                         processorRef.getAPVTS().undoManager),
      warmthAttachment(*processorRef.getAPVTS().getParameter("warmth"),
                       warmthRelay,
                       processorRef.getAPVTS().undoManager),
      loopStartAttachment(*processorRef.getAPVTS().getParameter("loopStart"),
                          loopStartRelay,
                          processorRef.getAPVTS().undoManager),
      loopEndAttachment(*processorRef.getAPVTS().getParameter("loopEnd"),
                        loopEndRelay,
                        processorRef.getAPVTS().undoManager),
      loopSpeedAttachment(*processorRef.getAPVTS().getParameter("loopSpeed"),
                          loopSpeedRelay,
                          processorRef.getAPVTS().undoManager)
{
    addAndMakeVisible(webView);
    webView.goToURL(juce::WebBrowserComponent::getResourceProviderRoot());

    // Set default size to fit the full UI without scrolling
    // The UI has: header, tabs, looper controls, waveform, knobs, sample section, footer
    setSize(600, 700);
    setResizable(true, true);
    setResizeLimits(550, 600, 1000, 1000);

    // Start timer for BPM polling (100ms interval)
    startTimerHz(10);
}

LoopEngineEditor::~LoopEngineEditor()
{
    stopTimer();
}

void LoopEngineEditor::timerCallback()
{
    // Push BPM updates to JavaScript
    const float bpm = processorRef.getHostBpm();
    juce::String script = "if (window.updateBpmDisplay) window.updateBpmDisplay(" + juce::String(bpm, 1) + ");";
    webView.evaluateJavascript(script, nullptr);
}

void LoopEngineEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black);
}

void LoopEngineEditor::resized()
{
    webView.setBounds(getLocalBounds());
}

std::optional<juce::WebBrowserComponent::Resource> LoopEngineEditor::getResource(const juce::String& url)
{
    const auto urlToRetrieve = url == "/" ? juce::String("index.html") : url.fromFirstOccurrenceOf("/", false, false);

    // Determine MIME type
    juce::String mimeType = "application/octet-stream";
    if (urlToRetrieve.endsWith(".html"))
        mimeType = "text/html";
    else if (urlToRetrieve.endsWith(".css"))
        mimeType = "text/css";
    else if (urlToRetrieve.endsWith(".js"))
        mimeType = "text/javascript";

#if LOOP_ENGINE_DEV_MODE
    // DEV MODE: Load from file system for hot reload
    juce::File file(juce::String(DEV_UI_PATH) + urlToRetrieve);
    if (file.existsAsFile())
    {
        juce::MemoryBlock fileData;
        file.loadFileAsData(fileData);

        return juce::WebBrowserComponent::Resource{
            std::vector<std::byte>(reinterpret_cast<const std::byte*>(fileData.getData()),
                                   reinterpret_cast<const std::byte*>(fileData.getData()) + fileData.getSize()),
            mimeType.toStdString()
        };
    }
    return std::nullopt;
#else
    // PRODUCTION: Load from BinaryData
    static const std::map<juce::String, std::pair<const char*, int>> resourceMap = {
        {"index.html", {BinaryData::index_html, BinaryData::index_htmlSize}},
        {"styles.css", {BinaryData::styles_css, BinaryData::styles_cssSize}},
        {"main.js", {BinaryData::main_js, BinaryData::main_jsSize}}
    };

    auto it = resourceMap.find(urlToRetrieve);
    if (it == resourceMap.end())
        return std::nullopt;

    const auto& [data, size] = it->second;

    return juce::WebBrowserComponent::Resource{
        std::vector<std::byte>(reinterpret_cast<const std::byte*>(data),
                               reinterpret_cast<const std::byte*>(data) + size),
        mimeType.toStdString()
    };
#endif
}
