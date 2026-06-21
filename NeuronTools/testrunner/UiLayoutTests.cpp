// UiLayout tests (UiLayout.h): window/button/close-box rects + hit-testing for
// the Darwinia menu. Pure layout math — the same rects feed both the renderer
// and the click hit-test, so this guards that they stay consistent.

#include "../../NeuronRender/UiLayout.h"
#include "TestRunner.h"

using namespace Neuron::Render::Ui;
using namespace ertest;

namespace
{
  bool nearly(float a, float b) { return (a > b ? a - b : b - a) < 1e-3f; }
}

ER_TEST(UiLayout, RectContains)
{
  Rect r{ 10, 20, 100, 40 };
  ER_CHECK(r.Contains(10, 20));      // top-left inclusive
  ER_CHECK(r.Contains(59, 39));
  ER_CHECK(!r.Contains(9, 20));      // left of
  ER_CHECK(!r.Contains(110, 40));    // right edge exclusive
  ER_CHECK(!r.Contains(50, 60));     // bottom edge exclusive
}

ER_TEST(UiLayout, MainMenuButtonCountIncludesClose)
{
  const auto L = BuildMainMenu(0.f, 0.f, 1.f, 6); // 6 list items + Close
  ER_CHECK_EQ(L.count, 7);
}

ER_TEST(UiLayout, ButtonsStackInsideWindowBelowTitle)
{
  const float s = 1.f;
  const auto L = BuildMainMenu(100.f, 50.f, s, 6);
  const MenuMetrics m = MenuMetrics::At(s);

  // Window top-left honoured; title bar spans the window width.
  ER_CHECK(nearly(L.window.x, 100.f) && nearly(L.window.y, 50.f));
  ER_CHECK(nearly(L.titleBar.h, m.titleH));

  // Every button sits inside the window, below the title bar, and is ordered
  // strictly top-to-bottom (no overlap).
  float prevBottom = L.titleBar.y + L.titleBar.h;
  for (int i = 0; i < L.count; ++i)
  {
    const Rect& b = L.buttons[i];
    ER_CHECK(b.y >= prevBottom - 1e-3f);
    ER_CHECK(b.x >= L.window.x && b.x + b.w <= L.window.x + L.window.w + 1e-3f);
    ER_CHECK(b.y + b.h <= L.window.y + L.window.h + 1e-3f);
    prevBottom = b.y + b.h;
  }

  // The trailing Close has an extra gap above it (separated from the list).
  const float listGap = L.buttons[1].y - (L.buttons[0].y + L.buttons[0].h);
  const float closeGap = L.buttons[6].y - (L.buttons[5].y + L.buttons[5].h);
  ER_CHECK(closeGap > listGap + 1e-3f);
}

ER_TEST(UiLayout, CloseBoxInTitleBarTopRight)
{
  const auto L = BuildMainMenu(0.f, 0.f, 1.f, 6);
  // Close box is within the title bar and toward the right edge.
  ER_CHECK(L.closeBox.y >= L.titleBar.y - 1e-3f);
  ER_CHECK(L.closeBox.y + L.closeBox.h <= L.titleBar.y + L.titleBar.h + 1e-3f);
  ER_CHECK(L.closeBox.x > L.window.x + L.window.w * 0.5f);
}

ER_TEST(UiLayout, CenterPlacesWindowOnScreen)
{
  float gx = 0, gy = 0;
  CenterMainMenu(1920.f, 1080.f, 1.f, 6, gx, gy);
  const auto L = BuildMainMenu(gx, gy, 1.f, 6);
  // Horizontally centred; fully on-screen vertically.
  ER_CHECK(nearly(L.window.x + L.window.w * 0.5f, 960.f));
  ER_CHECK(L.window.y > 0.f);
  ER_CHECK(L.window.y + L.window.h < 1080.f);
}
