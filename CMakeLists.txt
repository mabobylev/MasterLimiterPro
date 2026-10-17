cmake_minimum_required(VERSION 3.22)
project(MasterLimiterPro VERSION 1.1.0)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/CMakeLists.txt")
    message(FATAL_ERROR "JUCE was not found. See README.md for pinned JUCE dependency setup.")
endif()

option(MASTERLIMITER_COPY_PLUGIN_AFTER_BUILD "Install VST3 after building" ON)

add_subdirectory(JUCE)

juce_add_plugin(MasterLimiterPro
    COMPANY_NAME "MikeAudio"
    COMPANY_WEBSITE "https://example.com"
    COMPANY_EMAIL "dev@example.com"
    PLUGIN_MANUFACTURER_CODE Mlim
    PLUGIN_CODE Mlpr
    FORMATS VST3
    PRODUCT_NAME "Master Limiter Pro"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD ${MASTERLIMITER_COPY_PLUGIN_AFTER_BUILD}
    VST3_CATEGORIES "Dynamics"
)

target_sources(MasterLimiterPro PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h
    Source/PluginEditor.cpp
    Source/PluginEditor.h
)

target_compile_definitions(MasterLimiterPro PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(MasterLimiterPro PRIVATE
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_dsp
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

include(CTest)
if(BUILD_TESTING)
    add_executable(MasterLimiterTests Tests/ProcessorTests.cpp)
    target_include_directories(MasterLimiterTests PRIVATE Source
        "$<TARGET_PROPERTY:MasterLimiterPro,INCLUDE_DIRECTORIES>")
    target_compile_definitions(MasterLimiterTests PRIVATE
        "$<TARGET_PROPERTY:MasterLimiterPro,COMPILE_DEFINITIONS>")
    target_link_libraries(MasterLimiterTests PRIVATE MasterLimiterPro)
    add_test(NAME MasterLimiterProcessor COMMAND MasterLimiterTests)
    set_tests_properties(MasterLimiterProcessor PROPERTIES TIMEOUT 90)
endif()
