# The Vortex test suite: one binary, macro-registered tests.
enable_testing()

add_executable(vortex_tests
    harness/main.cpp
    test_tagged.cpp
    test_ugb.cpp
    test_t0.cpp
    test_codegen_infra.cpp
    test_ir_deopt_infra.cpp
)
target_include_directories(vortex_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/harness)
target_link_libraries(vortex_tests PRIVATE vortex_core)
target_compile_options(vortex_tests PRIVATE -Wall -Wextra -Wno-unused-label)

add_test(NAME vortex_all COMMAND vortex_tests)

# Test presets are consumed via `ctest --preset <name>` from the build dir.
