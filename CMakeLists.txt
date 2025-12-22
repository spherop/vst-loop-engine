cmake_minimum_required(VERSION 3.22)

# Project setup
project(FuzzDelay VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include CPM for dependency management
include(cmake/CPM.cmake)

# Fetch JUCE 8
CPMAddPackage(
    NAME JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    OPTIONS
        "JUCE_ENABLE_MODULE_SOURCE_GROUPS ON"
)

# Plugin configuration
juce_add_plugin(FuzzDelay
    COMPANY_NAME "FuzzDelayAudio"
    PLUGIN_MANUFACTURER_CODE Fzdy
    PLUGIN_CODE Fzdl
    FORMATS VST3 AU Standalone
    PRODUCT_NAME "Fuzz Delay"
    BUNDLE_ID "com.fuzzdelayaudio.fuzzdelay"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    VST3_CATEGORIES "Fx" "Delay" "Distortion"
    AU_MAIN_TYPE "kAudioUnitType_Effect"
)

# Add UI resources as binary data
file(GLOB_RECURSE UI_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/*.html"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/*.css"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/*.js"
)

juce_add_binary_data(FuzzDelayData
    HEADER_NAME "BinaryData.h"
    NAMESPACE BinaryData
    SOURCES ${UI_FILES}
)

# Source files
target_sources(FuzzDelay
    PRIVATE
        src/PluginProcessor.cpp
        src/PluginEditor.cpp
)

# Include directories
target_include_directories(FuzzDelay
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Compile definitions
target_compile_definitions(FuzzDelay
    PUBLIC
        JUCE_WEB_BROWSER=1
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
)

# Link libraries
target_link_libraries(FuzzDelay
    PRIVATE
        FuzzDelayData
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)
