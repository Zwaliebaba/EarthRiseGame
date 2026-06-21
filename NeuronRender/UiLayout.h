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

  // ── Settings panel (rows of dropdowns + optional Close/Apply footer) ───────
  struct PanelMetrics
  {
    float titleH, pad, rowH, gap, footerH;
    static constexpr PanelMetrics At(float s) noexcept
    {
      return { 28.f * s, 14.f * s, 30.f * s, 8.f * s, 34.f * s };
    }
  };

  struct PanelLayout
  {
    Rect window, titleBar, closeBox;
    static constexpr int kMaxRows = 16;
    Rect rows[kMaxRows]{}; // dropdown/label row rects (full width inside padding)
    int  rowCount{ 0 };
    bool hasFooter{ false };
    Rect footerClose{}, footerApply{};
  };

  constexpr float PanelHeight(const PanelMetrics& m, int rowCount, bool footer) noexcept
  {
    return m.titleH + m.pad + rowCount * (m.rowH + m.gap)
         + (footer ? m.footerH + m.gap : 0.f) + m.pad;
  }

  constexpr PanelLayout BuildPanel(float gx, float gy, float s, float width,
                                   int rowCount, bool footer) noexcept
  {
    const PanelMetrics m = PanelMetrics::At(s);
    PanelLayout L;
    const float h = PanelHeight(m, rowCount, footer);
    L.window = { gx, gy, width, h };
    L.titleBar = { gx, gy, width, m.titleH };
    const float cb = 14.f * s;
    L.closeBox = { gx + width - cb - 7.f * s, gy + (m.titleH - cb) * 0.5f, cb, cb };

    float ry = gy + m.titleH + m.pad;
    int n = 0;
    for (int i = 0; i < rowCount && n < PanelLayout::kMaxRows; ++i)
    {
      L.rows[n++] = { gx + m.pad, ry, width - 2 * m.pad, m.rowH };
      ry += m.rowH + m.gap;
    }
    L.rowCount = n;
    L.hasFooter = footer;
    if (footer)
    {
      const float fw = (width - 2 * m.pad - m.gap) * 0.5f;
      const float fy = gy + h - m.pad - m.footerH;
      L.footerClose = { gx + m.pad, fy, fw, m.footerH };
      L.footerApply = { gx + m.pad + fw + m.gap, fy, fw, m.footerH };
    }
    return L;
  }

  // Dropdown value box = the right portion of a row (label sits to its left).
  constexpr Rect ValueBox(const Rect& row) noexcept
  {
    const float vx = row.x + row.w * 0.45f;
    return { vx, row.y, row.x + row.w - vx, row.h };
  }

  // The down-arrow triangle's bounding square at the right of the value box.
  constexpr Rect ArrowBox(const Rect& valueBox) noexcept
  {
    const float a = valueBox.h;
    return { valueBox.x + valueBox.w - a, valueBox.y, a, a };
  }

  // One row of the open dropdown popup (drawn below the value box).
  constexpr Rect PopupRow(const Rect& valueBox, int i) noexcept
  {
    return { valueBox.x, valueBox.y + valueBox.h + i * valueBox.h, valueBox.w, valueBox.h };
  }
  constexpr Rect PopupPanel(const Rect& valueBox, int total) noexcept
  {
    return { valueBox.x, valueBox.y + valueBox.h, valueBox.w, total * valueBox.h };
  }
} // namespace Neuron::Render::Ui
