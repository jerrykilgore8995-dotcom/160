cmake_minimum_required(VERSION 3.22)
project(160MKII VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "The MKII VST3 circuit-process transport currently supports Windows.")
endif()

include(FetchContent)
FetchContent_Declare(JUCE GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
                         GIT_TAG 8.0.10 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(Circuit160
    COMPANY_NAME Kilgore
    PLUGIN_MANUFACTURER_CODE Jkil
    PLUGIN_CODE K160
    PRODUCT_NAME "160 MKII"
    VERSION 0.1.0
    FORMATS VST3
    IS_SYNTH FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx Dynamics)
target_sources(Circuit160 PRIVATE Plugin.cpp)
target_compile_features(Circuit160 PRIVATE cxx_std_20)
target_compile_definitions(Circuit160 PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
                                             JUCE_VST3_CAN_REPLACE_VST2=0 NOMINMAX)
target_link_libraries(Circuit160 PRIVATE juce::juce_audio_processors juce::juce_gui_basics
                                         juce::juce_recommended_config_flags)

juce_add_console_app(Vst3Smoke)
target_sources(Vst3Smoke PRIVATE Vst3Smoke.cpp)
target_compile_features(Vst3Smoke PRIVATE cxx_std_20)
target_compile_definitions(Vst3Smoke PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(Vst3Smoke PRIVATE juce::juce_audio_processors juce::juce_gui_basics
                                        juce::juce_recommended_config_flags)

if(MKII_NATIVE_RUNTIME)
    target_sources(Vst3Smoke PRIVATE Plugin.cpp)
    foreach(target Circuit160 Vst3Smoke)
        target_compile_definitions(${target} PRIVATE MKII_NATIVE=1 NOMINMAX)
    endforeach()
endif()
