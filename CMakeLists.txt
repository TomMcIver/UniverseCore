cmake_minimum_required(VERSION 3.25)
project(UniverseCore VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(UNIVERSECORE_BUILD_TESTS "Build UniverseCore tests" ON)
option(UNIVERSECORE_BUILD_UI "Build SDL3 + Dear ImGui simulation inspector" OFF)

add_library(universe_core
    src/resources.cpp
    src/trade_merchants.cpp
    src/trade_pricing.cpp
    src/simulation.cpp
    src/snapshot.cpp
    src/demo_scenario.cpp
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

add_executable(universe_lod_benchmark src/lod_benchmark.cpp)
target_link_libraries(universe_lod_benchmark PRIVATE universe_core)

add_executable(universe_demo_seed_search src/demo_seed_search_main.cpp)
target_link_libraries(universe_demo_seed_search PRIVATE universe_core)

add_executable(universe_ui_bridge src/ui_bridge_main.cpp)
target_link_libraries(universe_ui_bridge PRIVATE universe_ui_support)

add_library(universe_ui_support
    src/ui/life_history.cpp
    src/ui/presentation_movement.cpp
    src/ui/simulation_controller.cpp
    src/ui/view_models.cpp
    src/ui/web_protocol.cpp
)

target_include_directories(universe_ui_support
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(universe_ui_support
    PUBLIC
        universe_core
)

if(UNIVERSECORE_BUILD_UI)
    include(FetchContent)

    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "" FORCE)

    FetchContent_Declare(
        SDL3
        URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.14/SDL3-3.4.14.tar.gz
        URL_HASH SHA256=30d4aa2b3037718142b32dffd4e72f917ebb6cc5227150e7bb9c45efb2153aeb
    )

    FetchContent_Declare(
        imgui
        URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.tar.gz
        URL_HASH SHA256=21d8a0a565e85dce943e375db00812c2f3f0ab21f3f0f7964e364a63422d7f99
    )

    FetchContent_MakeAvailable(SDL3 imgui)

    add_library(universe_imgui
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
    )

    target_include_directories(universe_imgui
        PUBLIC
            ${imgui_SOURCE_DIR}
            ${imgui_SOURCE_DIR}/backends
    )

    target_link_libraries(universe_imgui
        PUBLIC
            SDL3::SDL3
    )

    add_executable(universe_ui src/ui_main.cpp)
    target_link_libraries(universe_ui
        PRIVATE
            universe_ui_support
            universe_imgui
            SDL3::SDL3
    )
endif()

if(UNIVERSECORE_BUILD_TESTS)
    enable_testing()
    add_executable(universe_core_tests_fast tests/fast_tests.cpp)
    target_link_libraries(universe_core_tests_fast PRIVATE universe_core)
    add_test(NAME universe_core_tests_fast COMMAND universe_core_tests_fast)
    set_tests_properties(universe_core_tests_fast PROPERTIES LABELS "fast")

    add_executable(universe_life_events_placement_tests tests/life_events_placement_tests.cpp)
    target_link_libraries(universe_life_events_placement_tests PRIVATE universe_core)
    add_test(NAME universe_life_events_placement_tests COMMAND universe_life_events_placement_tests)
    set_tests_properties(universe_life_events_placement_tests PROPERTIES LABELS "fast")

    add_executable(universe_lod_roundtrip_tests tests/lod_roundtrip_tests.cpp)
    target_link_libraries(universe_lod_roundtrip_tests PRIVATE universe_core)
    add_test(NAME universe_lod_roundtrip_tests COMMAND universe_lod_roundtrip_tests)
    set_tests_properties(universe_lod_roundtrip_tests PROPERTIES LABELS "fast")

    add_executable(universe_settlement_structure_tests tests/settlement_structure_tests.cpp)
    target_link_libraries(universe_settlement_structure_tests PRIVATE universe_core)
    add_test(NAME universe_settlement_structure_tests COMMAND universe_settlement_structure_tests)
    set_tests_properties(universe_settlement_structure_tests PROPERTIES LABELS "fast")

    add_executable(universe_coin_tests tests/coin_tests.cpp)
    target_link_libraries(universe_coin_tests PRIVATE universe_core)
    add_test(NAME universe_coin_tests COMMAND universe_coin_tests)
    set_tests_properties(universe_coin_tests PROPERTIES LABELS "fast")

    add_executable(universe_trade_pricing_tests tests/trade_pricing_tests.cpp)
    target_link_libraries(universe_trade_pricing_tests PRIVATE universe_core)
    add_test(NAME universe_trade_pricing_tests COMMAND universe_trade_pricing_tests)
    set_tests_properties(universe_trade_pricing_tests PROPERTIES LABELS "fast")

    add_executable(universe_merchant_tests tests/merchant_tests.cpp)
    target_link_libraries(universe_merchant_tests PRIVATE universe_core)
    add_test(NAME universe_merchant_tests COMMAND universe_merchant_tests)
    set_tests_properties(universe_merchant_tests PROPERTIES LABELS "fast")

    add_executable(universe_settlement_distribution_tests tests/settlement_distribution_tests.cpp)
    target_link_libraries(universe_settlement_distribution_tests PRIVATE universe_core)
    add_test(NAME universe_settlement_distribution_tests COMMAND universe_settlement_distribution_tests)
    set_tests_properties(universe_settlement_distribution_tests PROPERTIES LABELS "integration")

    add_executable(universe_construction_tests tests/construction_tests.cpp)
    target_link_libraries(universe_construction_tests PRIVATE universe_core)
    add_test(NAME universe_construction_tests COMMAND universe_construction_tests)
    set_tests_properties(universe_construction_tests PROPERTIES LABELS "integration")

    add_executable(universe_demo_scenario_tests tests/demo_scenario_tests.cpp)
    target_link_libraries(universe_demo_scenario_tests PRIVATE universe_core)
    target_compile_definitions(universe_demo_scenario_tests PRIVATE UNIVERSE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    add_test(NAME universe_demo_scenario_tests COMMAND universe_demo_scenario_tests)
    set_tests_properties(universe_demo_scenario_tests PROPERTIES LABELS "fast")

    add_executable(universe_ui_protocol_tests tests/ui_protocol_tests.cpp)
    target_link_libraries(universe_ui_protocol_tests PRIVATE universe_ui_support)
    add_test(NAME universe_ui_protocol_tests COMMAND universe_ui_protocol_tests)
    set_tests_properties(universe_ui_protocol_tests PROPERTIES LABELS "fast")

    add_executable(universe_core_tests_integration tests/integration_tests.cpp)
    target_link_libraries(universe_core_tests_integration PRIVATE universe_core)
    add_test(NAME universe_core_tests_integration COMMAND universe_core_tests_integration)
    set_tests_properties(universe_core_tests_integration PROPERTIES LABELS "integration")

    add_executable(universe_core_tests tests/simulation_tests.cpp)
    target_link_libraries(universe_core_tests PRIVATE universe_core universe_ui_support)
    add_test(NAME universe_core_tests_long COMMAND universe_core_tests)
    set_tests_properties(universe_core_tests_long PROPERTIES LABELS "long")

    # Milestone 4 LOD scale benchmark doubles as the mixed-LOD lifecycle gate.
    add_test(NAME universe_lod_benchmark COMMAND universe_lod_benchmark)
    set_tests_properties(universe_lod_benchmark PROPERTIES LABELS "long")

    if(UNIVERSECORE_BUILD_UI)
        add_test(NAME universe_ui_smoke COMMAND universe_ui --smoke-test)
    endif()
endif()
