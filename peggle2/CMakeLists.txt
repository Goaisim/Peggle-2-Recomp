# peggle2 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
if(APPLE)
    project(peggle2 LANGUAGES C CXX OBJC OBJCXX)
else()
    project(peggle2 LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Git QUIET)
set(PEGGLE2_GIT_SHA "")
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
        OUTPUT_VARIABLE PEGGLE2_GIT_SHA
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()
if(NOT PEGGLE2_GIT_SHA AND DEFINED ENV{GITHUB_SHA} AND NOT "$ENV{GITHUB_SHA}" STREQUAL "")
    string(SUBSTRING "$ENV{GITHUB_SHA}" 0 7 PEGGLE2_GIT_SHA)
endif()
if(NOT PEGGLE2_GIT_SHA)
    set(PEGGLE2_GIT_SHA "unknown")
endif()

# Sources
set(PEGGLE2_SOURCES
    src/main.cpp
    src/rex_engine_monitor.cpp
)

if(APPLE AND IOS)
    if(NOT REXSDK_DIR)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../sdk/CMakeLists.txt")
            set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sdk" CACHE PATH "Path to rexglue-sdk")
        elseif(EXISTS "C:/Project/rexglue-sdk/CMakeLists.txt")
            set(REXSDK_DIR "C:/Project/rexglue-sdk" CACHE PATH "Path to rexglue-sdk")
        endif()
    endif()

    if(REXSDK_DIR)
        add_subdirectory("${REXSDK_DIR}" rexglue-sdk)
    else()
        find_package(rexglue REQUIRED CONFIG)
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/sources.cmake")
        include(generated/default/sources.cmake)
        set(PEGGLE2_GENERATED_SOURCES ${GENERATED_SOURCES})
    endif()

    set(PEGGLE2_IOS_SOURCES
        ios/main_ios.mm
        ios/rexvideo_metal.mm
        ios/msl_translator.cpp
        ios/rexaudio_ios.mm
    )
    set_source_files_properties(
        ios/main_ios.mm
        ios/rexvideo_metal.mm
        ios/rexaudio_ios.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc"
    )

    add_executable(peggle2 MACOSX_BUNDLE
        ${PEGGLE2_SOURCES}
        ${PEGGLE2_IOS_SOURCES}
    )

    if(PEGGLE2_GENERATED_SOURCES)
        add_library(peggle2_recomp OBJECT ${PEGGLE2_GENERATED_SOURCES})
        target_include_directories(peggle2_recomp PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/generated/default
        )
        target_link_libraries(peggle2_recomp PRIVATE rex::runtime)
        target_precompile_headers(peggle2_recomp PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/peggle2_pch.h")
        target_compile_options(peggle2_recomp PRIVATE -gline-tables-only $<$<CONFIG:Release>:-O3>)
        target_link_libraries(peggle2 PRIVATE peggle2_recomp)
    endif()

    target_include_directories(peggle2 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/ios
        ${CMAKE_CURRENT_SOURCE_DIR}/generated/default
        $<$<BOOL:${REXSDK_DIR}>:${REXSDK_DIR}/src>
    )
    target_compile_definitions(peggle2 PRIVATE
        REXGLUE_BUILD_CONFIG="$<CONFIG>"
        REX_BUILD_SHA="${PEGGLE2_GIT_SHA}"
        SIMDE_ENABLE_NATIVE_ALIASES
    )
    target_compile_options(peggle2 PRIVATE $<$<CONFIG:Release>:-O3>)

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/metadata/icons" AND COMMAND rexglue_embed_metadata)
        rexglue_embed_metadata(peggle2
            DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/metadata/icons"
            PREFIX "icons")
    endif()
    if(TARGET rex::runtime AND REXSDK_DIR AND EXISTS "${REXSDK_DIR}/src/ui/rex_app.cpp")
        target_sources(peggle2 PRIVATE "${REXSDK_DIR}/src/ui/rex_app.cpp")
    endif()

    target_link_libraries(peggle2 PRIVATE
        rex::runtime
        rexgpu-xenos
        rexui
        rexcore
        rexaudio
        rexinput
        imgui::imgui
        xxHash::xxhash
        "-framework UIKit"
        "-framework Foundation"
        "-framework Metal"
        "-framework QuartzCore"
        "-framework AudioToolbox"
        "-framework AVFoundation"
        "-framework CoreGraphics"
        "-framework GameController"
        "-framework UniformTypeIdentifiers"
    )

    set_target_properties(peggle2 PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/ios/Info.plist.in"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.peggle2recomp.ios"
        XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/ios/Entitlements.plist"
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
        XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "16.0"
        XCODE_ATTRIBUTE_ENABLE_BITCODE "NO"
        XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC "YES"
        XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
        XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO"
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY ""
    )
else()
    if(WIN32)
        add_executable(peggle2 WIN32 ${PEGGLE2_SOURCES} resources/peggle2.rc)
    else()
        add_executable(peggle2 ${PEGGLE2_SOURCES})
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../game/default.xex"
       OR NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/sources.cmake")
        include(generated/rexglue.cmake)
        rexglue_setup_target(peggle2 GPU_PLUGINS xenos)
    else()
        # Asset-free builds (CI and clean GitHub checkouts without ../game/default.xex)
        # compile committed generated/default C++ sources without registering a codegen edge.
        if(REXSDK_DIR)
            add_subdirectory("${REXSDK_DIR}" rexglue-sdk)
        else()
            find_package(rexglue 0.10.0 REQUIRED CONFIG)
        endif()
        include(generated/default/sources.cmake)
        add_library(peggle2_recomp OBJECT ${GENERATED_SOURCES})
        target_include_directories(peggle2_recomp PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/generated/default
        )
        target_link_libraries(peggle2_recomp PRIVATE rex::runtime)
        rexglue_apply_target_settings(peggle2_recomp)
        if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(peggle2_recomp PRIVATE -fasync-exceptions -gline-tables-only)
        elseif(WIN32 AND MSVC)
            target_compile_options(peggle2_recomp PRIVATE /EHa)
        elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(peggle2_recomp PRIVATE -gline-tables-only)
        endif()
        target_precompile_headers(peggle2_recomp PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/peggle2_pch.h")

        target_include_directories(peggle2 PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/generated/default
        )
        target_link_libraries(peggle2 PRIVATE peggle2_recomp rex::runtime)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/metadata/icons")
            rexglue_embed_metadata(peggle2
                DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/metadata/icons"
                PREFIX "icons")
        endif()
        rexglue_configure_target(peggle2 GPU_PLUGINS xenos)
    endif()

    target_compile_definitions(peggle2 PRIVATE REX_BUILD_SHA="${PEGGLE2_GIT_SHA}")
    target_compile_options(peggle2 PRIVATE $<$<CONFIG:Release>:-O3>)

    if(WIN32)
        target_link_options(peggle2 PRIVATE "LINKER:/MANIFESTUAC:level='asInvoker' uiAccess='false'")
    endif()
endif()
