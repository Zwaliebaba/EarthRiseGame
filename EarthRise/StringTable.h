#pragma once
// StringTable — id → display text (masterplan §22.4). English at launch; the
// indirection keeps display strings out of the widget/logic code so they can be
// localized later. A missing id returns a visible "!?!" fallback (so a typo'd
// id is obvious on screen, and unit-testable). Pure/header-only — no Windows
// deps — so it's covered by the Linux test runner.

#include <span>
#include <string_view>

namespace er::ui
{
  struct StringEntry
  {
    std::string_view id;
    const char*      text; // null-terminated (usable directly with DrawText)
  };

  inline constexpr StringEntry STRINGS[] = {
    { "app.title",          "EarthRise" },
    { "ui.mainmenu.title",  "MAIN MENU" },
    { "ui.options.title",   "OPTIONS" },
    { "ui.screen.title",    "SCREEN OPTIONS" },
    { "ui.graphics.title",  "GRAPHICS OPTIONS" },
    { "ui.other.title",     "OTHER OPTIONS" },
    { "ui.close",           "Close" },
    { "ui.apply",           "Apply" },
    { "ui.radar",           "RADAR" },
    // Connection-status banner (non-modal; complements the server-unavailable dialog).
    // ASCII only — the fixed-grid bitmap font has no ellipsis/em-dash glyphs.
    { "app.net.connecting", "CONNECTING TO SERVER..." },
    { "app.net.unavailable","SERVER UNAVAILABLE - RETRYING" },
  };

  // Look up display text by id; missing ids return a visible sentinel.
  inline constexpr const char* str(std::string_view id) noexcept
  {
    for (const auto& e : STRINGS)
      if (e.id == id) return e.text;
    return "!?!";
  }
} // namespace er::ui
