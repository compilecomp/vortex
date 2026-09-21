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

# ---- core library --------------------------------------------------------------

file(GLOB_RECURSE VORTEX_SOURCES CONFIGURE_DEPENDS "src/*.cpp")
add_library(vortex_core STATIC ${VORTEX_SOURCES})
target_include_directories(vortex_core PUBLIC include)
target_compile_options(vortex_core PRIVATE ${VORTEX_WARNINGS})
find_package(Threads REQUIRED)
target_link_libraries(vortex_core PUBLIC Threads::Threads)

# ---- tools ---------------------------------------------------------------------

if(VORTEX_BUILD_TOOLS)
    add_executable(vx tools/vx/main.cpp)
    target_link_libraries(vx PRIVATE vortex_core)
    target_compile_options(vx PRIVATE ${VORTEX_WARNINGS})
endif()

# ---- tests ---------------------------------------------------------------------

if(VORTEX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

install(TARGETS vortex_core ARCHIVE DESTINATION lib)
install(DIRECTORY include/vortex DESTINATION include)
