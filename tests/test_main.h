#pragma once

// A deliberately tiny test harness. No dependency to install, nothing to fetch
// in CI, and few enough moving parts that you can explain all of it.
//
// Usage:
//     #include "test_main.h"
//
//     TEST(two_plus_two) {
//         EXPECT_EQ(2 + 2, 4);
//     }
//
// Link the test file together with test_main.cpp, which supplies main().
// Tests register themselves at static-initialisation time, so there is no list
// to keep up to date.
//
// A test fails by recording failures and continuing — it does not abort. That
// way one broken case reports every one of its wrong values in a single run
// instead of hiding the rest behind the first assert.

#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace testing {

struct TestCase {
    const char* name;
    void (*fn)();
};

// Function-local statics rather than globals: initialised on first use, so a
// TEST in any translation unit can register before main() without depending on
// the order in which those translation units' globals are initialised.
inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

inline int& failure_count() {
    static int count = 0;
    return count;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline void record_failure(const char* file, int line, const std::string& what) {
    std::printf("    %s:%d: %s\n", file, line, what.c_str());
    ++failure_count();
}

template <typename T>
std::string to_str(const T& value) {
    std::ostringstream os;
    os << value;
    return os.str();
}

inline int run_all() {
    int failed_tests = 0;
    for (const TestCase& tc : registry()) {
        const int before = failure_count();
        tc.fn();
        const bool ok = (failure_count() == before);
        if (!ok) ++failed_tests;
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", tc.name);
    }
    std::printf("\n%zu tests, %d failed, %d failed assertions\n",
                registry().size(), failed_tests, failure_count());
    return failed_tests == 0 ? 0 : 1;
}

}  // namespace testing

#define TEST(test_name)                                                        \
    static void test_name();                                                   \
    static ::testing::Registrar registrar_##test_name(#test_name, test_name);  \
    static void test_name()

#define EXPECT_TRUE(expr)                                                      \
    do {                                                                       \
        if (!(expr)) {                                                         \
            ::testing::record_failure(__FILE__, __LINE__,                      \
                                      "expected true: " #expr);                \
        }                                                                      \
    } while (false)

#define EXPECT_FALSE(expr)                                                     \
    do {                                                                       \
        if ((expr)) {                                                          \
            ::testing::record_failure(__FILE__, __LINE__,                      \
                                      "expected false: " #expr);               \
        }                                                                      \
    } while (false)

// The operands are evaluated exactly once each, into named locals. Writing
// `((a) == (b))` directly would evaluate a side-effecting argument twice, and
// re-evaluate it a third time when building the failure message.
#define EXPECT_EQ(actual, expected)                                            \
    do {                                                                       \
        const auto& a_ = (actual);                                             \
        const auto& e_ = (expected);                                           \
        if (!(a_ == e_)) {                                                      \
            ::testing::record_failure(                                          \
                __FILE__, __LINE__,                                             \
                std::string(#actual) + " == " + #expected + " failed: got "     \
                    + ::testing::to_str(a_) + ", expected "                     \
                    + ::testing::to_str(e_));                                   \
        }                                                                       \
    } while (false)

#define EXPECT_NE(actual, unexpected)                                          \
    do {                                                                       \
        const auto& a_ = (actual);                                             \
        const auto& u_ = (unexpected);                                         \
        if (a_ == u_) {                                                         \
            ::testing::record_failure(                                          \
                __FILE__, __LINE__,                                             \
                std::string(#actual) + " != " + #unexpected                     \
                    + " failed: both are " + ::testing::to_str(a_));            \
        }                                                                       \
    } while (false)

// Absolute tolerance. Fine for money and prices at these magnitudes; it would
// be the wrong choice for comparing values spanning many orders of magnitude.
#define EXPECT_NEAR(actual, expected, tolerance)                               \
    do {                                                                       \
        const double a_ = (actual);                                            \
        const double e_ = (expected);                                          \
        const double t_ = (tolerance);                                         \
        if (!(std::fabs(a_ - e_) <= t_)) {                                     \
            ::testing::record_failure(                                          \
                __FILE__, __LINE__,                                             \
                std::string(#actual) + " ~= " + #expected + " failed: got "     \
                    + ::testing::to_str(a_) + ", expected "                     \
                    + ::testing::to_str(e_) + " +/- "                           \
                    + ::testing::to_str(t_));                                   \
        }                                                                       \
    } while (false)
