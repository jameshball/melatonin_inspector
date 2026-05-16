cmake_minimum_required(VERSION 3.20)

project(MelatoninInspector VERSION 1.3.0 LANGUAGES C CXX
    DESCRIPTION "JUCE module for inspecting Components"
    HOMEPAGE_URL "https://github.com/sudara/melatonin_inspector")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED YES)

include(FetchContent)
if (MelatoninInspector_IS_TOP_LEVEL)
    option(JUCE7 "Run tests on JUCE 7" OFF)
    option(MELATONIN_INSPECTOR_ENABLE_AUTOMATION "Enable the optional melatonin_inspector automation endpoint in the test app" OFF)
    option(MELATONIN_INSPECTOR_BUILD_CLI "Build the melatonin-ui automation CLI" ON)
    option(MELATONIN_INSPECTOR_BUILD_DEMORUNNER_AUTOMATION "Build a patched JUCE DemoRunner with melatonin_inspector automation enabled" OFF)

    message(STATUS "Cloning JUCE...")
    if (JUCE7)
        FetchContent_Declare(JUCE
            GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
            GIT_TAG 7.0.12
            GIT_PROGRESS TRUE
        )
    else ()

        FetchContent_Declare(JUCE
            GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
            GIT_TAG develop
            GIT_PROGRESS TRUE
        )
    endif ()

    FetchContent_MakeAvailable(JUCE)

    SET(TARGET_NAME "unique_ptr_disabled" CACHE STRING "Name of cpp and target to compile")

    juce_add_gui_app("${TARGET_NAME}" VERSION 1.0.0)
    target_sources("${TARGET_NAME}" PRIVATE "tests/${TARGET_NAME}.cpp")

    target_compile_definitions("${TARGET_NAME}" PUBLIC
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
    )

    # Windows errors due to sccache issues
    # see https://github.com/mozilla/sccache/pull/963/files
    if (MSVC)
        if (CMAKE_BUILD_TYPE STREQUAL "Debug")
            string(REPLACE "/Zi" "/Z7" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
            string(REPLACE "/MP" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
        elseif (CMAKE_BUILD_TYPE STREQUAL "Release")
            string(REPLACE "/Zi" "/Z7" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
            string(REPLACE "/MP" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
        endif ()
    endif ()

    set_target_properties("${TARGET_NAME}" PROPERTIES COMPILE_WARNING_AS_ERROR ON)

    if (MELATONIN_INSPECTOR_BUILD_CLI)
        juce_add_console_app(melatonin-ui VERSION 1.0.0)
        target_sources(melatonin-ui PRIVATE tools/melatonin_ui.cpp)
        target_compile_definitions(melatonin-ui PRIVATE JUCE_USE_CURL=0)
        target_link_libraries(melatonin-ui PRIVATE
            juce::juce_core
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)
    endif ()

endif ()

if (NOT COMMAND juce_add_module)
    message(FATAL_ERROR "JUCE must be added to your project before melatonin_inspector!")
endif ()

# this makes the assumption the current directory is named melatonin_inspector
juce_add_module("${CMAKE_CURRENT_LIST_DIR}")
add_library(Melatonin::Inspector ALIAS melatonin_inspector)

if (MELATONIN_INSPECTOR_ENABLE_AUTOMATION)
    target_compile_definitions(melatonin_inspector INTERFACE MELATONIN_INSPECTOR_ENABLE_AUTOMATION=1)
endif ()

if (MelatoninInspector_IS_TOP_LEVEL)
    target_link_libraries("${TARGET_NAME}" PRIVATE melatonin_inspector
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

    if (MELATONIN_INSPECTOR_BUILD_DEMORUNNER_AUTOMATION)
        set(MELATONIN_DEMORUNNER_SOURCE_DIR "${JUCE_SOURCE_DIR}/examples/DemoRunner")
        set(MELATONIN_DEMORUNNER_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/melatonin_demorunner")
        file(MAKE_DIRECTORY "${MELATONIN_DEMORUNNER_GENERATED_DIR}/Source")
        file(MAKE_DIRECTORY "${MELATONIN_DEMORUNNER_GENERATED_DIR}/Source/Demos")

        file(READ "${MELATONIN_DEMORUNNER_SOURCE_DIR}/Source/Main.cpp" MELATONIN_DEMORUNNER_MAIN)
        string(REPLACE
            "#include <JuceHeader.h>"
            "#include <JuceHeader.h>\n#include <melatonin_inspector/melatonin_inspector.h>"
            MELATONIN_DEMORUNNER_MAIN
            "${MELATONIN_DEMORUNNER_MAIN}")
        string(REPLACE
            "setContentOwned (new MainComponent(), false);"
            "auto* mainComponent = new MainComponent();\n            setContentOwned (mainComponent, false);\n\n           #if MELATONIN_INSPECTOR_ENABLE_AUTOMATION\n            melatonin::AutomationOptions automationOptions;\n            automationOptions.sessionName = \"juce_demorunner\";\n            automationOptions.allowFileWrite = true;\n            automationOptions.artifactRoot = File::getSpecialLocation (File::tempDirectory);\n            automationInspector = std::make_unique<melatonin::Inspector> (*mainComponent, false);\n            automationInspector->enableAutomation (automationOptions);\n           #endif"
            MELATONIN_DEMORUNNER_MAIN
            "${MELATONIN_DEMORUNNER_MAIN}")
        string(REPLACE
            "    private:\n       #if JUCE_MAC || JUCE_WINDOWS || JUCE_LINUX || JUCE_BSD"
            "    private:\n        std::unique_ptr<melatonin::Inspector> automationInspector;\n\n       #if JUCE_MAC || JUCE_WINDOWS || JUCE_LINUX || JUCE_BSD"
            MELATONIN_DEMORUNNER_MAIN
            "${MELATONIN_DEMORUNNER_MAIN}")
        string(FIND "${MELATONIN_DEMORUNNER_MAIN}" "automationOptions.sessionName = \"juce_demorunner\"" MELATONIN_DEMORUNNER_SESSION_PATCH)
        string(FIND "${MELATONIN_DEMORUNNER_MAIN}" "std::unique_ptr<melatonin::Inspector> automationInspector" MELATONIN_DEMORUNNER_MEMBER_PATCH)
        if (MELATONIN_DEMORUNNER_SESSION_PATCH EQUAL -1 OR MELATONIN_DEMORUNNER_MEMBER_PATCH EQUAL -1)
            message(FATAL_ERROR "JUCE DemoRunner automation patch failed: Source/Main.cpp patch anchors changed")
        endif ()
        file(WRITE "${MELATONIN_DEMORUNNER_GENERATED_DIR}/Source/Main.cpp" "${MELATONIN_DEMORUNNER_MAIN}")

        file(READ "${MELATONIN_DEMORUNNER_SOURCE_DIR}/Source/Demos/JUCEDemos.cpp" MELATONIN_DEMORUNNER_DEMOS)
        string(REPLACE
            "#include \"../../../Assets/DemoUtilities.h\""
            "#include \"${JUCE_SOURCE_DIR}/examples/Assets/DemoUtilities.h\""
            MELATONIN_DEMORUNNER_DEMOS
            "${MELATONIN_DEMORUNNER_DEMOS}")
        string(REPLACE
            "    auto* currentDevice = sharedAudioDeviceManager->getCurrentAudioDevice();"
            "   #if MELATONIN_DEMORUNNER_SKIP_AUDIO_DEVICE_INITIALISE\n    ignoreUnused (numInputChannels, numOutputChannels);\n    return *sharedAudioDeviceManager;\n   #else\n\n    auto* currentDevice = sharedAudioDeviceManager->getCurrentAudioDevice();"
            MELATONIN_DEMORUNNER_DEMOS
            "${MELATONIN_DEMORUNNER_DEMOS}")
        string(REPLACE
            "    return *sharedAudioDeviceManager;\n}"
            "   #endif\n\n    return *sharedAudioDeviceManager;\n}"
            MELATONIN_DEMORUNNER_DEMOS
            "${MELATONIN_DEMORUNNER_DEMOS}")
        string(FIND "${MELATONIN_DEMORUNNER_DEMOS}" "MELATONIN_DEMORUNNER_SKIP_AUDIO_DEVICE_INITIALISE" MELATONIN_DEMORUNNER_AUDIO_PATCH)
        if (MELATONIN_DEMORUNNER_AUDIO_PATCH EQUAL -1)
            message(FATAL_ERROR "JUCE DemoRunner automation patch failed: Demos/JUCEDemos.cpp patch anchors changed")
        endif ()
        file(WRITE "${MELATONIN_DEMORUNNER_GENERATED_DIR}/Source/Demos/JUCEDemos.cpp" "${MELATONIN_DEMORUNNER_DEMOS}")

        juce_add_gui_app(melatonin-demorunner
            BUNDLE_ID                       com.melatonin.inspector.demorunner
            ICON_BIG                        "${MELATONIN_DEMORUNNER_SOURCE_DIR}/Source/JUCEAppIcon.png"
            ICON_COMPOSER_BUNDLE            "${MELATONIN_DEMORUNNER_SOURCE_DIR}/Source/AppIcon.icon"
            NEEDS_CURL                      TRUE
            NEEDS_WEB_BROWSER               TRUE
            MICROPHONE_PERMISSION_ENABLED   TRUE
            CAMERA_PERMISSION_ENABLED       TRUE
            BLUETOOTH_PERMISSION_ENABLED    TRUE
            FILE_SHARING_ENABLED            TRUE
            DOCUMENT_BROWSER_ENABLED        TRUE
            REQUIRES_FULL_SCREEN            FALSE)

        juce_generate_juce_header(melatonin-demorunner)

        target_sources(melatonin-demorunner PRIVATE
            "${MELATONIN_DEMORUNNER_SOURCE_DIR}/Source/Demos/DemoPIPs1.cpp"
            "${MELATONIN_DEMORUNNER_SOURCE_DIR}/Source/Demos/DemoPIPs2.cpp"
            "${MELATONIN_DEMORUNNER_GENERATED_DIR}/Source/Demos/JUCEDemos.cpp"
            "${MELATONIN_DEMORUNNER_GENERATED_DIR}/Source/Main.cpp"
            "${MELATONIN_DEMORUNNER_SOURCE_DIR}/Source/UI/DemoContentComponent.cpp"
            "${MELATONIN_DEMORUNNER_SOURCE_DIR}/Source/UI/MainComponent.cpp")

        if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            set_source_files_properties("${MELATONIN_DEMORUNNER_GENERATED_DIR}/Source/Demos/JUCEDemos.cpp"
                PROPERTIES COMPILE_OPTIONS "-Wno-unused-function")
        endif ()

        target_include_directories(melatonin-demorunner PRIVATE
            "${MELATONIN_DEMORUNNER_SOURCE_DIR}/Source"
            "${MELATONIN_DEMORUNNER_SOURCE_DIR}/Source/Demos")

        target_compile_definitions(melatonin-demorunner PRIVATE
            MELATONIN_INSPECTOR_ENABLE_AUTOMATION=1
            PIP_JUCE_EXAMPLES_DIRECTORY_STRING="${JUCE_SOURCE_DIR}/examples"
            JUCE_ASIO=1
            JUCE_ALLOW_STATIC_NULL_VARIABLES=0
            JUCE_CONTENT_SHARING=1
            JUCE_DEMO_RUNNER=1
            JUCE_PLUGINHOST_AU=1
            JUCE_PLUGINHOST_LV2=1
            JUCE_PLUGINHOST_VST3=1
            JUCE_PUSH_NOTIFICATIONS=1
            JUCE_STRICT_REFCOUNTEDPOINTER=1
            JUCE_UNIT_TESTS=1
            JUCE_USE_CAMERA=1
            JUCE_USE_CURL=1
            JUCE_USE_MP3AUDIOFORMAT=1
            JUCE_WEB_BROWSER=1
            MELATONIN_DEMORUNNER_SKIP_AUDIO_DEVICE_INITIALISE=1
            JUCE_SILENCE_XCODE_15_LINKER_WARNING=1)

        target_link_libraries(melatonin-demorunner PRIVATE
            melatonin_inspector
            juce::juce_analytics
            juce::juce_animation
            juce::juce_audio_utils
            juce::juce_box2d
            juce::juce_build_tools
            juce::juce_dsp
            juce::juce_javascript
            juce::juce_midi_ci
            juce::juce_opengl
            juce::juce_osc
            juce::juce_product_unlocking
            juce::juce_video
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)

        foreach(folder IN ITEMS ../Assets ../Audio ../DSP ../GUI ../Utilities)
            juce_add_bundle_resources_directory(melatonin-demorunner "${MELATONIN_DEMORUNNER_SOURCE_DIR}/${folder}")
        endforeach()

        if (NOT TARGET melatonin-ui)
            message(FATAL_ERROR "MELATONIN_INSPECTOR_BUILD_DEMORUNNER_AUTOMATION requires MELATONIN_INSPECTOR_BUILD_CLI=ON")
        endif ()

        juce_add_console_app(melatonin-demorunner-e2e VERSION 1.0.0)
        juce_generate_juce_header(melatonin-demorunner-e2e)
        target_sources(melatonin-demorunner-e2e PRIVATE tests/demorunner_e2e.cpp)
        target_compile_definitions(melatonin-demorunner-e2e PRIVATE
            JUCE_USE_CURL=0
            MELATONIN_DEMORUNNER_EXECUTABLE="$<TARGET_FILE:melatonin-demorunner>"
            MELATONIN_UI_EXECUTABLE="$<TARGET_FILE:melatonin-ui>")
        target_link_libraries(melatonin-demorunner-e2e PRIVATE
            juce::juce_core
            juce::juce_graphics
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)
        add_dependencies(melatonin-demorunner-e2e melatonin-demorunner melatonin-ui)
    endif ()
endif ()

# Assets are precompiled in the module to make it Projucer friendly
#
# To add or modify images:
#  * Uncomment the following CMake lines to generate the MelatoninInspectorAssets binary
#  * uncomment #include "InspectorBinaryData.h in misc.h and comment out the LatestCompiledAssets include
#  * build as you would normally via CMake
#  * run ./modules/melatonin_inspector/copy_cmake_assets.rb from your root PROJECT folder (not the melatonin_inspector folder)
#  * comment these lines back out and swap the misc.h include again

#file(GLOB_RECURSE MelatoninInspectorAssetFiles CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Assets/*")
#juce_add_binary_data(MelatoninInspectorAssets SOURCES ${MelatoninInspectorAssetFiles} HEADER_NAME InspectorBinaryData.h NAMESPACE InspectorBinaryData)
#set_target_properties(MelatoninInspectorAssets PROPERTIES POSITION_INDEPENDENT_CODE TRUE)
#target_link_libraries(melatonin_inspector INTERFACE MelatoninInspectorAssets)
