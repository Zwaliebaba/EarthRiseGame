#pragma once
// UiLayout.h — pure (device-free) layout + hit-test math for the Darwinia-style
// windowed UI (docs/design/darwinia-menu-ui.md §5/§6). Both the renderer
// (App::DrawMainMenu) and the input/hit-test path consume the SAME layout, so a
// button's drawn rect and its clickable rect can never drift. No Windows/D3D
// dependency, so it's covered by the Linux test runner.

#include <cstdint>

namespace Neuron::Render::Ui
{
  struct Rect
  {
    float x{ 0 }, y{ 0 }, w{ 0 }, h{ 0 };
    constexpr bool Contains(float px, float py) const noexcept
    {
      return px >= x && px < x + w && py >= y && py < y + h;
    }
  };

  // Window chrome metrics at a given HUD scale (1.0 = 1080p reference).
  struct MenuMetrics
  {
    float titleH, pad, btnH, gap, w;
    static constexpr MenuMetrics At(float s) noexcept
    {
      return { 28.f * s, 14.f * s, 34.f * s, 9.f * s, 300.f * s };
    }
  };

  struct MainMenuLayout
  {
    Rect window;   // full window (title + body)
    Rect titleBar; // title strip
    Rect closeBox; // close square (top-right)
    static constexpr int kMaxButtons = 16;
    Rect buttons[kMaxButtons]{}; // [0..count-2] list items, [count-1] = the trailing Close
    int  count{ 0 };
  };

  // Total window height for 'listCount' list buttons + a gapped trailing Close.
  constexpr float MainMenuHeight(const MenuMetrics& m, int listCount) noexcept
  {
    return m.titleH + m.pad + listCount * (m.btnH + m.gap) + m.gap + m.btnH + m.pad;
  }

  // Build the full layout for a window whose top-left is (gx, gy).
  constexpr MainMenuLayout BuildMainMenu(float gx, float gy, float s, int listCount) noexcept
  {
    const MenuMetrics m = MenuMetrics::At(s);
    MainMenuLayout L;
    const float totalH = MainMenuHeight(m, listCount);
    L.window = { gx, gy, m.w, totalH };
    L.titleBar = { gx, gy, m.w, m.titleH };
    const float cb = 14.f * s;
    L.closeBox = { gx + m.w - cb - 7.f * s, gy + (m.titleH - cb) * 0.5f, cb, cb };

    float by = gy + m.titleH + m.pad;
    int n = 0;
    for (int i = 0; i < listCount && n < MainMenuLayout::kMaxButtons - 1; ++i)
    {
      L.buttons[n++] = { gx + m.pad, by, m.w - 2 * m.pad, m.btnH };
      by += m.btnH + m.gap;
    }
    by += m.gap; // extra gap before Close
    L.buttons[n++] = { gx + m.pad, by, m.w - 2 * m.pad, m.btnH };
    L.count = n;
    return L;
  }

  // Centred window top-left for a screen of (screenW, screenH).
  constexpr void CenterMainMenu(float screenW, float screenH, float s, int listCount,
                                float& gx, float& gy) noexcept
  {
    const MenuMetrics m = MenuMetrics::At(s);
    gx = (screenW - m.w) * 0.5f;
    gy = screenH * 0.5f - MainMenuHeight(m, listCount) * 0.5f - 40.f * s;
  }
} // namespace Neuron::Render::Ui
