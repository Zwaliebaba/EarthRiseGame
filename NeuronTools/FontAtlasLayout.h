#pragma once

// Pure fixed-grid monospace font-atlas layout (no rendering dependency).
//
// Codepoint → cell → UV-rect math shared by the NeuronRender FontAtlas
// (masterplan §12.2, docs/design/neuronrender-architecture.md §9) and the
// `fontpack` tool. The reference atlas is `EditorFont` from the Darwinia
// menu/options UI (docs/design/darwinia-menu-ui.md §2.2):
//   256×224 px, 16 cols × 14 rows = 224 cells, 16×16 px cells, first cp 32.
//
// Unicode-capable by construction (any first codepoint / grid size), so it
// stays localization-ready per §22.4.

#include <cstdint>

namespace er::format
{
  struct FontAtlasConfig
  {
    uint32_t cols = 16;            // grid columns
    uint32_t rows = 14;            // grid rows
    uint32_t firstCodepoint = 32;  // codepoint of cell 0 (32 = space)
    uint32_t cellPx = 16;          // cell size in px (square; = monospace advance before scale)
    uint32_t atlasWidth = 256;     // atlas texture width in px
    uint32_t atlasHeight = 224;    // atlas texture height in px

    constexpr uint32_t cellCount() const noexcept { return cols * rows; }
    constexpr uint32_t lastCodepoint() const noexcept
    {
      return firstCodepoint + (cellCount() == 0 ? 0 : cellCount() - 1);
    }
    constexpr bool inRange(uint32_t cp) const noexcept
    {
      return cp >= firstCodepoint && cp <= lastCodepoint();
    }
  };

  // Normalized texture coordinates for one glyph cell (top-left, bottom-right).
  struct UvRect
  {
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 0.0f;
    float v1 = 0.0f;
  };

  // Map a codepoint to its cell index, clamping out-of-range codepoints to the
  // first cell (space) so missing glyphs render blank rather than garbage.
  inline constexpr uint32_t glyphCellIndex(const FontAtlasConfig& cfg, uint32_t codepoint) noexcept
  {
    if (cfg.cellCount() == 0 || !cfg.inRange(codepoint))
      return 0;
    return codepoint - cfg.firstCodepoint;
  }

  // Compute the UV rect for a codepoint within the atlas.
  inline constexpr UvRect glyphUv(const FontAtlasConfig& cfg, uint32_t codepoint) noexcept
  {
    const uint32_t index = glyphCellIndex(cfg, codepoint);
    const uint32_t col = cfg.cols == 0 ? 0 : index % cfg.cols;
    const uint32_t row = cfg.cols == 0 ? 0 : index / cfg.cols;

    const float w = cfg.atlasWidth == 0 ? 1.0f : static_cast<float>(cfg.atlasWidth);
    const float h = cfg.atlasHeight == 0 ? 1.0f : static_cast<float>(cfg.atlasHeight);
    const float cw = static_cast<float>(cfg.cellPx) / w;
    const float ch = static_cast<float>(cfg.cellPx) / h;

    UvRect uv{};
    uv.u0 = static_cast<float>(col) * cw;
    uv.v0 = static_cast<float>(row) * ch;
    uv.u1 = uv.u0 + cw;
    uv.v1 = uv.v0 + ch;
    return uv;
  }
} // namespace er::format
