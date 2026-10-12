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
    # app icon (python/tools/make_icon.py): the small one is the bar-only variant for 16/32 px
    ICON_BIG ${PROJECT_SOURCE_DIR}/resources/icon/ada-icon-1024.png
    ICON_SMALL ${PROJECT_SOURCE_DIR}/resources/icon/ada-icon-32.png
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(AudioDynamicsAnalyzer PRIVATE
    DynamicsViews.cpp
    FileAnalysis.cpp
    HistoryGraph.cpp
    MediaFoundationFormat.cpp
    OutputMonitor.cpp
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

# Media Foundation (M4A / AAC reader, part of Windows). Delay-loaded: Windows "N" editions without
# the Media Feature Pack (and some server images) have no mfplat.dll; the app and the plugin must
# still start there, only without M4A (ada::mediaFoundationAvailable). PUBLIC, so the options reach
# the VST3 / standalone targets that link JUCE's shared-code library.
function(ada_link_media_foundation target)
    if(MSVC)
        target_link_libraries(${target} PUBLIC mfplat mfreadwrite mfuuid delayimp)
        target_link_options(${target} PUBLIC /DELAYLOAD:mfplat.dll /DELAYLOAD:mfreadwrite.dll)
    endif()
endfunction()
ada_link_media_foundation(AudioDynamicsAnalyzer)

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

# File analysis tests (JUCE readers + engine), registered with CTest
if(ADA_BUILD_TESTS)
    FetchContent_GetProperties(doctest)
    juce_add_console_app(ada_file_analysis_tests PRODUCT_NAME "ada_file_analysis_tests")
    target_sources(ada_file_analysis_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/tests/cpp/test_file_analysis.cpp
        FileAnalysis.cpp
        MediaFoundationFormat.cpp)
    target_include_directories(ada_file_analysis_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/tests/cpp)
    target_include_directories(ada_file_analysis_tests SYSTEM PRIVATE ${doctest_SOURCE_DIR})
    target_compile_definitions(ada_file_analysis_tests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        JUCE_USE_MP3AUDIOFORMAT=1 JUCE_USE_WINDOWS_MEDIA_FORMAT=1)
    target_link_libraries(ada_file_analysis_tests PRIVATE
        ada_engine
        juce::juce_audio_formats
        juce::juce_events
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
    ada_link_media_foundation(ada_file_analysis_tests)
    add_test(NAME ada_file_analysis_tests COMMAND ada_file_analysis_tests)

    # File runner for the Python parity test of decoded formats (python/tests/test_parity.py)
    juce_add_console_app(ada_file_cli PRODUCT_NAME "ada_file_cli")
    target_sources(ada_file_cli PRIVATE
        ${PROJECT_SOURCE_DIR}/tests/cpp/ada_file_cli.cpp
        FileAnalysis.cpp
        MediaFoundationFormat.cpp)
    target_include_directories(ada_file_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/tests/cpp)
    target_compile_definitions(ada_file_cli PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        JUCE_USE_MP3AUDIOFORMAT=1 JUCE_USE_WINDOWS_MEDIA_FORMAT=1)
    target_link_libraries(ada_file_cli PRIVATE
        ada_engine
        juce::juce_audio_formats
        juce::juce_events
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
    ada_link_media_foundation(ada_file_cli)
endif()
