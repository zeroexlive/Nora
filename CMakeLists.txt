cmake_minimum_required(VERSION 3.16)
project(Nora VERSION 0.2.0 DESCRIPTION "Physically based renderer with Metropolis light transport" LANGUAGES C)

option(NORA_BUILD_GUI "Build the interactive GUI (CUI window layer: X11/OpenGL on Linux, Win32, Cocoa)" ON)
option(NORA_BUILD_TESTS "Build the unit tests" ON)
option(NORA_WITH_OSL "Also build the C++ shim for the real OpenShadingLanguage (liboslcomp/liboslexec)" OFF)
option(NORA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

if(MSVC)
    set(CMAKE_C_STANDARD 11)   # MSVC has no C99 switch; /std:c11 is a superset of what Nora uses
else()
    set(CMAKE_C_STANDARD 99)
endif()
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------- CUI (UI library, also ships stb)
include(FetchContent)
set(CUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
if(NOT NORA_BUILD_GUI)
    set(CUI_WITH_APP OFF CACHE BOOL "" FORCE)   # core only: no X11 / OpenGL needed
endif()
FetchContent_Declare(cui
    GIT_REPOSITORY https://github.com/yappologistic/CUI.git
    GIT_TAG v1.4.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(cui)
set(NORA_STB_DIR ${cui_SOURCE_DIR}/third_party)

find_package(Threads REQUIRED)

function(nora_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W3 /wd4244 /wd4267 /wd4305 /wd4996 /wd4100)
        if(NORA_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
        if(NORA_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# ---------------------------------------------------------------- OSL subset compiler
add_library(nora_osl STATIC
    src/osl/osl_front.c
    src/osl/osl_compile.c
    src/osl/osl_vm.c)
target_include_directories(nora_osl PUBLIC src/osl)
nora_warnings(nora_osl)
if(NOT MSVC)
    target_link_libraries(nora_osl PUBLIC m)
endif()

# ---------------------------------------------------------------- shader graph
add_library(nora_graph STATIC src/graph/graph.c src/graph/json.c)
target_include_directories(nora_graph PUBLIC src/graph)
nora_warnings(nora_graph)

# ---------------------------------------------------------------- core renderer
add_library(nora_core STATIC
    src/core/spectrum.c
    src/core/cmf_data.c
    src/core/image.c
    src/core/scene.c
    src/core/bsdf.c
    src/core/render.c
    src/core/cornell.c
    src/core/obj_import.c
    src/core/nthread.c
    src/core/fsutil.c
    src/core/scenefile.c
    src/core/edit.c
    src/core/watch.c
    src/core/raster.c
    third_party/obj/obj.c)
target_include_directories(nora_core PUBLIC src/core third_party/obj)
target_include_directories(nora_core PRIVATE ${NORA_STB_DIR})
target_link_libraries(nora_core PUBLIC nora_osl nora_graph Threads::Threads)
nora_warnings(nora_core)
set_source_files_properties(third_party/obj/obj.c PROPERTIES COMPILE_OPTIONS "$<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-sign-compare>")

if(NORA_WITH_OSL)
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    add_library(nora_osl_real STATIC src/osl/real/osl_real.cpp)
    target_include_directories(nora_osl_real PUBLIC src/osl/real src/osl)
    find_package(OSL CONFIG QUIET)
    if(OSL_FOUND)
        message(STATUS "Nora: real OpenShadingLanguage ${OSL_VERSION} found, building the liboslexec backend")
        target_compile_definitions(nora_osl_real PRIVATE NORA_OSL_REAL_ENABLED=1)
        target_link_libraries(nora_osl_real PRIVATE OSL::oslcomp OSL::oslexec)
    else()
        message(STATUS "Nora: NORA_WITH_OSL=ON but OSL was not found (set OSL_DIR); building the shim in stub mode")
    endif()
    target_link_libraries(nora_core PUBLIC nora_osl_real)
    target_compile_definitions(nora_core PUBLIC NORA_WITH_OSL=1)
endif()

# ---------------------------------------------------------------- command line renderer
add_executable(nora-cli src/cli/main.c)
target_link_libraries(nora-cli PRIVATE nora_core nora_graph)
nora_warnings(nora-cli)

# ---------------------------------------------------------------- GUI
# The GUI logic is a library so it can also be exercised headless (software renderer) in tests.
add_library(nora_gui STATIC src/gui/gui.c)
target_include_directories(nora_gui PUBLIC src/gui)
target_link_libraries(nora_gui PUBLIC nora_core nora_graph cui::cui)
nora_warnings(nora_gui)

if(NORA_BUILD_GUI)
    add_executable(nora src/gui/main.c)
    target_link_libraries(nora PRIVATE nora_gui)
    cui_app_target(nora)
    nora_warnings(nora)
endif()

# ---------------------------------------------------------------- tests
if(NORA_BUILD_TESTS)
    enable_testing()
    add_executable(nora_tests tests/test_main.c tests/test_core.c tests/test_osl.c tests/test_graph.c tests/test_scene.c)
    target_link_libraries(nora_tests PRIVATE nora_core nora_graph)
    target_compile_definitions(nora_tests PRIVATE NORA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    nora_warnings(nora_tests)
    foreach(suite spectrum bsdf bvh obj osl graph render scenefile edit watch raster checkpoint)
        add_test(NAME ${suite} COMMAND nora_tests ${suite})
    endforeach()

    add_executable(nora_imgcmp tests/imgcmp.c)
    if(NOT MSVC)
        target_link_libraries(nora_imgcmp PRIVATE m)
    endif()
    nora_warnings(nora_imgcmp)
    add_test(NAME cli_checkpoint COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:nora-cli> -DCMP=$<TARGET_FILE:nora_imgcmp>
             -DDIR=${CMAKE_CURRENT_BINARY_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/checkpoint_cli.cmake)
    add_test(NAME cli_scene_file COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:nora-cli> -DCMP=$<TARGET_FILE:nora_imgcmp>
             -DDIR=${CMAKE_CURRENT_BINARY_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/scene_cli.cmake)

    add_executable(nora_gui_headless tests/test_gui_headless.c)
    target_link_libraries(nora_gui_headless PRIVATE nora_gui)
    target_compile_definitions(nora_gui_headless PRIVATE NORA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    nora_warnings(nora_gui_headless)
    add_test(NAME gui_headless COMMAND nora_gui_headless ${CMAKE_CURRENT_BINARY_DIR}/gui_headless.png)

    # CLI smoke tests: a small Cornell box in both modes, MLT and PT, and an OBJ with OSL
    add_test(NAME cli_rgb_mlt COMMAND nora-cli cornell --mode rgb --spp 4 --width 64 --height 64 --bootstrap 4000 -q
             -o ${CMAKE_CURRENT_BINARY_DIR}/smoke_rgb.png -o ${CMAKE_CURRENT_BINARY_DIR}/smoke_rgb.exr)
    add_test(NAME cli_spectral_mlt COMMAND nora-cli cornell-glass --mode spectral --spp 4 --width 64 --height 64 --bootstrap 4000 -q
             -o ${CMAKE_CURRENT_BINARY_DIR}/smoke_spectral.png -o ${CMAKE_CURRENT_BINARY_DIR}/smoke_spectral.pfm)
    add_test(NAME cli_pt COMMAND nora-cli cornell --integrator pt --spp 2 --width 48 --height 48 -q -o ${CMAKE_CURRENT_BINARY_DIR}/smoke_pt.png)
    add_test(NAME cli_obj_osl COMMAND nora-cli ${CMAKE_CURRENT_SOURCE_DIR}/scenes/shaderball.obj --integrator pt --spp 2 --width 48 --height 48 -q
             --shader ${CMAKE_CURRENT_SOURCE_DIR}/examples/shaders/checker.osl --material Floor -o ${CMAKE_CURRENT_BINARY_DIR}/smoke_obj.png)
    add_test(NAME cli_graph COMMAND nora-cli cornell --integrator pt --spp 2 --width 48 --height 48 -q
             --graph ${CMAKE_CURRENT_SOURCE_DIR}/examples/graphs/copper.json --material White -o ${CMAKE_CURRENT_BINARY_DIR}/smoke_graph.png)
endif()

install(TARGETS nora-cli RUNTIME DESTINATION bin)
if(NORA_BUILD_GUI)
    install(TARGETS nora RUNTIME DESTINATION bin)
endif()
