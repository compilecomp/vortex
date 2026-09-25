// The Vortex test harness: zero dependencies, one header, macro-registered
// tests. Used by every test binary in tests/.
#pragma once

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace vortex::testing {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

inline int& failure_count() {
    static int count = 0;
    return count;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

inline int run_all(const char* binary_name,
                   const char* filter = nullptr) {
    std::printf("[====] %s: %zu tests\n", binary_name, registry().size());
    int failed_tests = 0;
    for (const auto& t : registry()) {
        if (filter != nullptr && std::strstr(t.name, filter) == nullptr) {
            continue;
        }
        // Print + flush BEFORE running so a crashed test names itself.
        std::printf("[ RUN] %s\n", t.name);
        std::fflush(stdout);
        const int before = failure_count();
        t.fn();
        const bool ok = failure_count() == before;
        if (!ok) ++failed_tests;
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", t.name);
    }
    std::printf("[====] %s: %zu tests, %d failed\n", binary_name,
                registry().size(), failed_tests);
    return failed_tests == 0 ? 0 : 1;
}

}  // namespace vortex::testing

#define VORTEX_TEST(name)                                                     \
    static void vortex_test_body_##name();                                    \
    static const ::vortex::testing::Registrar vortex_test_reg_##name{         \
        #name, &vortex_test_body_##name};                                     \
    static void vortex_test_body_##name()

#define VORTEX_EXPECT(cond)                                                   \
    do {                                                                      \
        if (!(cond)) {                                                        \
            ++::vortex::testing::failure_count();                             \
            std::printf("  !! %s:%d: expected %s\n", __FILE__, __LINE__,     \
                        #cond);                                               \
        }                                                                     \
    } while (0)

#define VORTEX_EXPECT_EQ(a, b)                                                \
    do {                                                                      \
        const auto va = (a);                                                  \
        const auto vb = (b);                                                  \
        if (!(va == vb)) {                                                    \
            ++::vortex::testing::failure_count();                             \
            std::printf("  !! %s:%d: expected %s == %s\n", __FILE__,          \
                        __LINE__, #a, #b);                                    \
        }                                                                     \
    } while (0)
