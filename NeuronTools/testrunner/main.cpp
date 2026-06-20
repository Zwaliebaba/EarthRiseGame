// NeuronTools test runner — platform-independent parser tests for Linux CI
// (masterplan §16.2). Test cases self-register; this just runs them all.

#include "TestRunner.h"

int main()
{
  return ertest::run();
}
