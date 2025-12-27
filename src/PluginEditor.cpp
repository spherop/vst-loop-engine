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
                  .withOptionsFrom(loopPitchRelay)
                  .withOptionsFrom(loopFadeRelay)
                  .withOptionsFrom(degradeHPRelay)
                  .withOptionsFrom(degradeHPQRelay)
                  .withOptionsFrom(degradeLPRelay)
                  .withOptionsFrom(degradeLPQRelay)
                  .withOptionsFrom(degradeBitRelay)
                  .withOptionsFrom(degradeSRRelay)
                  .withOptionsFrom(degradeWobbleRelay)
                  .withOptionsFrom(degradeVinylRelay)
                  .withOptionsFrom(degradeMixRelay)
                  .withOptionsFrom(textureDensityRelay)
                  .withOptionsFrom(textureScatterRelay)
                  .withOptionsFrom(textureMotionRelay)
                  .withOptionsFrom(textureMixRelay)
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
                  .withNativeFunction("loopRedo", [this](const juce::Array<juce::var>&, auto complete)
                  {
                      processorRef.getLoopEngine().redo();
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
                  .withNativeFunction("setLayerMuted", [this](const juce::Array<juce::var>& args, auto complete)
                  {
                      if (args.size() >= 2)
                      {
                          int layer = static_cast<int>(args[0]);
                          bool muted = static_cast<bool>(args[1]);
                          processorRef.getLoopEngine().setLayerMuted(layer, muted);
                      }
                      complete({});
                  })
                  .withNativeFunction("getLayerMuted", [this](const juce::Array<juce::var>& args, auto complete)
                  {
                      juce::DynamicObject::Ptr result = new juce::DynamicObject();
                      if (args.size() > 0)
                      {
                          int layer = static_cast<int>(args[0]);
                          result->setProperty("muted", processorRef.getLoopEngine().getLayerMuted(layer));
                      }
                      complete(juce::var(result.get()));
                  })
                  .withNativeFunction("setLayerVolume", [this](const juce::Array<juce::var>& args, auto complete)
                  {
                      if (args.size() >= 2)
                      {
                          int layer = static_cast<int>(args[0]);
                          float vol = static_cast<float>(args[1]);
                          processorRef.getLoopEngine().setLayerVolume(layer, vol);
                      }
                      complete({});
                  })
                  .withNativeFunction("getLayerVolume", [this](const juce::Array<juce::var>& args, auto complete)
                  {
                      if (args.size() > 0)
                      {
                          int layer = static_cast<int>(args[0]);
                          complete(processorRef.getLoopEngine().getLayerVolume(layer));
                      }
                      else
                      {
                          complete(1.0f);
                      }
                  })
                  .withNativeFunction("setLayerPan", [this](const juce::Array<juce::var>& args, auto complete)
                  {
                      if (args.size() >= 2)
                      {
                          int layer = static_cast<int>(args[0]);
                          float p = static_cast<float>(args[1]);
                          processorRef.getLoopEngine().setLayerPan(layer, p);
                      }
                      complete({});
                  })
                  .withNativeFunction("getLayerPan", [this](const juce::Array<juce::var>& args, auto complete)
                  {
                      if (args.size() > 0)
                      {
                          int layer = static_cast<int>(args[0]);
                          complete(processorRef.getLoopEngine().getLayerPan(layer));
                      }
                      else
                      {
                          complete(0.0f);
                      }
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
                  .withNativeFunction("setLoopLengthBeats", [this](const juce::Array<juce::var>& args, auto complete)
                  {
                      if (args.size() > 0)
                      {
                          int beats = static_cast<int>(args[0]);
                          processorRef.getLoopEngine().setLoopLengthBeats(beats);
                          DBG("setLoopLengthBeats: " + juce::String(beats));
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
                      if (auto* param = processorRef.getAPVTS().getParameter("loopPitch"))
                          param->setValueNotifyingHost(param->convertTo0to1(0.0f));  // 0 semitones
                      if (auto* param = processorRef.getAPVTS().getParameter("loopFade"))
                          param->setValueNotifyingHost(param->convertTo0to1(100.0f));  // 100% (no fade)

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

                      // Get combined waveform data (100 points for visualization)
                      auto waveformData = loopEngine.getWaveformData(100);
                      juce::Array<juce::var> waveformArray;
                      for (float val : waveformData)
                          waveformArray.add(val);
                      result->setProperty("waveform", waveformArray);

                      // Get per-layer waveform data for colored visualization
                      auto layerWaveforms = loopEngine.getLayerWaveforms(100);
                      juce::Array<juce::var> layerWaveformArrays;
                      for (const auto& layerWf : layerWaveforms)
                      {
                          juce::Array<juce::var> layerArray;
                          for (float val : layerWf)
                              layerArray.add(val);
                          layerWaveformArrays.add(layerArray);
                      }
                      result->setProperty("layerWaveforms", layerWaveformArrays);

                      // Get mute states for each layer
                      auto muteStates = loopEngine.getLayerMuteStates();
                      juce::Array<juce::var> muteArray;
                      for (bool muted : muteStates)
                          muteArray.add(muted);
                      result->setProperty("layerMutes", muteArray);

                      // Input monitoring data
                      result->setProperty("inputLevelL", loopEngine.getInputLevelL());
                      result->setProperty("inputLevelR", loopEngine.getInputLevelR());
                      result->setProperty("inputMuted", loopEngine.getInputMuted());

                      complete(juce::var(result.get()));
                  })
                  .withNativeFunction("setInputMuted", [this](const juce::Array<juce::var>& args, auto complete)
                  {
                      if (args.size() > 0)
                          processorRef.getLoopEngine().setInputMuted(static_cast<bool>(args[0]));
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
                  .withNativeFunction("setDegradeEnabled", [this](const juce::Array<juce::var>& args, auto complete)
                  {
                      if (args.size() > 0)
                          processorRef.setDegradeEnabled(static_cast<bool>(args[0]));
                      complete({});
                  })
                  .withNativeFunction("setDegradeFilterEnabled", [this](const juce::Array<juce::var>& args, auto complete)
                  {
                      if (args.size() > 0)
                          processorRef.setDegradeFilterEnabled(static_cast<bool>(args[0]));
                      complete({});
                  })
                  .withNativeFunction("setDegradeLofiEnabled", [this](const juce::Array<juce::var>& args, auto complete)
                  {
                      if (args.size() > 0)
                          processorRef.setDegradeLofiEnabled(static_cast<bool>(args[0]));
                      complete({});
                  })
                  .withNativeFunction("setTextureEnabled", [this](const juce::Array<juce::var>& args, auto complete)
                  {
                      if (args.size() > 0)
                          processorRef.setTextureEnabled(static_cast<bool>(args[0]));
                      complete({});
                  })
                  .withNativeFunction("setDegradeHPEnabled", [this](const juce::Array<juce::var>& args, auto complete)
                  {
                      if (args.size() > 0)
                          processorRef.setDegradeHPEnabled(static_cast<bool>(args[0]));
                      complete({});
                  })
                  .withNativeFunction("setDegradeLPEnabled", [this](const juce::Array<juce::var>& args, auto complete)
                  {
                      if (args.size() > 0)
                          processorRef.setDegradeLPEnabled(static_cast<bool>(args[0]));
                      complete({});
                  })
                  .withNativeFunction("getDegradeState", [this](const juce::Array<juce::var>&, auto complete)
                  {
                      juce::DynamicObject::Ptr result = new juce::DynamicObject();
                      result->setProperty("enabled", processorRef.getDegradeEnabled());
                      result->setProperty("filterEnabled", processorRef.getDegradeFilterEnabled());
                      result->setProperty("lofiEnabled", processorRef.getDegradeLofiEnabled());
                      result->setProperty("textureEnabled", processorRef.getTextureEnabled());
                      result->setProperty("hpEnabled", processorRef.getDegradeHPEnabled());
                      result->setProperty("lpEnabled", processorRef.getDegradeLPEnabled());
                      // Filter visualization data
                      auto& degrade = processorRef.getDegradeProcessor();
                      result->setProperty("hpFreq", degrade.getCurrentHPFreq());
                      result->setProperty("lpFreq", degrade.getCurrentLPFreq());
                      result->setProperty("hpQ", degrade.getCurrentHPQ());
                      result->setProperty("lpQ", degrade.getCurrentLPQ());
                      complete(juce::var(result.get()));
                  })
                  .withNativeFunction("clearLayer", [this](const juce::Array<juce::var>& args, auto complete)
                  {
                      if (args.size() > 0)
                      {
                          int layer = static_cast<int>(args[0]);
                          processorRef.getLoopEngine().clearLayer(layer);
                      }
                      complete({});
                  })
                  .withNativeFunction("deleteLayer", [this](const juce::Array<juce::var>& args, auto complete)
                  {
                      if (args.size() > 0)
                      {
                          int layer = static_cast<int>(args[0]);
                          processorRef.getLoopEngine().deleteLayer(layer);
                      }
                      complete({});
                  })
                  .withNativeFunction("flattenLayers", [this](const juce::Array<juce::var>&, auto complete)
                  {
                      processorRef.getLoopEngine().flattenLayers();
                      complete({});
                  })
                  .withNativeFunction("getLayerContentStates", [this](const juce::Array<juce::var>&, auto complete)
                  {
                      const auto& loopEngine = processorRef.getLoopEngine();
                      juce::Array<juce::var> contentArray;
                      for (int i = 1; i <= 8; ++i)  // 1-indexed
                      {
                          contentArray.add(loopEngine.layerHasContent(i));
                      }
                      complete(contentArray);
                  })
                  .withNativeFunction("getAudioDiagnostics", [this](const juce::Array<juce::var>&, auto complete)
                  {
                      // Return diagnostic data for debugging audio issues
                      auto& loopEngine = processorRef.getLoopEngine();
                      juce::DynamicObject::Ptr result = new juce::DynamicObject();

                      // Peak levels (before soft clipping)
                      result->setProperty("preClipPeakL", loopEngine.getPreClipPeakL());
                      result->setProperty("preClipPeakR", loopEngine.getPreClipPeakR());
                      result->setProperty("loopOutputPeakL", loopEngine.getLoopOutputPeakL());
                      result->setProperty("loopOutputPeakR", loopEngine.getLoopOutputPeakR());

                      // Clip event count
                      result->setProperty("clipEventCount", loopEngine.getClipEventCount());

                      // Per-layer clip counts
                      juce::Array<juce::var> layerClips;
                      for (int i = 0; i < 8; ++i)
                      {
                          layerClips.add(loopEngine.getLayerClipCount(i));
                      }
                      result->setProperty("layerClipCounts", layerClips);

                      complete(juce::var(result.get()));
                  })
                  .withNativeFunction("resetAudioDiagnostics", [this](const juce::Array<juce::var>&, auto complete)
                  {
                      auto& loopEngine = processorRef.getLoopEngine();
                      loopEngine.resetClipEventCount();
                      loopEngine.resetLayerClipCounts();
                      complete({});
                  })
                  .withNativeFunction("setCrossfadeParams", [this](const juce::Array<juce::var>& args, auto complete)
                  {
                      // Args: preTimeMs, postTimeMs, volDepth (0-1), filterFreq (Hz, 0=off), filterDepth (0-1)
                      if (args.size() >= 5)
                      {
                          int preTimeMs = static_cast<int>(args[0]);
                          int postTimeMs = static_cast<int>(args[1]);
                          float volDepth = static_cast<float>(args[2]);
                          float filterFreq = static_cast<float>(args[3]);
                          float filterDepth = static_cast<float>(args[4]);

                          auto& loopEngine = processorRef.getLoopEngine();
                          loopEngine.setCrossfadeParams(preTimeMs, postTimeMs, volDepth, filterFreq, filterDepth);

                          DBG("setCrossfadeParams: pre=" + juce::String(preTimeMs) +
                              "ms post=" + juce::String(postTimeMs) +
                              "ms volDepth=" + juce::String(volDepth, 2) +
                              " filterFreq=" + juce::String(filterFreq, 0) +
                              "Hz filterDepth=" + juce::String(filterDepth, 2));
                      }
                      complete({});
                  })
                  .withNativeFunction("saveCrossfadeSettings", [this](const juce::Array<juce::var>& args, auto complete)
                  {
                      if (args.size() >= 1 && args[0].isObject())
                      {
                          auto* obj = args[0].getDynamicObject();
                          if (obj != nullptr)
                          {
                              // Save to user preferences file
                              juce::File settingsDir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                  .getChildFile("LoopEngine");
                              settingsDir.createDirectory();
                              juce::File settingsFile = settingsDir.getChildFile("crossfade_settings.json");

                              juce::var settings = args[0];
                              juce::String json = juce::JSON::toString(settings);
                              settingsFile.replaceWithText(json);

                              DBG("Saved crossfade settings to: " + settingsFile.getFullPathName());
                          }
                      }
                      complete({});
                  })
                  .withNativeFunction("loadCrossfadeSettings", [this](const juce::Array<juce::var>&, auto complete)
                  {
                      juce::File settingsDir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                          .getChildFile("LoopEngine");
                      juce::File settingsFile = settingsDir.getChildFile("crossfade_settings.json");

                      if (settingsFile.existsAsFile())
                      {
                          juce::String json = settingsFile.loadFileAsString();
                          auto settings = juce::JSON::parse(json);
                          if (settings.isObject())
                          {
                              DBG("Loaded crossfade settings from: " + settingsFile.getFullPathName());
                              complete(settings);
                              return;
                          }
                      }

                      // Return default settings
                      auto* defaults = new juce::DynamicObject();
                      defaults->setProperty("preTime", 80);
                      defaults->setProperty("postTime", 100);
                      defaults->setProperty("volDepth", 0);
                      defaults->setProperty("filterFreq", 0);
                      defaults->setProperty("filterDepth", 0);
                      defaults->setProperty("enabled", true);
                      complete(juce::var(defaults));
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
                          processorRef.getAPVTS().undoManager),
      loopPitchAttachment(*processorRef.getAPVTS().getParameter("loopPitch"),
                          loopPitchRelay,
                          processorRef.getAPVTS().undoManager),
      loopFadeAttachment(*processorRef.getAPVTS().getParameter("loopFade"),
                         loopFadeRelay,
                         processorRef.getAPVTS().undoManager),
      degradeHPAttachment(*processorRef.getAPVTS().getParameter("degradeHP"),
                          degradeHPRelay,
                          processorRef.getAPVTS().undoManager),
      degradeHPQAttachment(*processorRef.getAPVTS().getParameter("degradeHPQ"),
                           degradeHPQRelay,
                           processorRef.getAPVTS().undoManager),
      degradeLPAttachment(*processorRef.getAPVTS().getParameter("degradeLP"),
                          degradeLPRelay,
                          processorRef.getAPVTS().undoManager),
      degradeLPQAttachment(*processorRef.getAPVTS().getParameter("degradeLPQ"),
                           degradeLPQRelay,
                           processorRef.getAPVTS().undoManager),
      degradeBitAttachment(*processorRef.getAPVTS().getParameter("degradeBit"),
                           degradeBitRelay,
                           processorRef.getAPVTS().undoManager),
      degradeSRAttachment(*processorRef.getAPVTS().getParameter("degradeSR"),
                          degradeSRRelay,
                          processorRef.getAPVTS().undoManager),
      degradeWobbleAttachment(*processorRef.getAPVTS().getParameter("degradeWobble"),
                              degradeWobbleRelay,
                              processorRef.getAPVTS().undoManager),
      degradeVinylAttachment(*processorRef.getAPVTS().getParameter("degradeVinyl"),
                             degradeVinylRelay,
                             processorRef.getAPVTS().undoManager),
      degradeMixAttachment(*processorRef.getAPVTS().getParameter("degradeMix"),
                           degradeMixRelay,
                           processorRef.getAPVTS().undoManager),
      textureDensityAttachment(*processorRef.getAPVTS().getParameter("textureDensity"),
                               textureDensityRelay,
                               processorRef.getAPVTS().undoManager),
      textureScatterAttachment(*processorRef.getAPVTS().getParameter("textureScatter"),
                               textureScatterRelay,
                               processorRef.getAPVTS().undoManager),
      textureMotionAttachment(*processorRef.getAPVTS().getParameter("textureMotion"),
                              textureMotionRelay,
                              processorRef.getAPVTS().undoManager),
      textureMixAttachment(*processorRef.getAPVTS().getParameter("textureMix"),
                           textureMixRelay,
                           processorRef.getAPVTS().undoManager)
{
    addAndMakeVisible(webView);
    webView.goToURL(juce::WebBrowserComponent::getResourceProviderRoot());

    // Set default size to fit the full UI without scrolling
    // Two-column layout: looper left, degrade right (larger degrade panel)
    // Increased 15% from 990x667 for more comfortable layout
    setSize(1140, 767);
    setResizable(true, true);
    setResizeLimits(990, 667, 1600, 1150);

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
