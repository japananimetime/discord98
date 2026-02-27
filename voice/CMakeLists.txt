cmake_minimum_required(VERSION 3.16)
project(discord-voice VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Options ---
option(DV_BUILD_EXAMPLE "Build the example/test program" ON)

# --- Library sources ---
set(VOICE_SOURCES
    src/udp_socket.cpp
    src/voice_client.cpp
    src/audio_engine.cpp
    src/audio_devices.cpp
    src/miniaudio_impl.cpp
)

set(RNNOISE_SOURCES
    deps/rnnoise/denoise.c
    deps/rnnoise/rnn.c
    deps/rnnoise/rnn_data.c
    deps/rnnoise/pitch.c
    deps/rnnoise/kiss_fft.c
    deps/rnnoise/celt_lpc.c
)

set(VOICE_HEADERS
    include/discord_voice.h
    include/voice_types.h
    include/voice_client.h
    include/udp_socket.h
    include/audio_engine.h
    include/audio_devices.h
)

add_library(discord-voice STATIC ${VOICE_SOURCES} ${RNNOISE_SOURCES} ${VOICE_HEADERS})

target_include_directories(discord-voice PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/deps
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/rnnoise
)

# --- Find libsodium ---
# Try pkg-config first, then fall back to manual paths
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(SODIUM QUIET libsodium)
endif()

if(NOT SODIUM_FOUND)
    # Manual find for Windows
    find_path(SODIUM_INCLUDE_DIRS sodium.h
        PATHS
            "${CMAKE_CURRENT_SOURCE_DIR}/deps/libsodium-msvc/libsodium/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/deps/libsodium/include"
            "$ENV{SODIUM_DIR}/include"
            "C:/libsodium/include"
    )
    find_library(SODIUM_LIBRARIES
        NAMES sodium libsodium
        PATHS
            "${CMAKE_CURRENT_SOURCE_DIR}/deps/libsodium-msvc/libsodium/x64/Release/v143/static"
            "${CMAKE_CURRENT_SOURCE_DIR}/deps/libsodium/lib"
            "$ENV{SODIUM_DIR}/lib"
            "C:/libsodium/lib"
    )
    if(SODIUM_INCLUDE_DIRS AND SODIUM_LIBRARIES)
        set(SODIUM_FOUND TRUE)
    endif()
endif()

if(SODIUM_FOUND)
    target_include_directories(discord-voice PUBLIC ${SODIUM_INCLUDE_DIRS})
    target_link_libraries(discord-voice PUBLIC ${SODIUM_LIBRARIES})
    target_compile_definitions(discord-voice PUBLIC SODIUM_STATIC)
    message(STATUS "Found libsodium: ${SODIUM_LIBRARIES}")
else()
    message(FATAL_ERROR "libsodium not found. Set SODIUM_DIR environment variable or install it.")
endif()

# --- Find Opus ---
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(OPUS QUIET opus)
endif()

if(NOT OPUS_FOUND)
    find_path(OPUS_INCLUDE_DIRS opus/opus.h
        PATHS
            "${CMAKE_CURRENT_SOURCE_DIR}/deps/opus-install/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/deps/opus/include"
            "$ENV{OPUS_DIR}/include"
            "C:/opus/include"
    )
    find_library(OPUS_LIBRARIES
        NAMES opus libopus
        PATHS
            "${CMAKE_CURRENT_SOURCE_DIR}/deps/opus-install/lib"
            "${CMAKE_CURRENT_SOURCE_DIR}/deps/opus/lib"
            "$ENV{OPUS_DIR}/lib"
            "C:/opus/lib"
    )
    if(OPUS_INCLUDE_DIRS AND OPUS_LIBRARIES)
        set(OPUS_FOUND TRUE)
    endif()
endif()

if(OPUS_FOUND)
    target_include_directories(discord-voice PUBLIC ${OPUS_INCLUDE_DIRS})
    target_link_libraries(discord-voice PUBLIC ${OPUS_LIBRARIES})
    message(STATUS "Found Opus: ${OPUS_LIBRARIES}")
else()
    message(FATAL_ERROR "Opus not found. Set OPUS_DIR environment variable or install it.")
endif()

# --- Platform libraries ---
if(WIN32)
    target_link_libraries(discord-voice PRIVATE ws2_32 winmm ole32)
    target_compile_definitions(discord-voice PRIVATE
        _WIN32_WINNT=0x0601  # Windows 7+
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
endif()

# --- Example program ---
if(DV_BUILD_EXAMPLE)
    add_executable(voice-test example/main.cpp)
    target_link_libraries(voice-test PRIVATE discord-voice)
    if(WIN32)
        target_link_libraries(voice-test PRIVATE ws2_32 winmm ole32)
    endif()
endif()
