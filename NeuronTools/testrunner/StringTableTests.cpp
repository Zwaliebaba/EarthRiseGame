// StringTable tests (EarthRise/StringTable.h): id lookup + missing-id fallback
// (masterplan §22.4). Pure data, so the Linux runner covers it.

#include "../../EarthRise/StringTable.h"
#include "TestRunner.h"

#include <cstring>

using namespace er::ui;
using namespace ertest;

ER_TEST(StringTable, KnownIdsResolve)
{
  ER_CHECK(std::strcmp(str("app.title"), "EarthRise") == 0);
  ER_CHECK(std::strcmp(str("ui.mainmenu.title"), "MAIN MENU") == 0);
  ER_CHECK(std::strcmp(str("ui.graphics.title"), "GRAPHICS OPTIONS") == 0);
  ER_CHECK(std::strcmp(str("ui.close"), "Close") == 0);
}

ER_TEST(StringTable, MissingIdReturnsVisibleFallback)
{
  ER_CHECK(std::strcmp(str("does.not.exist"), "!?!") == 0);
  ER_CHECK(std::strcmp(str(""), "!?!") == 0);
}
