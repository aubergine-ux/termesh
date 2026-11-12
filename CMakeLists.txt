cmake_minimum_required(VERSION 3.20)

project(termesh
    VERSION 0.2.0
    DESCRIPTION "3D model viewer for the terminal with a from-scratch software rasterizer"
    LANGUAGES C CXX)

option(TERMESH_BUILD_TESTS "Build the unit tests" ON)
option(TERMESH_NATIVE "Optimise for the build machine's CPU (-march=native)" OFF)
option(TERMESH_STATIC "Link the C/C++ runtime statically (for release binaries)" OFF)
option(TERMESH_BUILD_LIBRARY "Build libtermesh, the renderer as a library with a C API" ON)
option(TERMESH_LIBRARY_STATIC "Build libtermesh as a static library rather than a shared one" OFF)
option(TERMESH_BUILD_PLUGINS "Build the example plugins" ON)
option(TERMESH_PYTHON "Build the Python module (needs pybind11)" OFF)
option(TERMESH_ASSIMP "Read more formats (FBX, COLLADA, 3DS, ...) through the Assimp library" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# The same objects go into the program, the tests and the shared library, so
# they are all built position-independent - and with nothing exported: the
# library's public symbols are the ones termesh.h marks.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(TERMESH_STATIC AND MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(Threads REQUIRED)

# WebAssembly (configure with emcmake). Exceptions and SIMD both have to be
# asked for, when compiling and when linking; the SSE2 code paths compile to
# WebAssembly's own 128-bit SIMD. There are no threads: a page would need
# special server headers to be allowed them.
if(EMSCRIPTEN)
    add_compile_options(-fwasm-exceptions -msimd128 -msse2)
    add_link_options(-fwasm-exceptions -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=2097152)
    set(TERMESH_BUILD_LIBRARY OFF CACHE BOOL "" FORCE)
    set(TERMESH_PYTHON OFF CACHE BOOL "" FORCE)
endif()

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
# Everything except main() is compiled once, into object libraries, and then
# linked into each of the things built from it: the program, the tests and
# libtermesh.
add_library(termesh_third_party OBJECT third_party/impl.cpp)
target_include_directories(termesh_third_party SYSTEM PUBLIC third_party)
if(MSVC)
    target_compile_options(termesh_third_party PRIVATE /w)
    target_compile_definitions(termesh_third_party PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(termesh_third_party PRIVATE -w)
endif()

if(EMSCRIPTEN)
    set(TERMESH_TERMINAL_SOURCE src/input/terminal_wasm.cpp)
elseif(WIN32)
    set(TERMESH_TERMINAL_SOURCE src/input/terminal_win.cpp)
else()
    set(TERMESH_TERMINAL_SOURCE src/input/terminal_posix.cpp)
endif()

add_library(termesh_core OBJECT
    src/accel/bvh.cpp
    src/accel/model_accel.cpp
    src/accel/simplify.cpp
    src/anim/player.cpp
    src/anim/pose.cpp
    src/anim/skinning.cpp
    src/app/benchmark.cpp
    src/app/capabilities.cpp
    src/app/inspector.cpp
    src/app/options.cpp
    src/app/preview.cpp
    src/app/setup.cpp
    src/app/viewer.cpp
    src/core/json.cpp
    src/core/texture.cpp
    src/core/thread_pool.cpp
    src/export/gif.cpp
    src/export/offscreen.cpp
    src/export/recorder.cpp
    src/input/events.cpp
    ${TERMESH_TERMINAL_SOURCE}
    src/output/cell_backends.cpp
    src/output/cell_grid.cpp
    src/output/pixel_backends.cpp
    src/physics/demo.cpp
    src/physics/world.cpp
    src/pipeline/clip.cpp
    src/plugin/plugin.cpp
    src/pipeline/renderer.cpp
    src/pipeline/resolve.cpp
    src/post/post.cpp
    src/scene/environment.cpp
    src/scene/gltf_loader.cpp
    src/scene/loader.cpp
    src/scene/model.cpp
    src/scene/obj_loader.cpp
    src/scene/ply_loader.cpp
    src/scene/primitives.cpp
    src/scene/stl_loader.cpp
    src/stage/camera_path.cpp
    src/stage/scene.cpp
    src/stage/scene_file.cpp
)
target_include_directories(termesh_core PUBLIC src include)
target_include_directories(termesh_core SYSTEM PUBLIC third_party)
target_link_libraries(termesh_core PUBLIC Threads::Threads PRIVATE termesh_warnings)
target_compile_definitions(termesh_core PUBLIC TERMESH_VERSION="${PROJECT_VERSION}")
# Plugins are shared libraries loaded while the program runs, which a fully
# static program cannot do.
target_link_libraries(termesh_core PUBLIC ${CMAKE_DL_LIBS})
if(TERMESH_STATIC AND NOT MSVC AND NOT APPLE)
    target_compile_definitions(termesh_core PUBLIC TERMESH_NO_PLUGINS)
endif()

# The optional Assimp backend. Everything termesh needs is in the tree; this
# trades that for several dozen more file formats.
if(TERMESH_ASSIMP)
    find_package(assimp REQUIRED)
    target_sources(termesh_core PRIVATE src/scene/assimp_loader.cpp)
    target_link_libraries(termesh_core PUBLIC assimp::assimp)
    target_compile_definitions(termesh_core PUBLIC TERMESH_HAS_ASSIMP)
endif()

if(TERMESH_NATIVE AND NOT MSVC)
    target_compile_options(termesh_core PUBLIC -march=native)
endif()

# ------------------------------------------------------------- executable ---
add_executable(termesh src/app/main.cpp)
target_link_libraries(termesh PRIVATE termesh_core termesh_third_party termesh_warnings)
if(TERMESH_STATIC AND NOT MSVC AND NOT APPLE)
    target_link_options(termesh PRIVATE -static)
endif()

# -------------------------------------------------------------------- web ---
# Two things come out of a WebAssembly build. `termesh` is the ordinary
# program, run by Node with the real file system (which is how the tests and
# the command line are exercised in CI). `termesh-web` is the viewer for a
# web page: no main(), just the entry points in src/web/web_main.cpp, with
# the example scenes packed inside. web/ holds the page that loads it.
if(EMSCRIPTEN)
    target_link_options(termesh PRIVATE -sNODERAWFS=1 -sEXIT_RUNTIME=1)

    add_executable(termesh_web src/web/web_main.cpp)
    set_target_properties(termesh_web PROPERTIES OUTPUT_NAME termesh-web)
    target_link_libraries(termesh_web PRIVATE termesh_core termesh_third_party termesh_warnings)
    target_link_options(termesh_web PRIVATE
        --no-entry
        -sMODULARIZE=1
        -sEXPORT_NAME=createTermesh
        -sENVIRONMENT=web,node
        -sEXPORTED_FUNCTIONS=_termesh_web_start,_termesh_web_frame,_termesh_web_stop
        -sEXPORTED_RUNTIME_METHODS=FS,ccall
        "SHELL:--embed-file ${CMAKE_CURRENT_SOURCE_DIR}/examples/showroom.json@/examples/showroom.json"
        "SHELL:--embed-file ${CMAKE_CURRENT_SOURCE_DIR}/examples/flythrough.json@/examples/flythrough.json")
endif()

include(GNUInstallDirs)
if(SKBUILD)
    # Building a Python wheel: the program goes where pip puts commands.
    install(TARGETS termesh RUNTIME DESTINATION ${SKBUILD_SCRIPTS_DIR})
else()
    install(TARGETS termesh RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ---------------------------------------------------------------- library ---
# libtermesh: the renderer for other programs to use, through the C API in
# include/termesh.h. Shared by default; either way it is one self-contained
# file with no dependencies beyond the C++ runtime.
if(TERMESH_BUILD_LIBRARY)
    if(TERMESH_LIBRARY_STATIC)
        set(TERMESH_LIBRARY_TYPE STATIC)
    else()
        set(TERMESH_LIBRARY_TYPE SHARED)
    endif()
    add_library(termesh_lib ${TERMESH_LIBRARY_TYPE}
        src/capi/capi.cpp
        $<TARGET_OBJECTS:termesh_core>
        $<TARGET_OBJECTS:termesh_third_party>)
    add_library(termesh::termesh ALIAS termesh_lib)
    target_include_directories(termesh_lib
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE src)
    target_include_directories(termesh_lib SYSTEM PRIVATE third_party)
    target_compile_definitions(termesh_lib PRIVATE
        TERMESH_BUILDING $<TARGET_PROPERTY:termesh_core,INTERFACE_COMPILE_DEFINITIONS>)
    target_link_libraries(termesh_lib PRIVATE Threads::Threads ${CMAKE_DL_LIBS} $<BUILD_INTERFACE:termesh_warnings>)
    if(TERMESH_ASSIMP)
        target_link_libraries(termesh_lib PRIVATE assimp::assimp)
    endif()
    if(NOT TERMESH_LIBRARY_STATIC)
        target_compile_definitions(termesh_lib PUBLIC TERMESH_SHARED)
    endif()
    # The file is libtermesh.so / libtermesh.dylib / libtermesh.a on Unix. On
    # Windows a library called "termesh" would collide with the program's
    # own files, so there it keeps the prefix: libtermesh.dll.
    set_target_properties(termesh_lib PROPERTIES
        EXPORT_NAME termesh
        VERSION ${PROJECT_VERSION}
        SOVERSION 1)
    if(WIN32)
        set_target_properties(termesh_lib PROPERTIES OUTPUT_NAME libtermesh PREFIX "")
        set(TERMESH_PC_LIBNAME libtermesh)
    else()
        set_target_properties(termesh_lib PROPERTIES OUTPUT_NAME termesh)
        set(TERMESH_PC_LIBNAME termesh)
    endif()

    # The example doubles as a check that the header really is C.
    add_executable(termesh_c_example examples/c/render.c)
    # (Compiled as C, but linked by the C++ compiler, so that whatever the
    # library was built with - a sanitizer's runtime, say - comes along.)
    set_target_properties(termesh_c_example PROPERTIES
        C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF LINKER_LANGUAGE CXX)
    target_link_libraries(termesh_c_example PRIVATE termesh_lib)
    if(NOT MSVC)
        target_compile_options(termesh_c_example PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    # Install: the library, its header, and the files that let CMake
    # (find_package(termesh)) and pkg-config (termesh.pc) find them.
    include(CMakePackageConfigHelpers)
    install(TARGETS termesh_lib EXPORT termeshTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(FILES include/termesh.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(EXPORT termeshTargets NAMESPACE termesh:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/termesh)
    configure_package_config_file(cmake/termeshConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/termeshConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/termesh)
    write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/termeshConfigVersion.cmake
        VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/termeshConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/termeshConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/termesh)
    if(TERMESH_LIBRARY_STATIC)
        set(TERMESH_PC_CFLAGS "")
        if(CMAKE_DL_LIBS)
            set(TERMESH_PC_DL "-l${CMAKE_DL_LIBS}")
        endif()
        # What a C program has to add when linking the static library: the
        # C++ runtime it was written against.
        if(APPLE)
            set(TERMESH_PC_LIBS_PRIVATE "-lc++ ${CMAKE_THREAD_LIBS_INIT} ${TERMESH_PC_DL}")
        else()
            set(TERMESH_PC_LIBS_PRIVATE "-lstdc++ -lm ${CMAKE_THREAD_LIBS_INIT} ${TERMESH_PC_DL}")
        endif()
    else()
        set(TERMESH_PC_CFLAGS "-DTERMESH_SHARED")
        set(TERMESH_PC_LIBS_PRIVATE "")
    endif()
    # The way from the .pc file's directory back up to the prefix ("../..").
    file(RELATIVE_PATH TERMESH_PC_PREFIX
        "${CMAKE_CURRENT_BINARY_DIR}/prefix/${CMAKE_INSTALL_LIBDIR}/pkgconfig" "${CMAKE_CURRENT_BINARY_DIR}/prefix")
    string(REGEX REPLACE "/$" "" TERMESH_PC_PREFIX "${TERMESH_PC_PREFIX}")
    configure_file(cmake/termesh.pc.in ${CMAKE_CURRENT_BINARY_DIR}/termesh.pc @ONLY)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/termesh.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
endif()

# ---------------------------------------------------------------- plugins ---
# The example plugins: each is one C file that includes termesh_plugin.h and
# links against nothing. They are built so that they can be tried
# (termesh --plugin build/plugins/xyz.so cloud.xyz) and so that the tests
# have real plugins to load.
if(TERMESH_BUILD_PLUGINS AND NOT EMSCRIPTEN)
    foreach(plugin xyz quadrant)
        add_library(termesh_plugin_${plugin} MODULE examples/plugins/${plugin}.c)
        target_include_directories(termesh_plugin_${plugin} PRIVATE include)
        set_target_properties(termesh_plugin_${plugin} PROPERTIES
            PREFIX "" OUTPUT_NAME ${plugin}
            C_STANDARD 99 C_STANDARD_REQUIRED ON C_VISIBILITY_PRESET hidden
            LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/plugins)
        foreach(config ${CMAKE_CONFIGURATION_TYPES})
            string(TOUPPER ${config} config_upper)
            set_target_properties(termesh_plugin_${plugin} PROPERTIES
                LIBRARY_OUTPUT_DIRECTORY_${config_upper} ${CMAKE_CURRENT_BINARY_DIR}/plugins)
        endforeach()
        if(NOT MSVC)
            target_compile_options(termesh_plugin_${plugin} PRIVATE -Wall -Wextra -Wpedantic)
            target_link_libraries(termesh_plugin_${plugin} PRIVATE m)
        endif()
    endforeach()
endif()
if(NOT SKBUILD AND NOT EMSCRIPTEN)
    install(FILES include/termesh_plugin.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

# ----------------------------------------------------------------- python ---
# The Python package: the C API again, wrapped by python/bindings.cpp, with
# the engine linked in so the module is one self-contained file. Normally
# built by pip (see pyproject.toml); a plain CMake build puts an importable
# package in <build>/python for the tests.
if(TERMESH_PYTHON)
    set(PYBIND11_FINDPYTHON ON)
    find_package(pybind11 CONFIG REQUIRED)
    pybind11_add_module(_termesh
        python/bindings.cpp
        src/capi/capi.cpp
        $<TARGET_OBJECTS:termesh_core>
        $<TARGET_OBJECTS:termesh_third_party>)
    target_include_directories(_termesh PRIVATE include src)
    target_include_directories(_termesh SYSTEM PRIVATE third_party)
    target_compile_definitions(_termesh PRIVATE $<TARGET_PROPERTY:termesh_core,INTERFACE_COMPILE_DEFINITIONS>)
    target_link_libraries(_termesh PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    if(TERMESH_ASSIMP)
        target_link_libraries(_termesh PRIVATE assimp::assimp)
    endif()
    if(SKBUILD)
        install(TARGETS _termesh DESTINATION termesh)
    else()
        set(TERMESH_PYTHON_DIR ${CMAKE_CURRENT_BINARY_DIR}/python/termesh)
        set_target_properties(_termesh PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${TERMESH_PYTHON_DIR})
        foreach(config ${CMAKE_CONFIGURATION_TYPES})
            string(TOUPPER ${config} config_upper)
            set_target_properties(_termesh PROPERTIES
                LIBRARY_OUTPUT_DIRECTORY_${config_upper} ${TERMESH_PYTHON_DIR}
                RUNTIME_OUTPUT_DIRECTORY_${config_upper} ${TERMESH_PYTHON_DIR})
        endforeach()
        add_custom_command(TARGET _termesh POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/python/termesh/__init__.py ${TERMESH_PYTHON_DIR}/__init__.py)
    endif()
endif()

# ------------------------------------------------------------------ tests ---
if(TERMESH_BUILD_TESTS)
    enable_testing()
    add_executable(termesh_tests
        tests/test_main.cpp
        tests/test_accel.cpp
        tests/test_anim.cpp
        tests/test_app.cpp
        tests/test_clip.cpp
        tests/test_formats.cpp
        tests/test_input.cpp
        tests/test_inspector.cpp
        tests/test_lod.cpp
        tests/test_math.cpp
        tests/test_output.cpp
        tests/test_physics.cpp
        tests/test_plugin.cpp
        tests/test_post.cpp
        tests/test_raster.cpp
        tests/test_record.cpp
        tests/test_render.cpp
        tests/test_scene.cpp
        tests/test_stage.cpp
    )
    target_link_libraries(termesh_tests PRIVATE termesh_core termesh_third_party termesh_warnings)
    # So the tests can find the example scenes.
    target_compile_definitions(termesh_tests PRIVATE TERMESH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    # doctest includes <ciso646> under Clang, which newer libstdc++ flags with
    # a #warning in C++20. It is harmless and not ours to fix.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(termesh_tests PRIVATE -Wno-\#warnings)
    endif()
    if(EMSCRIPTEN)
        target_link_options(termesh_tests PRIVATE -sNODERAWFS=1 -sEXIT_RUNTIME=1)
    endif()
    if(TARGET termesh_plugin_xyz)
        # The tests load the example plugins: tell them where those are, and
        # make sure they are built first.
        target_compile_definitions(termesh_tests PRIVATE
            TERMESH_TEST_PLUGIN_DIR="${CMAKE_CURRENT_BINARY_DIR}/plugins"
            TERMESH_TEST_PLUGIN_SUFFIX="${CMAKE_SHARED_MODULE_SUFFIX}")
        add_dependencies(termesh_tests termesh_plugin_xyz termesh_plugin_quadrant)
    endif()
    add_test(NAME termesh_tests COMMAND termesh_tests)

    # The C API is tested from outside, through the library as built.
    if(TERMESH_BUILD_LIBRARY)
        add_executable(termesh_capi_tests tests/test_main.cpp tests/test_capi.cpp)
        target_link_libraries(termesh_capi_tests PRIVATE termesh_lib termesh_warnings)
        target_include_directories(termesh_capi_tests SYSTEM PRIVATE third_party)
        target_compile_definitions(termesh_capi_tests PRIVATE TERMESH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(termesh_capi_tests PRIVATE -Wno-\#warnings)
        endif()
        add_test(NAME termesh_capi_tests COMMAND termesh_capi_tests)
        add_test(NAME termesh_c_example COMMAND termesh_c_example)
        if(WIN32 AND NOT TERMESH_LIBRARY_STATIC)
            # No rpath on Windows: put the DLL beside the programs that use it.
            set_target_properties(termesh_lib termesh_capi_tests termesh_c_example PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
        endif()
    endif()
endif()
