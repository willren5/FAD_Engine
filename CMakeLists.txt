cmake_minimum_required(VERSION 3.24)
project(FadEngine VERSION 0.1.0 LANGUAGES CXX)

option(FAD_ENGINE_BUILD_TESTS "Build dependency-free engine contract tests" ${PROJECT_IS_TOP_LEVEL})
option(FAD_ENGINE_BUILD_RENDERER "Build the standalone WAV renderer" ${PROJECT_IS_TOP_LEVEL})
option(FAD_ENGINE_BUILD_MINIMAL_EXAMPLE "Build the small standalone C++ consumer" ${PROJECT_IS_TOP_LEVEL})
option(FAD_ENGINE_BUILD_VST3_DEMO "Build the optional independent-SDK VST3 example" OFF)
find_package(Threads REQUIRED)

option(FAD_ENGINE_BUILD_TEST_VARIANT "Build a private instrumented variant from the same sources" OFF)

set(FAD_ENGINE_SOURCES src/ParameterSnapshot.cpp src/WavetableData.cpp src/WavetableBank.cpp
    src/Lfo.cpp src/ModMatrix.cpp src/AudioSafetyStage.cpp src/FilterBlock.cpp src/OscillatorBlock.cpp
    src/VoiceCore.cpp src/RenderEngine.cpp src/CpuMeter.cpp src/ClipDetector.cpp src/SignalFlowGraph.cpp src/fx/DriveFx.cpp
    src/fx/BitcrusherFx.cpp src/fx/CompressorFx.cpp src/fx/Eq3BandFx.cpp src/fx/LimiterFx.cpp src/fx/PhaserFx.cpp src/fx/FlangerFx.cpp src/fx/DelayFx.cpp src/fx/ReverbFx.cpp src/fx/FilterFx.cpp src/fx/StereoFx.cpp src/fx/ChorusFx.cpp src/fx/DistortionFx.cpp src/fx/DynamicFxChain.cpp src/fx/FxFactory.cpp)
add_library(FadEngineCore STATIC ${FAD_ENGINE_SOURCES})
add_library(FAD::EngineCore ALIAS FadEngineCore)
target_compile_features(FadEngineCore PUBLIC cxx_std_20)
target_link_libraries(FadEngineCore PUBLIC Threads::Threads)
set_target_properties(FadEngineCore PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON EXPORT_NAME EngineCore)
target_include_directories(FadEngineCore PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

if(FAD_ENGINE_BUILD_RENDERER)
    add_executable(FadEngineRender examples/headless/RenderWave.cpp)
    target_link_libraries(FadEngineRender PRIVATE FAD::EngineCore)
    set_target_properties(FadEngineRender PROPERTIES CXX_EXTENSIONS OFF)
endif()

if(FAD_ENGINE_BUILD_VST3_DEMO)
    add_subdirectory(examples/vst3)
endif()

if(FAD_ENGINE_BUILD_TEST_VARIANT)
    # No algorithm fork: only private diagnostics and test-only controls differ.
    # This target is never installed or exported with the production library.
    add_library(FadEngineCoreForTests STATIC ${FAD_ENGINE_SOURCES})
    add_library(FAD::EngineCoreForTests ALIAS FadEngineCoreForTests)
    target_compile_features(FadEngineCoreForTests PUBLIC cxx_std_20)
    target_link_libraries(FadEngineCoreForTests PUBLIC Threads::Threads)
    target_compile_definitions(FadEngineCoreForTests PUBLIC FAD_ENGINE_TEST_HOOKS=1)
    target_include_directories(FadEngineCoreForTests PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    set_target_properties(FadEngineCoreForTests PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
endif()

if(FAD_ENGINE_BUILD_TESTS)
    enable_testing()
    add_executable(FadEngineCoreContractTests tests/CoreContractTests.cpp)
    set_target_properties(FadEngineCoreContractTests PROPERTIES CXX_EXTENSIONS OFF)
    target_link_libraries(FadEngineCoreContractTests PRIVATE FAD::EngineCore)
    add_test(NAME FadEngineCoreContractTests COMMAND FadEngineCoreContractTests)
    find_package(Threads REQUIRED)
    add_executable(FadEngineRuntimePrimitiveTests tests/RuntimePrimitiveTests.cpp)
    target_link_libraries(FadEngineRuntimePrimitiveTests PRIVATE FAD::EngineCore Threads::Threads)
    set_target_properties(FadEngineRuntimePrimitiveTests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME FadEngineRuntimePrimitiveTests COMMAND FadEngineRuntimePrimitiveTests)
    set_tests_properties(FadEngineRuntimePrimitiveTests PROPERTIES TIMEOUT 30)
    add_executable(FadEngineAssetPublicationTests tests/AssetPublicationTests.cpp)
    target_link_libraries(FadEngineAssetPublicationTests PRIVATE FAD::EngineCore Threads::Threads)
    set_target_properties(FadEngineAssetPublicationTests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME FadEngineAssetPublicationTests COMMAND FadEngineAssetPublicationTests)
    set_tests_properties(FadEngineAssetPublicationTests PROPERTIES TIMEOUT 30)
    add_executable(FadEngineVoiceCoreTests tests/VoiceCoreTests.cpp)
    target_link_libraries(FadEngineVoiceCoreTests PRIVATE FAD::EngineCore)
    set_target_properties(FadEngineVoiceCoreTests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME FadEngineVoiceCoreTests COMMAND FadEngineVoiceCoreTests)
    set_tests_properties(FadEngineVoiceCoreTests PROPERTIES TIMEOUT 30)
    add_executable(FadEngineRenderEngineTests tests/RenderEngineTests.cpp)
    target_link_libraries(FadEngineRenderEngineTests PRIVATE FAD::EngineCore)
    set_target_properties(FadEngineRenderEngineTests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME FadEngineRenderEngineTests COMMAND FadEngineRenderEngineTests)
    set_tests_properties(FadEngineRenderEngineTests PROPERTIES TIMEOUT 30)
    add_executable(FadEngineFullRenderTests tests/FullRenderTests.cpp)
    target_link_libraries(FadEngineFullRenderTests PRIVATE FAD::EngineCore)
    set_target_properties(FadEngineFullRenderTests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME FadEngineFullRenderTests COMMAND FadEngineFullRenderTests)
    set_tests_properties(FadEngineFullRenderTests PROPERTIES TIMEOUT 30)
endif()

if(FAD_ENGINE_BUILD_MINIMAL_EXAMPLE)
    add_subdirectory(examples/minimal)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
configure_package_config_file(cmake/FadEngineConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/FadEngineConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/FadEngine")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/FadEngineConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY ExactVersion)
install(TARGETS FadEngineCore EXPORT FadEngineTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT FadEngineTargets NAMESPACE FAD:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FadEngine)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/FadEngineConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/FadEngineConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/FadEngine")
install(FILES LICENSE NOTICE.md THIRD_PARTY_NOTICES.md
    DESTINATION "${CMAKE_INSTALL_DATADIR}/FadEngine")
install(DIRECTORY legal/ DESTINATION "${CMAKE_INSTALL_DATADIR}/FadEngine/legal"
    FILES_MATCHING PATTERN "*.md")
install(FILES docs/LICENSING.zh-CN.md docs/PROVENANCE.md
    DESTINATION "${CMAKE_INSTALL_DATADIR}/FadEngine/docs")
install(DIRECTORY assets/steinberg/ DESTINATION "${CMAKE_INSTALL_DATADIR}/FadEngine/assets/steinberg"
    FILES_MATCHING PATTERN "*.png" PATTERN "*.md")
