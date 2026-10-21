cmake_minimum_required(VERSION 3.22)
project(LecteurPVoc VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Binaire universel (Apple Silicon + Intel) et macOS 11 minimum
if(APPLE)
    if(NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
    endif()
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
endif()

# JUCE : dossier local (-DJUCE_DIR=...) ou telechargement automatique
if(DEFINED JUCE_DIR AND EXISTS "${JUCE_DIR}/CMakeLists.txt")
    add_subdirectory("${JUCE_DIR}" JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.15
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

if(APPLE)
    set(PVOC_FORMATS AU VST3 Standalone)
else()
    set(PVOC_FORMATS VST3 Standalone)
endif()

juce_add_plugin(LecteurPVoc
    COMPANY_NAME "Lecteur PVoc"
    BUNDLE_ID "be.pvoc.lecteur.plugin"
    PLUGIN_MANUFACTURER_CODE Lpvc
    PLUGIN_CODE Pvoc
    FORMATS ${PVOC_FORMATS}
    PRODUCT_NAME "Lecteur PVoc"
    DESCRIPTION "Lecteur et effet a vocodeur de phase : time-stretch et pitch-shift independants"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    AU_MAIN_TYPE kAudioUnitType_Effect
    VST3_CATEGORIES Fx Pitch Shift
    COPY_PLUGIN_AFTER_BUILD FALSE
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Le mode effet traite le son de l'entree audio."
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE)

target_sources(LecteurPVoc PRIVATE
    Source/PvocEngine.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_include_directories(LecteurPVoc PRIVATE Source)

target_compile_definitions(LecteurPVoc PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(LecteurPVoc
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_formats
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ------------------------------------------------------------------ Sampler PVoc (instrument)
juce_add_plugin(SamplerPVoc
    COMPANY_NAME "Lecteur PVoc"
    BUNDLE_ID "be.pvoc.sampler.plugin"
    PLUGIN_MANUFACTURER_CODE Lpvc
    PLUGIN_CODE Pvsm
    FORMATS ${PVOC_FORMATS}
    PRODUCT_NAME "Sampler PVoc"
    DESCRIPTION "Sampler polyphonique a vocodeur de phase"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    AU_MAIN_TYPE kAudioUnitType_MusicDevice
    VST3_CATEGORIES Instrument Sampler
    COPY_PLUGIN_AFTER_BUILD FALSE
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE)

target_sources(SamplerPVoc PRIVATE
    Source/PvocEngine.cpp
    Source/Sampler/SamplerProcessor.cpp
    Source/Sampler/SamplerEditor.cpp)

target_include_directories(SamplerPVoc PRIVATE Source)

target_compile_definitions(SamplerPVoc PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(SamplerPVoc
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_formats
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Tests (moteur seul, sans JUCE) : cmake -DPVOC_TESTS=ON
option(PVOC_TESTS "Compiler les tests" OFF)
if(PVOC_TESTS)
    target_compile_definitions(LecteurPVoc PUBLIC JUCE_MODAL_LOOPS_PERMITTED=1)
    target_compile_definitions(SamplerPVoc PUBLIC JUCE_MODAL_LOOPS_PERMITTED=1)
    add_executable(pvoc_effect_test tests/effect_test.cpp Source/PvocEngine.cpp)
    add_executable(pvoc_scrub_test tests/scrub_test.cpp Source/PvocEngine.cpp)
    add_executable(pvoc_plugin_test tests/plugin_test.cpp)
    target_link_libraries(pvoc_plugin_test PRIVATE LecteurPVoc juce::juce_recommended_config_flags)
    # memes chemins et definitions que le code du plugin
    target_include_directories(pvoc_plugin_test PRIVATE $<TARGET_PROPERTY:LecteurPVoc,INCLUDE_DIRECTORIES>)
    target_compile_definitions(pvoc_plugin_test PRIVATE $<TARGET_PROPERTY:LecteurPVoc,COMPILE_DEFINITIONS>)
endif()

if(PVOC_TESTS)
    add_executable(pvoc_sampler_test tests/sampler_test.cpp)
    target_link_libraries(pvoc_sampler_test PRIVATE SamplerPVoc juce::juce_recommended_config_flags)
    target_include_directories(pvoc_sampler_test PRIVATE $<TARGET_PROPERTY:SamplerPVoc,INCLUDE_DIRECTORIES>)
    target_compile_definitions(pvoc_sampler_test PRIVATE $<TARGET_PROPERTY:SamplerPVoc,COMPILE_DEFINITIONS>)
endif()
