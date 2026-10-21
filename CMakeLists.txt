cmake_minimum_required(VERSION 3.25)

# Both must be set before project() to apply to every target. macOS 11 is the
# first release with Apple Silicon, so it is the floor for a universal binary.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures to build")

project(Livecut VERSION 0.6.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LIVECUT_BUILD_TESTS "Build the unit tests" ${PROJECT_IS_TOP_LEVEL})
option(LIVECUT_COPY_PLUGIN_AFTER_BUILD "Install the plugins into the user's plug-in folders after building" OFF)

add_subdirectory(libs/JUCE)

# The cutting engine is plain C++ with no JUCE dependency, so it can be tested in isolation.
add_library(LivecutEngine STATIC
    src/engine/Accent.h
    src/engine/WetHold.h
    src/engine/AudioHistory.h
    src/engine/SwingGrid.h
    src/engine/DelayTimeline.h
    src/engine/OnsetDetector.h
    src/engine/TransformSpan.h
    src/engine/Bounce.cpp
    src/engine/CutProc11.cpp
    src/engine/Cutter.cpp
    src/engine/Effects.cpp
    src/engine/ImpactEffects.cpp
    src/engine/LivePlayer.cpp
    src/engine/PhraseSeed.cpp
    src/engine/Random.cpp
    src/engine/SQPusher.cpp
    src/engine/WarpCut.cpp)
target_include_directories(LivecutEngine PUBLIC src)
target_compile_features(LivecutEngine PUBLIC cxx_std_20)
target_link_libraries(LivecutEngine
    PRIVATE
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

juce_add_plugin(Livecut
    PRODUCT_NAME "Livecut"
    DESCRIPTION "BBCut-style live beat slicer"
    COMPANY_NAME "Sean Dearnaley"
    COMPANY_WEBSITE "https://github.com/seandearnaley/livecut"
    BUNDLE_ID "com.seandearnaley.livecut"
    PLUGIN_MANUFACTURER_CODE Sdrn
    PLUGIN_CODE Lvct
    FORMATS VST3 AU Standalone
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    VST3_CATEGORIES Fx
    AU_MAIN_TYPE kAudioUnitType_Effect
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Livecut cuts the audio coming into your Mac."
    COPY_PLUGIN_AFTER_BUILD ${LIVECUT_COPY_PLUGIN_AFTER_BUILD})

target_compile_definitions(Livecut
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)

# Plugin sources live in an INTERFACE library so the test runner can compile
# the same code without linking against a plugin wrapper.
add_library(LivecutCode INTERFACE)
target_sources(LivecutCode
    INTERFACE
        src/CaptureService.cpp
        src/CaptureService.h
        src/StreamResources.h
        src/OutputMixer.h
        src/PhraseCapture.cpp
        src/PhraseCapture.h
        src/DisplayState.h
        src/HostSync.cpp
        src/HostSync.h
        src/Parameters.cpp
        src/Parameters.h
        src/PluginEditor.cpp
        src/PluginEditor.h
        src/PluginProcessor.cpp
        src/PluginProcessor.h
        src/ui/Controls.cpp
        src/ui/Controls.h
        src/ui/LivecutLookAndFeel.cpp
        src/ui/LivecutLookAndFeel.h
        src/ui/PhraseRuler.cpp
        src/ui/PhraseRuler.h
        src/ui/RangeSlider.cpp
        src/ui/RangeSlider.h)
target_include_directories(LivecutCode INTERFACE src)
target_link_libraries(LivecutCode INTERFACE LivecutEngine juce::juce_audio_utils)

target_link_libraries(Livecut
    PRIVATE
        LivecutCode
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

if(LIVECUT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(libs/Catch2 EXCLUDE_FROM_ALL)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/libs/Catch2/extras")
    include(Catch)

    # Tests only run on the build machine, so skip the other slice of a universal build.
    function(livecut_build_for_host_only target)
        if(APPLE)
            set_target_properties(${target} PROPERTIES OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")
        endif()
    endfunction()

    add_executable(LivecutTests
        tests/engine/AllocationCounter.cpp
        tests/CaptureTests.cpp
        tests/EditorTests.cpp
        tests/HostSyncTests.cpp
        tests/ParametersTests.cpp
        tests/PluginProcessorTests.cpp
        tests/ImpactStateTests.cpp
        tests/ImpactProcessorTests.cpp
        tests/RecutProcessorTests.cpp)
    livecut_build_for_host_only(LivecutTests)
    # The JucePlugin_* definitions only exist on the plugin target, so copy them.
    target_compile_definitions(LivecutTests PRIVATE $<TARGET_PROPERTY:Livecut,COMPILE_DEFINITIONS>)
    target_link_libraries(LivecutTests
        PRIVATE
            LivecutCode
            Catch2::Catch2WithMain
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)

    catch_discover_tests(LivecutTests DISCOVERY_MODE PRE_TEST)

    add_executable(LivecutEngineTests
        tests/engine/AttackCandidatesTests.cpp
        tests/engine/TransformPlaybackTests.cpp
        tests/engine/RecutSelectionTests.cpp
        tests/legacy/LegacyCutter.cpp
        tests/legacy/LegacyPlayer.cpp
        tests/engine/LegacyEquivalenceTests.cpp
        tests/engine/PerformTests.cpp
        tests/engine/StudioTests.cpp
        tests/engine/TransformSpanTests.cpp
        tests/engine/AllocationCounter.cpp
        tests/engine/AudioHistoryTests.cpp
        tests/engine/SwingGridTests.cpp
        tests/engine/DelayTimelineTests.cpp
        tests/engine/OnsetDetectorTests.cpp
        tests/engine/BounceTests.cpp
        tests/engine/CutProc11Tests.cpp
        tests/engine/CutterTests.cpp
        tests/engine/EffectsTests.cpp
        tests/engine/ImpactEffectsTests.cpp
        tests/engine/ImpactIntegrationTests.cpp
        tests/engine/LivePlayerTests.cpp
        tests/engine/PhraseSeedTests.cpp
        tests/engine/RandomTests.cpp
        tests/engine/SQPusherTests.cpp
        tests/engine/WarpCutTests.cpp)
    target_include_directories(LivecutEngineTests PRIVATE tests)
    livecut_build_for_host_only(LivecutEngineTests)
    target_link_libraries(LivecutEngineTests
        PRIVATE
            LivecutEngine
            Catch2::Catch2WithMain
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)

    catch_discover_tests(LivecutEngineTests DISCOVERY_MODE PRE_TEST)
endif()
