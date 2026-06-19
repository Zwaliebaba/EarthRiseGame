#pragma once
// Lightweight custom assert-based test runner — §16 of the masterplan.
//
// Usage:
//   TEST_SUITE("SuiteName") {
//       TEST_CASE("CaseName") { CHECK(expr); CHECK_EQ(a, b); }
//   }
//   int main() { return Neuron::Test::RunAll(); }

#include <cstdint>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace Neuron::Test
{

struct TestCase
{
    std::string_view      suite;
    std::string_view      name;
    std::function<void()> fn;
};

struct RunStats
{
    uint32_t total{ 0 };
    uint32_t passed{ 0 };
    uint32_t failed{ 0 };
};

// Global registry — populated by static initializers from TEST_SUITE blocks.
inline std::vector<TestCase>& Registry()
{
    static std::vector<TestCase> s;
    return s;
}

inline thread_local uint32_t t_checkFails = 0;

inline void RecordCase(std::string_view suite, std::string_view name,
                        std::function<void()> fn)
{
    Registry().push_back({ suite, name, std::move(fn) });
}

// Run all registered test cases. Returns non-zero on any failure.
inline int RunAll()
{
    RunStats stats;
    for (const auto& tc : Registry()) {
        t_checkFails = 0;
        printf("[RUN ] %.*s::%.*s\n",
               (int)tc.suite.size(), tc.suite.data(),
               (int)tc.name.size(),  tc.name.data());

        bool threw = false;
        try { tc.fn(); }
        catch (const std::exception& ex) {
            printf("[EXCP] %s\n", ex.what());
            threw = true;
        } catch (...) {
            printf("[EXCP] unknown exception\n");
            threw = true;
        }

        ++stats.total;
        if (t_checkFails == 0 && !threw) {
            ++stats.passed;
            printf("[PASS]\n");
        } else {
            ++stats.failed;
            printf("[FAIL] %u check(s) failed\n", t_checkFails + (threw ? 1u : 0u));
        }
    }

    printf("\n=== Results: %u/%u passed", stats.passed, stats.total);
    if (stats.failed)
        printf(", %u FAILED", stats.failed);
    printf(" ===\n");

    return stats.failed == 0 ? 0 : 1;
}

} // namespace Neuron::Test

// ---------------------------------------------------------------------------
// Macros
// ---------------------------------------------------------------------------

// CHECK(expr) — non-fatal assertion; marks the test as failed but continues.
#define CHECK(expr) \
    do { \
        if (!(expr)) { \
            ++::Neuron::Test::t_checkFails; \
            ::printf("  CHECK failed: %s  (%s:%d)\n", #expr, __FILE__, __LINE__); \
        } \
    } while (0)

#define CHECK_EQ(a, b)  CHECK((a) == (b))
#define CHECK_NEQ(a, b) CHECK((a) != (b))
#define CHECK_LT(a, b)  CHECK((a) <  (b))
#define CHECK_LE(a, b)  CHECK((a) <= (b))
#define CHECK_GT(a, b)  CHECK((a) >  (b))
#define CHECK_GE(a, b)  CHECK((a) >= (b))

// REQUIRE(expr) — fatal: throws immediately on failure, aborting the test case.
#define REQUIRE(expr) \
    do { \
        if (!(expr)) { \
            ++::Neuron::Test::t_checkFails; \
            ::printf("  REQUIRE failed: %s  (%s:%d)\n", #expr, __FILE__, __LINE__); \
            throw ::std::logic_error("REQUIRE failed: " #expr); \
        } \
    } while (0)

// TEST_SUITE / TEST_CASE: wrap a test case registration in a static initializer.
#define TEST_SUITE(SuiteName) \
    namespace { \
    struct _Suite_##SuiteName { \
        _Suite_##SuiteName(); \
    } _suite_##SuiteName##_instance; \
    } \
    _Suite_##SuiteName::_Suite_##SuiteName()

#define TEST_CASE(CaseName) \
    ::Neuron::Test::RecordCase(#CaseName, #CaseName, [&]()
// Caller closes the lambda with });
