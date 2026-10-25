cmake_minimum_required(VERSION 3.24)

project(AudioDynamicsAnalyzer VERSION 0.2.0 LANGUAGES C CXX)

# Release binaries must run without the Visual C++ redistributable: link the C/C++ runtime
# statically (all targets, including JUCE and the tests)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(ADA_BUILD_PLUGIN "Build the JUCE VST3 plugin and standalone app" ON)
option(ADA_BUILD_TESTS "Build the C++ engine tests" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

function(ada_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_subdirectory(src/engine)

if(ADA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/cpp)
endif()

if(ADA_BUILD_PLUGIN)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.15
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
    add_subdirectory(src/plugin)
endif()
