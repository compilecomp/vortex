cmake_minimum_required(VERSION 3.28)

project(vortex
    VERSION 0.1.0
    DESCRIPTION "The ultimate JIT: T0-J4 tiers, UGB, SoN/CIOG, RBPD, ICGGC"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)  # computed-goto dispatch in the T0 interpreter
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(VORTEX_BUILD_TESTS "Build the test suite" ON)
option(VORTEX_BUILD_TOOLS "Build the vx driver" ON)
option(VORTEX_WERROR "Treat warnings as errors" OFF)

# ---- compiler flags ----------------------------------------------------------

set(VORTEX_WARNINGS -Wall -Wextra -Wpedantic -Wno-unused-label)
if(VORTEX_WERROR)
    list(APPEND VORTEX_WARNINGS -Werror)
endif()

# Rule 68: no RTTI in hot compiler/runtime code. RTTI-free builds are the
# default for every vortex target; the frontend dispatches on explicit kind
# enums instead of dynamic_cast.
set(VORTEX_HOT_FLAGS -fno-rtti)

# ---- core library --------------------------------------------------------------

file(GLOB_RECURSE VORTEX_SOURCES CONFIGURE_DEPENDS "src/*.cpp")
add_library(vortex_core STATIC ${VORTEX_SOURCES})
target_include_directories(vortex_core PUBLIC include)
target_compile_options(vortex_core PRIVATE ${VORTEX_WARNINGS} ${VORTEX_HOT_FLAGS})
find_package(Threads REQUIRED)
target_link_libraries(vortex_core PUBLIC Threads::Threads)

# ---- tools ---------------------------------------------------------------------

if(VORTEX_BUILD_TOOLS)
    add_executable(vx tools/vx/main.cpp)
    target_link_libraries(vx PRIVATE vortex_core)
    target_compile_options(vx PRIVATE ${VORTEX_WARNINGS} ${VORTEX_HOT_FLAGS})
endif()

# ---- tests ---------------------------------------------------------------------

if(VORTEX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---- compliance lint (Rule 133: every rule has an enforcement mechanism) -------
# Hot-path structure laws, mechanically checked:
#   Rule 65 — no `throw` in the core library (frontends included: they feed it)
#   Rule 50 — no node-based containers in hot trees (vm / ir / gc / runtime)
#   Rule 48 — no raw-pointer IR edges (would reintroduce Node*)
#   Rule 68 — no RTTI constructs anywhere in src/include
add_custom_target(compliance_lint
    COMMAND ${CMAKE_SOURCE_DIR}/tools/lint/compliance.sh ${CMAKE_SOURCE_DIR}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Checking hot-path structural laws (Rules 48/50/65/68)"
    VERBATIM)

install(TARGETS vortex_core ARCHIVE DESTINATION lib)
install(DIRECTORY include/vortex DESTINATION include)
