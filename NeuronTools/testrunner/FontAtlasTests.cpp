// Font-atlas layout tests (mirror NeuronRenderTest, darwinia-menu-ui.md §2.2).

#include "../FontAtlasLayout.h"
#include "TestRunner.h"

#include <cmath>

using namespace er::format;
using namespace ertest;

namespace
{
  // The reference EditorFont atlas.
  FontAtlasConfig editorFont() { return FontAtlasConfig{16, 14, 32, 16, 256, 224}; }

  bool nearly(float a, float b) { return std::fabs(a - b) < 1e-5f; }
} // namespace

ER_TEST(FontAtlas, RangeMetrics)
{
  auto cfg = editorFont();
  ER_CHECK_EQ(cfg.cellCount(), 224u);
  ER_CHECK_EQ(cfg.firstCodepoint, 32u);
  ER_CHECK_EQ(cfg.lastCodepoint(), 255u); // 32 + 224 - 1
  ER_CHECK(cfg.inRange(32));
  ER_CHECK(cfg.inRange(255));
  ER_CHECK(!cfg.inRange(31));
  ER_CHECK(!cfg.inRange(256));
}

ER_TEST(FontAtlas, SpaceIsCellZero)
{
  auto cfg = editorFont();
  ER_CHECK_EQ(glyphCellIndex(cfg, 32), 0u);
  auto uv = glyphUv(cfg, 32);
  ER_CHECK(nearly(uv.u0, 0.0f));
  ER_CHECK(nearly(uv.v0, 0.0f));
  ER_CHECK(nearly(uv.u1, 16.0f / 256.0f));
  ER_CHECK(nearly(uv.v1, 16.0f / 224.0f));
}

ER_TEST(FontAtlas, LetterAUv)
{
  // 'A' = cp 65 → cell index 33 → col 1, row 2 (16 cols).
  auto cfg = editorFont();
  ER_CHECK_EQ(glyphCellIndex(cfg, 65), 33u);
  auto uv = glyphUv(cfg, 65);
  ER_CHECK(nearly(uv.u0, 1.0f * 16.0f / 256.0f));
  ER_CHECK(nearly(uv.v0, 2.0f * 16.0f / 224.0f));
  ER_CHECK(nearly(uv.u1, 2.0f * 16.0f / 256.0f));
  ER_CHECK(nearly(uv.v1, 3.0f * 16.0f / 224.0f));
}

ER_TEST(FontAtlas, LastCodepoint)
{
  // cp 255 → cell 223 → col 15, row 13 (the bottom-right cell).
  auto cfg = editorFont();
  ER_CHECK_EQ(glyphCellIndex(cfg, 255), 223u);
  auto uv = glyphUv(cfg, 255);
  ER_CHECK(nearly(uv.u1, 1.0f)); // 16 * 16 / 256
  ER_CHECK(nearly(uv.v1, 1.0f)); // 14 * 16 / 224
}

ER_TEST(FontAtlas, OutOfRangeClampsToBlank)
{
  auto cfg = editorFont();
  // Out-of-range codepoints fall back to cell 0 (space) → blank glyph.
  ER_CHECK_EQ(glyphCellIndex(cfg, 0), 0u);
  ER_CHECK_EQ(glyphCellIndex(cfg, 0x3042), 0u); // a CJK codepoint outside the atlas
  auto uv = glyphUv(cfg, 0x3042);
  ER_CHECK(nearly(uv.u0, 0.0f));
  ER_CHECK(nearly(uv.v0, 0.0f));
}
