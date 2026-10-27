cmake_minimum_required(VERSION 3.16)
project(ndfex_trading_system LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Language baseline
# ---------------------------------------------------------------------------
# Build with plain -std=c++17 instead of -std=gnu++17 to ensure the code 
# builds with other compilers such as clang
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Emitted for clangd / editor tooling. Harmless, and stops the "no compile
# commands" complaints in VS Code.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(_sanitizers_default ON)
else()
    set(_sanitizers_default OFF)
endif()
option(ENABLE_SANITIZERS "Build with AddressSanitizer and UndefinedBehaviorSanitizer"
       ${_sanitizers_default})
option(WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# ---------------------------------------------------------------------------
# ndfex_warnings — an interface target carrying the warning flags.
# ---------------------------------------------------------------------------
# Applied per-target rather than via CMAKE_CXX_FLAGS on purpose: a global flag
# would also be forced onto any third-party code added later, and you cannot
# fix warnings in code you do not own. Anything in this repo links this target.
add_library(ndfex_warnings INTERFACE)
target_compile_options(ndfex_warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
)
if(WARNINGS_AS_ERRORS)
    target_compile_options(ndfex_warnings INTERFACE -Werror)
endif()

# ---------------------------------------------------------------------------
# ndfex_sanitizers — interface target carrying ASan/UBSan flags.
# ---------------------------------------------------------------------------
# The flags must reach BOTH compile and link steps; sanitizers need their
# runtime linked in, and a compile-only -fsanitize gives undefined symbols at
# link time. target_link_options is what makes that happen.
#
# No ThreadSanitizer here. Threading is AUDIT.md §5 step 3, and TSan is
# mutually exclusive with ASan anyway — it gets its own build preset when the
# threading work starts.
add_library(ndfex_sanitizers INTERFACE)
if(ENABLE_SANITIZERS)
    target_compile_options(ndfex_sanitizers INTERFACE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
        -fno-sanitize-recover=undefined   # UB aborts instead of logging and continuing
        -g
    )
    target_link_options(ndfex_sanitizers INTERFACE
        -fsanitize=address,undefined
    )
endif()

# ---------------------------------------------------------------------------
# ndfex_core — the library under test
# ---------------------------------------------------------------------------
# NOTE: exposure_tracker.{h,cpp} are deliberately NOT listed. exposure_tracker.h
# includes "position_tracker.h", which does not exist in this repo (AUDIT.md S2).
# The files are parked in the tree as a design reference for the pre-trade risk
# layer in §5 step 5, not built.
add_library(ndfex_core STATIC
    src/common/config.cpp
    src/md/listener.cpp
    src/md/orderbook.cpp
    src/md/snapshot_parser.cpp
    src/md/symbol_manager.cpp
    src/oe/etf_client.cpp
    src/oe/oe_client.cpp
    src/strategy/etf_arb.cpp
)

# PUBLIC, and one entry per directory rather than just src/, because the
# existing sources include their headers flat: #include "symbol_manager.h",
# not "md/symbol_manager.h". Worth revisiting later — flat includes across
# four directories make it easy to create a cycle without noticing.
target_include_directories(ndfex_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/common
    ${CMAKE_CURRENT_SOURCE_DIR}/src/md
    ${CMAKE_CURRENT_SOURCE_DIR}/src/oe
    ${CMAKE_CURRENT_SOURCE_DIR}/src/strategy
)

target_link_libraries(ndfex_core
    PUBLIC  Threads::Threads
    PRIVATE ndfex_warnings ndfex_sanitizers
)

# Sanitizer flags must be PUBLIC in effect: a target linking an ASan-built
# library must itself be built and linked with ASan, or the runtime is missing.
# Simplest correct thing is for every consumer to link ndfex_sanitizers too,
# which the targets below do explicitly.

# ---------------------------------------------------------------------------
# bot — the executable
# ---------------------------------------------------------------------------
add_executable(bot src/main.cpp)
target_link_libraries(bot PRIVATE ndfex_core ndfex_warnings ndfex_sanitizers)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
include(CTest)   # defines BUILD_TESTING (default ON) and calls enable_testing()
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
