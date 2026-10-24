cmake_minimum_required(VERSION 3.25)
set(FAD_VST3_SDK_ROOT "" CACHE PATH "Existing, unmodified official SDK at the reviewed revision")
if(NOT EXISTS "${FAD_VST3_SDK_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "Provide the independent official SDK with -DFAD_VST3_SDK_ROOT; no SDK is downloaded")
endif()
find_package(Git REQUIRED)
function(fad_demo_require_revision path revision)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${path}" rev-parse HEAD
        OUTPUT_VARIABLE actual OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${path}" status --porcelain --untracked-files=no
        OUTPUT_VARIABLE dirty OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    if(NOT actual STREQUAL revision OR NOT dirty STREQUAL "")
        message(FATAL_ERROR "VST3 example requires the clean pinned official SDK: ${path}")
    endif()
endfunction()
fad_demo_require_revision("${FAD_VST3_SDK_ROOT}" "3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96")
fad_demo_require_revision("${FAD_VST3_SDK_ROOT}/base" "fcf9da0bd27a16f7f03773a3a39822f28f5c8477")
fad_demo_require_revision("${FAD_VST3_SDK_ROOT}/cmake" "054c9143cbb8d47fc4694e473f2ee3b4d951a8f5")
fad_demo_require_revision("${FAD_VST3_SDK_ROOT}/pluginterfaces" "4f547e8e102b47de4a8b8aaf343c73b700786372")
fad_demo_require_revision("${FAD_VST3_SDK_ROOT}/public.sdk" "586dc5e6c8012c3e4b01c79389375cbe96bdb1da")

# No install/link, background scan, wrapper copying or publishing side effect.
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "No SDK plugin examples" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "No SDK GUI hosts" FORCE)
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "No VSTGUI dependency" FORCE)
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "No global plugin link" FORCE)
set(SMTG_CREATE_MODULE_INFO OFF CACHE BOOL "No post-build module tool/signing" FORCE)
set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "Validation is an explicit separate run" FORCE)
set(SMTG_DISABLE_CODE_SIGNING ON CACHE BOOL "No signing identity" FORCE)
add_subdirectory("${FAD_VST3_SDK_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/sdk" EXCLUDE_FROM_ALL)
set(SDK_ROOT "${FAD_VST3_SDK_ROOT}")
list(APPEND CMAKE_MODULE_PATH "${SDK_ROOT}/cmake/modules")
include(SMTG_VST3_SDK)
smtg_enable_vst3_sdk()

# The SDK's high-level helper unconditionally adds a post-build codesign on
# Xcode >= 15, even for Ninja with signing disabled. Compose its official
# packaging helpers directly, retaining the SDK's platform entry point.
add_library(FadEngineDemo MODULE DemoPlugin.cpp)
smtg_target_set_vst_win_architecture_name(FadEngineDemo)
smtg_target_make_plugin_package(FadEngineDemo "FAD Engine Community Demo" vst3)
target_link_libraries(FadEngineDemo PRIVATE FAD::EngineCore sdk)
target_compile_features(FadEngineDemo PRIVATE cxx_std_20)
set_target_properties(FadEngineDemo PROPERTIES CXX_EXTENSIONS OFF)
if(APPLE)
    smtg_target_set_bundle(FadEngineDemo
        BUNDLE_IDENTIFIER "com.fadrecords.engine.communitydemo"
        COMPANY_NAME "FAD Engine Examples")
    # The SDK helper's metadata properties are Xcode-only. Ninja also needs
    # an explicit plugin plist and the same independent product identity.
    set_target_properties(FadEngineDemo PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.fadrecords.engine.communitydemo"
        MACOSX_BUNDLE_BUNDLE_VERSION "0.1.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0")
endif()
configure_file("${FAD_VST3_SDK_ROOT}/LICENSE.txt" "${CMAKE_CURRENT_BINARY_DIR}/VST3-SDK-LICENSE.txt" COPYONLY)
get_target_property(FAD_DEMO_PACKAGE FadEngineDemo SMTG_PLUGIN_PACKAGE_PATH)
# Ninja/Makefile relink when distributed license/notice inputs change, so the
# post-build bundle resources cannot silently retain an older license text.
set_property(TARGET FadEngineDemo APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../../LICENSE"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../NOTICE.md"
    "${FAD_VST3_SDK_ROOT}/LICENSE.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../assets/steinberg/vst-compatible.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../assets/steinberg/README.md")
add_custom_command(TARGET FadEngineDemo POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${FAD_DEMO_PACKAGE}/Contents/Resources"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${FAD_VST3_SDK_ROOT}/LICENSE.txt"
        "${FAD_DEMO_PACKAGE}/Contents/Resources/VST3-SDK-LICENSE.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/../../NOTICE.md"
        "${FAD_DEMO_PACKAGE}/Contents/Resources/ENGINE-NOTICE.md"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/../../LICENSE"
        "${FAD_DEMO_PACKAGE}/Contents/Resources/LICENSE"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${FAD_DEMO_PACKAGE}/Contents/Resources/assets/steinberg"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/../../assets/steinberg/vst-compatible.png"
        "${FAD_DEMO_PACKAGE}/Contents/Resources/assets/steinberg/vst-compatible.png"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/../../assets/steinberg/README.md"
        "${FAD_DEMO_PACKAGE}/Contents/Resources/assets/steinberg/README.md" VERBATIM)

if(APPLE)
    add_executable(FadEngineDemoHost DemoHost.cpp
        "${SDK_ROOT}/public.sdk/source/common/memorystream.cpp"
        "${SDK_ROOT}/public.sdk/source/vst/hosting/plugprovider.cpp"
        "${SDK_ROOT}/public.sdk/source/vst/hosting/module_mac.mm")
    set_source_files_properties("${SDK_ROOT}/public.sdk/source/vst/hosting/module_mac.mm"
        PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    target_link_libraries(FadEngineDemoHost PRIVATE sdk_hosting)
    find_library(FAD_DEMO_COCOA_FRAMEWORK Cocoa REQUIRED)
    target_link_libraries(FadEngineDemoHost PRIVATE "${FAD_DEMO_COCOA_FRAMEWORK}")
    target_compile_features(FadEngineDemoHost PRIVATE cxx_std_20)
    set_target_properties(FadEngineDemoHost PROPERTIES CXX_EXTENSIONS OFF)
    set(FAD_DEMO_RT_PROBE_LIBRARY "" CACHE FILEPATH "Optional existing local RT audit library, host probe only")
    if(FAD_DEMO_RT_PROBE_LIBRARY)
        if(NOT EXISTS "${FAD_DEMO_RT_PROBE_LIBRARY}")
            message(FATAL_ERROR "RT probe library does not exist")
        endif()
        target_link_libraries(FadEngineDemoHost PRIVATE "${FAD_DEMO_RT_PROBE_LIBRARY}")
        target_compile_definitions(FadEngineDemoHost PRIVATE FAD_DEMO_RT_PROBE=1)
    endif()
endif()
