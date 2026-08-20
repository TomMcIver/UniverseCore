cmake_minimum_required(VERSION 3.25)
project(UniverseCore VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(UNIVERSECORE_BUILD_TESTS "Build UniverseCore tests" ON)

add_library(universe_core
    src/simulation.cpp
)

target_include_directories(universe_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(universe_core PUBLIC cxx_std_23)

if(MSVC)
    target_compile_options(universe_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(universe_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

add_executable(universe_sim src/main.cpp)
target_link_libraries(universe_sim PRIVATE universe_core)

if(UNIVERSECORE_BUILD_TESTS)
    enable_testing()
    add_executable(universe_core_tests tests/simulation_tests.cpp)
    target_link_libraries(universe_core_tests PRIVATE universe_core)
    add_test(NAME universe_core_tests COMMAND universe_core_tests)
endif()
