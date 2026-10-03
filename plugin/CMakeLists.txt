cmake_minimum_required(VERSION 3.25)
project(MaxCraft VERSION 0.0.1 LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Max Payne 2 is a 32-bit game: configure with -A Win32 (use the x86-* presets).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# MinHook v1.3.3 ships no CMake project: fetch the sources and build them as a static library.
include(FetchContent)
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        v1.3.3)
FetchContent_Populate(minhook)
add_library(minhook STATIC
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    ${minhook_SOURCE_DIR}/src/hde/hde32.c)
target_include_directories(minhook PUBLIC ${minhook_SOURCE_DIR}/include)

# SkyCraft's protocol header, pinned by the extern/SkyCraft submodule: both halves must match.
set(SKYCRAFT_DIR ${CMAKE_SOURCE_DIR}/../extern/SkyCraft)
if(NOT EXISTS ${SKYCRAFT_DIR}/protocol/skycraft_protocol.h)
    message(FATAL_ERROR "extern/SkyCraft is missing: run `git submodule update --init` (not --recursive).")
endif()

add_library(MaxCraft SHARED
    src/main.cpp
    src/Camera.cpp
    src/Characters.cpp
    src/Collision.cpp
    src/Config.cpp
    src/CrashLog.cpp
    src/Engine.cpp
    src/Input.cpp
    src/Launcher.cpp
    src/Link.cpp
    src/Log.cpp
    src/Mapping.cpp
    src/Render.cpp)

target_include_directories(MaxCraft PRIVATE src ${SKYCRAFT_DIR}/protocol)
target_link_libraries(MaxCraft PRIVATE minhook advapi32)
target_compile_definitions(MaxCraft PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(MaxCraft PRIVATE /W4 /utf-8 /permissive- /EHsc /wd4324)

# ASI loaders pick up *.asi files from the game folder.
set_target_properties(MaxCraft PROPERTIES SUFFIX ".asi")

# Copy into the game folder after each build when MAXPAYNE2_DIR is set.
if(DEFINED ENV{MAXPAYNE2_DIR})
    add_custom_command(TARGET MaxCraft POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:MaxCraft> "$ENV{MAXPAYNE2_DIR}"
        COMMENT "Deploying MaxCraft.asi to $ENV{MAXPAYNE2_DIR}")
endif()
