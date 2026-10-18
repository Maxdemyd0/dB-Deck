cmake_minimum_required(VERSION 3.24)
project(dbdeck_engine VERSION 1.0.0 LANGUAGES CXX)
if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT: no VC++ redist needed

option(DBDECK_BUILD_TESTS "Build dB-Deck engine tests" ON)

include(FetchContent)
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nlohmann_json)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus /wd4100 /wd4324)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
endif()

# --- Platform-independent core (graph, DSP, config, protocol) ---------------
add_library(dbdeck_core STATIC
    core/audio/AdaptiveStream.cpp
    core/audio/Format.cpp
    core/dsp/Effects.cpp
    core/graph/Model.cpp
    core/graph/Validation.cpp
    core/graph/Mixer.cpp
    core/graph/GraphCompiler.cpp
    core/config/ConfigStore.cpp
    core/config/VoicemeeterImport.cpp
    core/protocol/Protocol.cpp
    core/util/Log.cpp)
target_include_directories(dbdeck_core PUBLIC core)
target_link_libraries(dbdeck_core PUBLIC nlohmann_json::nlohmann_json)

# --- Windows integration (WASAPI, sessions, devices, IPC) -------------------
if(WIN32 AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/win/main.cpp)
    add_library(dbdeck_win STATIC
        win/ComUtil.cpp
        win/DeviceManager.cpp
        win/SessionManager.cpp
        win/IconCache.cpp
        win/EndpointVolumeController.cpp
        win/AppOutputRouting.cpp
        win/WasapiStreams.cpp
        win/AudioEngine.cpp
        win/PipeServer.cpp
        win/EngineService.cpp)
    target_include_directories(dbdeck_win PUBLIC win)
    target_link_libraries(dbdeck_win PUBLIC dbdeck_core ole32 oleaut32 uuid avrt mmdevapi
        windowscodecs shell32 version advapi32 user32 gdi32 runtimeobject)

    add_executable(dbdeck-engine win/main.cpp win/engine.rc)
    target_link_libraries(dbdeck-engine PRIVATE dbdeck_win)
endif()

if(DBDECK_BUILD_TESTS)
    enable_testing()
    add_executable(dbdeck-tests tests/test_main.cpp tests/test_core.cpp tests/test_mixer.cpp tests/test_config.cpp tests/test_import.cpp)
    target_link_libraries(dbdeck-tests PRIVATE dbdeck_core)
    add_test(NAME core COMMAND dbdeck-tests)

    if(TARGET dbdeck_win)
        add_executable(dbdeck-wintests tests/test_main.cpp tests/test_windows.cpp)
        target_link_libraries(dbdeck-wintests PRIVATE dbdeck_win)
        # Touches real audio devices; run explicitly (see docs/TESTING.md).
    endif()
endif()
