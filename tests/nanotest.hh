/**
 * nanotest.hh - Lightweight unit testing library for nanopdf
 *
 * A minimal, header-only C++ unit testing framework with no external dependencies.
 * Provides a simple API similar to popular testing frameworks.
 *
 * Usage:
 *   #define NANOTEST_IMPLEMENTATION
 *   #include "nanotest.hh"
 *
 *   TEST_SUITE("MyComponent") {
 *       TEST_CASE("Basic test") {
 *           int x = 2 + 2;
 *           CHECK(x == 4);
 *           REQUIRE(x > 0);
 *       }
 *   }
 *
 *   int main() {
 *       return nanotest::run_all_tests();
 *   }
 */

#ifndef NANOTEST_HH_
#define NANOTEST_HH_

#include <iostream>
#include <sstream>
#include <vector>
#include <string>
#include <cstring>

namespace nanotest {

// ============================================================================
// Test Statistics
// ============================================================================

struct Stats {
    int test_cases_total = 0;
    int test_cases_passed = 0;
    int test_cases_failed = 0;
    int assertions_total = 0;
    int assertions_passed = 0;
    int assertions_failed = 0;
};

static Stats global_stats;
static bool current_test_failed = false;
static std::string current_suite_name;
static std::string current_test_name;

// ============================================================================
// Test Registry
// ============================================================================

using TestFunction = void (*)();

struct TestInfo {
    std::string suite_name;
    std::string test_name;
    TestFunction func;
    const char* file;
    int line;
};

static std::vector<TestInfo>& get_test_registry() {
    static std::vector<TestInfo> registry;
    return registry;
}

inline void register_test(const char* suite, const char* name,
                         TestFunction func, const char* file, int line) {
    TestInfo info;
    info.suite_name = suite;
    info.test_name = name;
    info.func = func;
    info.file = file;
    info.line = line;
    get_test_registry().push_back(info);
}

// ============================================================================
// Assertion Helpers
// ============================================================================

inline void report_assertion(bool passed, const char* expr, const char* file,
                            int line, bool is_require) {
    global_stats.assertions_total++;

    if (passed) {
        global_stats.assertions_passed++;
    } else {
        global_stats.assertions_failed++;
        current_test_failed = true;

        std::cerr << "\n" << file << ":" << line << ": ";
        if (is_require) {
            std::cerr << "FATAL ERROR: REQUIRE( " << expr << " ) failed\n";
        } else {
            std::cerr << "ERROR: CHECK( " << expr << " ) failed\n";
        }
    }
}

template <typename T, typename U>
inline void report_comparison(bool passed, const T& lhs, const U& rhs,
                             const char* lhs_expr, const char* rhs_expr,
                             const char* op, const char* file, int line,
                             bool is_require) {
    global_stats.assertions_total++;

    if (passed) {
        global_stats.assertions_passed++;
    } else {
        global_stats.assertions_failed++;
        current_test_failed = true;

        std::cerr << "\n" << file << ":" << line << ": ";
        if (is_require) {
            std::cerr << "FATAL ERROR: ";
        } else {
            std::cerr << "ERROR: ";
        }
        std::cerr << "CHECK( " << lhs_expr << " " << op << " " << rhs_expr
                  << " ) failed\n";
        std::cerr << "  values: " << lhs_expr << " = " << lhs
                  << ", " << rhs_expr << " = " << rhs << "\n";
    }
}

// ============================================================================
// Test Execution
// ============================================================================

inline int run_all_tests() {
    std::cout << "[nanotest] Running tests...\n";
    std::cout << "===============================================================================\n";

    auto& tests = get_test_registry();
    for (auto& test : tests) {
        global_stats.test_cases_total++;
        current_test_failed = false;
        current_suite_name = test.suite_name;
        current_test_name = test.test_name;

        std::cout << "\nTEST SUITE: " << test.suite_name << "\n";
        std::cout << "TEST CASE:  " << test.test_name << "\n";

        try {
            test.func();
            if (!current_test_failed) {
                global_stats.test_cases_passed++;
            } else {
                global_stats.test_cases_failed++;
            }
        } catch (const std::exception& e) {
            global_stats.test_cases_failed++;
            current_test_failed = true;
            std::cerr << "EXCEPTION: " << e.what() << "\n";
        } catch (...) {
            global_stats.test_cases_failed++;
            current_test_failed = true;
            std::cerr << "UNKNOWN EXCEPTION\n";
        }
    }

    // Print summary
    std::cout << "\n===============================================================================\n";
    std::cout << "[nanotest] test cases: " << global_stats.test_cases_total << " | "
              << global_stats.test_cases_passed << " passed | "
              << global_stats.test_cases_failed << " failed\n";
    std::cout << "[nanotest] assertions: " << global_stats.assertions_total << " | "
              << global_stats.assertions_passed << " passed | "
              << global_stats.assertions_failed << " failed\n";

    if (global_stats.test_cases_failed == 0) {
        std::cout << "[nanotest] Status: SUCCESS!\n";
        return 0;
    } else {
        std::cout << "[nanotest] Status: FAILURE!\n";
        return 1;
    }
}

// ============================================================================
// Helper class for test registration
// ============================================================================

struct TestRegistrar {
    TestRegistrar(const char* suite, const char* name, TestFunction func,
                 const char* file, int line) {
        register_test(suite, name, func, file, line);
    }
};

}  // namespace nanotest

