juce_add_plugin(AudioDynamicsAnalyzer
    COMPANY_NAME "klangme1ster"
    PRODUCT_NAME "Audio Dynamics Analyzer"
    PLUGIN_MANUFACTURER_CODE Klm1
    PLUGIN_CODE AdaX
    FORMATS VST3 Standalone
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES Analyzer
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(AudioDynamicsAnalyzer PRIVATE
    DynamicsViews.cpp
    HistoryGraph.cpp
    PluginEditor.cpp
    PluginProcessor.cpp)

target_compile_definitions(AudioDynamicsAnalyzer PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_USE_WINDOWS_MEDIA_FORMAT=1)

target_link_libraries(AudioDynamicsAnalyzer
    PRIVATE
        ada_engine
        juce::juce_audio_utils
        juce::juce_audio_processors
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Developer tool: renders the dynamics views for a synthetic song into a PNG (no host needed)
juce_add_console_app(ada_view_snapshot PRODUCT_NAME "ada_view_snapshot")
target_sources(ada_view_snapshot PRIVATE
    ${PROJECT_SOURCE_DIR}/tests/cpp/view_snapshot.cpp
    DynamicsViews.cpp
    HistoryGraph.cpp)
target_include_directories(ada_view_snapshot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(ada_view_snapshot PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(ada_view_snapshot PRIVATE
    ada_engine
    juce::juce_gui_basics
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
