#pragma once

// Minimal dependency-free test harness for the platform-independent parser
// logic (masterplan §16.2). It mirrors the cases in the Windows MSTest projects
// (NeuronRenderTest / NeuronAudioTest) so Linux CI catches parser regressions
// without a Windows build. No third-party frameworks — same MS-only spirit as
// the rest of the tree, just buildable anywhere with a C++ compiler.

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace ertest
{
  struct TestCase
  {
    std::string suite;
    std::string name;
    std::function<void()> fn;
  };

  struct Failure
  {
    std::string message;
  };

  // Thrown by a failed check to abort the current test (caught by the runner).
  struct AssertionError
  {
    std::string message;
  };

  inline std::vector<TestCase>& registry()
  {
    static std::vector<TestCase> cases;
    return cases;
  }

  struct Registrar
  {
    Registrar(const char* suite, const char* name, std::function<void()> fn)
    {
      registry().push_back({suite, name, std::move(fn)});
    }
  };

  inline void fail(const std::string& msg) { throw AssertionError{msg}; }

  inline int run()
  {
    int passed = 0;
    std::vector<Failure> failures;
    for (const auto& tc : registry())
    {
      try
      {
        tc.fn();
        ++passed;
      }
      catch (const AssertionError& e)
      {
        failures.push_back({tc.suite + "." + tc.name + ": " + e.message});
      }
      catch (const std::exception& e)
      {
        failures.push_back({tc.suite + "." + tc.name + ": unexpected exception: " + e.what()});
      }
      catch (...)
      {
        failures.push_back({tc.suite + "." + tc.name + ": unknown exception"});
      }
    }

    std::printf("\n%d passed, %zu failed (%zu total)\n", passed, failures.size(),
                registry().size());
    for (const auto& f : failures)
      std::printf("  FAIL  %s\n", f.message.c_str());
    return failures.empty() ? 0 : 1;
  }
} // namespace ertest

#define ER_TEST_CONCAT_INNER(a, b) a##b
#define ER_TEST_CONCAT(a, b) ER_TEST_CONCAT_INNER(a, b)

#define ER_TEST(suite, name)                                                                       \
  static void ER_TEST_CONCAT(er_test_fn_, __LINE__)();                                             \
  static ::ertest::Registrar ER_TEST_CONCAT(er_test_reg_, __LINE__)(                               \
      #suite, #name, &ER_TEST_CONCAT(er_test_fn_, __LINE__));                                      \
  static void ER_TEST_CONCAT(er_test_fn_, __LINE__)()

#define ER_CHECK(cond)                                                                             \
  do                                                                                               \
  {                                                                                                \
    if (!(cond))                                                                                   \
      ::ertest::fail(std::string("ER_CHECK failed: ") + #cond + " (" + __FILE__ + ":" +            \
                     std::to_string(__LINE__) + ")");                                              \
  } while (0)

#define ER_CHECK_EQ(a, b)                                                                          \
  do                                                                                               \
  {                                                                                                \
    auto er_a = (a);                                                                               \
    auto er_b = (b);                                                                               \
    if (!(er_a == er_b))                                                                           \
      ::ertest::fail(std::string("ER_CHECK_EQ failed: ") + #a + " == " + #b + " (" + __FILE__ +    \
                     ":" + std::to_string(__LINE__) + ")");                                        \
  } while (0)