// ============================================================================
// Public Macros
// ============================================================================

// Test suite definition (currently just for naming/organization)
#define TEST_SUITE(name) \
    namespace /* anonymous namespace for test suite */

// Test case definition with automatic registration
#define TEST_CASE(name) \
    static void NANOTEST_UNIQUE_NAME(test_func_)(); \
    static nanotest::TestRegistrar NANOTEST_UNIQUE_NAME(test_registrar_)( \
        "", name, &NANOTEST_UNIQUE_NAME(test_func_), __FILE__, __LINE__); \
    static void NANOTEST_UNIQUE_NAME(test_func_)()

// Alternative test case with suite name
#define TEST_CASE_IN_SUITE(suite, name) \
    static void NANOTEST_UNIQUE_NAME(test_func_)(); \
    static nanotest::TestRegistrar NANOTEST_UNIQUE_NAME(test_registrar_)( \
        suite, name, &NANOTEST_UNIQUE_NAME(test_func_), __FILE__, __LINE__); \
    static void NANOTEST_UNIQUE_NAME(test_func_)()

// Create unique names for test functions and registrars
#define NANOTEST_CONCAT_IMPL(a, b) a##b
#define NANOTEST_CONCAT(a, b) NANOTEST_CONCAT_IMPL(a, b)
#define NANOTEST_UNIQUE_NAME(prefix) NANOTEST_CONCAT(prefix, __LINE__)

// CHECK: Non-fatal assertion (continues on failure)
#define CHECK(expr) \
    do { \
        bool check_result_ = static_cast<bool>(expr); \
        nanotest::report_assertion(check_result_, #expr, __FILE__, __LINE__, false); \
    } while (false)

// REQUIRE: Fatal assertion (stops test on failure)
#define REQUIRE(expr) \
    do { \
        bool require_result_ = static_cast<bool>(expr); \
        nanotest::report_assertion(require_result_, #expr, __FILE__, __LINE__, true); \
        if (!require_result_) return; \
    } while (false)

// CHECK_FALSE: Check that expression is false
#define CHECK_FALSE(expr) CHECK(!(expr))

// REQUIRE_FALSE: Fatal check that expression is false
#define REQUIRE_FALSE(expr) REQUIRE(!(expr))

// CHECK_EQ: Check equality with detailed output
#define CHECK_EQ(lhs, rhs) \
    do { \
        auto lhs_val_ = (lhs); \
        auto rhs_val_ = (rhs); \
        bool eq_result_ = (lhs_val_ == rhs_val_); \
        nanotest::report_comparison(eq_result_, lhs_val_, rhs_val_, \
                                    #lhs, #rhs, "==", __FILE__, __LINE__, false); \
    } while (false)

// REQUIRE_EQ: Fatal equality check
#define REQUIRE_EQ(lhs, rhs) \
    do { \
        auto lhs_val_ = (lhs); \
        auto rhs_val_ = (rhs); \
        bool eq_result_ = (lhs_val_ == rhs_val_); \
        nanotest::report_comparison(eq_result_, lhs_val_, rhs_val_, \
                                    #lhs, #rhs, "==", __FILE__, __LINE__, true); \
        if (!eq_result_) return; \
    } while (false)

// CHECK_NE: Check inequality
#define CHECK_NE(lhs, rhs) \
    do { \
        auto lhs_val_ = (lhs); \
        auto rhs_val_ = (rhs); \
        bool ne_result_ = (lhs_val_ != rhs_val_); \
        nanotest::report_comparison(ne_result_, lhs_val_, rhs_val_, \
                                    #lhs, #rhs, "!=", __FILE__, __LINE__, false); \
    } while (false)

// SKIP_IF: Skip test gracefully when preconditions are not met
// Prints a skip message and returns (does not count as failure)
#define SKIP_IF(condition, message) \
    do { \
        if (condition) { \
            std::cout << "[nanotest] SKIP: " << message << "\n"; \
            nanotest::global_stats.test_cases_total--; \
            return; \
        } \
    } while (false)

// For doctest compatibility
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN  // Ignored, we use main()

#endif  // NANOTEST_HH_
