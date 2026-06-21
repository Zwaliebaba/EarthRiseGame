#include "pch.h"
#include "CppUnitTest.h"

#include <cstring>

#include "StringTable.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace er::ui;

// EarthRiseTest — pure, device-free logic for the EarthRise app shell. Mirrors
// the StringTable cases from NeuronTools/testrunner (§16.2): id lookup + the
// visible "!?!" fallback for a missing id (masterplan §22.4). StringTable.h is
// header-only, so nothing here needs to link the EarthRise executable.

namespace EarthRiseTest
{
  TEST_CLASS(StringTableTests)
  {
  public:
    TEST_METHOD(KnownIdsResolve)
    {
      Assert::IsTrue(std::strcmp(str("app.title"), "EarthRise") == 0);
      Assert::IsTrue(std::strcmp(str("ui.mainmenu.title"), "MAIN MENU") == 0);
      Assert::IsTrue(std::strcmp(str("ui.graphics.title"), "GRAPHICS OPTIONS") == 0);
      Assert::IsTrue(std::strcmp(str("ui.close"), "Close") == 0);
    }

    TEST_METHOD(MissingIdReturnsVisibleFallback)
    {
      Assert::IsTrue(std::strcmp(str("does.not.exist"), "!?!") == 0);
      Assert::IsTrue(std::strcmp(str(""), "!?!") == 0);
    }
  };
} // namespace EarthRiseTest
