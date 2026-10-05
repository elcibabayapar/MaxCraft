cmake_minimum_required(VERSION 3.25)
# The one place the version is written down. CMake hands it to main.cpp for the log banner, and
# tools/package.ps1 reads it back out of this line to name dist\MaxCraft-<version>.zip, so the two
# can no longer disagree - the log said 0.1.1, package.ps1's default said 0.1.1 and this line, which
# nothing read, still said 0.0.1.
project(MaxCraft VERSION 0.1.1 LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Max Payne 2 is a 32-bit game: configure with -A Win32 (use the x86-* presets).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# MinHook v1.3.3 ships no CMake project (there is no CMakeLists.txt anywhere in the tag), so
# MakeAvailable only fetches and populates: nothing is added as a subdirectory and no target appears,
# which is what the guard below asserts. MakeAvailable instead of the deprecated FetchContent_Populate
# because the latter warns on every configure - "the ability to call it with declared details will be
# removed completely in a future version" - and would then take the build down with it. GIT_SHALLOW is
# measured to be honoured here (the clone has .git/shallow and one commit), but it is kept either way:
# MinHook is a build input fetched over the network, and its full history is not one.
include(FetchContent)
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        v1.3.3
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(minhook)
if(TARGET minhook)
    message(FATAL_ERROR "MinHook v1.3.3 now ships a CMake project: its own DLL target would be built here too, and the sources below would clash with it.")
endif()
add_library(minhook STATIC
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    # hde/hde32.c only. hde/hde64.c is left out on purpose: this is an x86-only target (the size check
    # at the top), and MinHook's 64-bit disassembler is dead weight in a 32-bit Max Payne 2 process.
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
target_compile_definitions(MaxCraft PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    # main.cpp's log banner. The quotes matter: they are what makes this a string literal rather than
    # a bare token, and a generator that passed them unescaped would fail to compile on `0.1.1` rather
    # than log a number. main.cpp keeps a literal fallback so a plain compiler command still works.
    MCRAFT_VERSION="${PROJECT_VERSION}")
target_compile_options(MaxCraft PRIVATE /W4 /utf-8 /permissive- /EHsc /wd4324)

# ASI loaders pick up *.asi files from the game folder.
set_target_properties(MaxCraft PROPERTIES SUFFIX ".asi")

# Copy into the game folder after each build when MAXPAYNE2_DIR is set.
if(DEFINED ENV{MAXPAYNE2_DIR})
    add_custom_command(TARGET MaxCraft POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:MaxCraft> "$ENV{MAXPAYNE2_DIR}"
        COMMENT "Deploying MaxCraft.asi to $ENV{MAXPAYNE2_DIR}")
endif()
