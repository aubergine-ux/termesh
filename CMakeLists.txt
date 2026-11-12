cmake_minimum_required(VERSION 3.20)

project(termesh
    VERSION 0.1.0
    DESCRIPTION "3D model viewer for the terminal with a from-scratch software rasterizer"
    LANGUAGES CXX)

option(TERMESH_BUILD_TESTS "Build the unit tests" ON)
option(TERMESH_NATIVE "Optimise for the build machine's CPU (-march=native)" OFF)
option(TERMESH_STATIC "Link the C/C++ runtime statically (for release binaries)" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(TERMESH_STATIC AND MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(Threads REQUIRED)

# Warnings for our own code. Third-party headers are included as SYSTEM so
# they stay quiet.
add_library(termesh_warnings INTERFACE)
if(MSVC)
    target_compile_options(termesh_warnings INTERFACE /W4 /permissive- /utf-8)
    target_compile_definitions(termesh_warnings INTERFACE
        NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(termesh_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
endif()

# ---------------------------------------------------------------- library ---
# Everything except main() lives in a static library so the tests can link it.
add_library(termesh_third_party STATIC third_party/impl.cpp)
target_include_directories(termesh_third_party SYSTEM PUBLIC third_party)
if(MSVC)
    target_compile_options(termesh_third_party PRIVATE /w)
    target_compile_definitions(termesh_third_party PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(termesh_third_party PRIVATE -w)
endif()

if(WIN32)
    set(TERMESH_TERMINAL_SOURCE src/input/terminal_win.cpp)
else()
    set(TERMESH_TERMINAL_SOURCE src/input/terminal_posix.cpp)
endif()

add_library(termesh_core STATIC
    src/app/capabilities.cpp
    src/app/options.cpp
    src/app/viewer.cpp
    src/core/texture.cpp
    src/core/thread_pool.cpp
    src/export/gif.cpp
    src/export/offscreen.cpp
    src/input/events.cpp
    ${TERMESH_TERMINAL_SOURCE}
    src/output/cell_backends.cpp
    src/output/cell_grid.cpp
    src/output/pixel_backends.cpp
    src/pipeline/clip.cpp
    src/pipeline/renderer.cpp
    src/pipeline/resolve.cpp
    src/scene/gltf_loader.cpp
    src/scene/loader.cpp
    src/scene/model.cpp
    src/scene/obj_loader.cpp
    src/scene/primitives.cpp
)
target_include_directories(termesh_core PUBLIC src)
target_include_directories(termesh_core SYSTEM PUBLIC third_party)
target_link_libraries(termesh_core PUBLIC Threads::Threads termesh_third_party PRIVATE termesh_warnings)
target_compile_definitions(termesh_core PUBLIC TERMESH_VERSION="${PROJECT_VERSION}")

if(TERMESH_NATIVE AND NOT MSVC)
    target_compile_options(termesh_core PUBLIC -march=native)
endif()

# ------------------------------------------------------------- executable ---
add_executable(termesh src/app/main.cpp)
target_link_libraries(termesh PRIVATE termesh_core termesh_warnings)
if(TERMESH_STATIC AND NOT MSVC AND NOT APPLE)
    target_link_options(termesh PRIVATE -static)
endif()

install(TARGETS termesh RUNTIME DESTINATION bin)

# ------------------------------------------------------------------ tests ---
if(TERMESH_BUILD_TESTS)
    enable_testing()
    add_executable(termesh_tests
        tests/test_main.cpp
        tests/test_clip.cpp
        tests/test_math.cpp
        tests/test_raster.cpp
    )
    target_link_libraries(termesh_tests PRIVATE termesh_core termesh_warnings)
    add_test(NAME termesh_tests COMMAND termesh_tests)
endif()
