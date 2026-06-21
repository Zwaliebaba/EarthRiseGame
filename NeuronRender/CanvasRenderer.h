#pragma once
// CanvasRenderer — 2D HUD / UI pass: immediate-mode batched primitives over the
// 3D scene (drawn after tone-map, alpha-blended, no depth).
//
// M2: solid quads/triangles + textured quads (window chrome strips) + real
// bitmap-font text (EditorFont atlas, §2.2 of docs/design/darwinia-menu-ui.md).
// One dynamic vertex batch is split into draw "batches" that flush on a render-
// mode / texture change; each primitive picks Solid / textured-linear (chrome) /
// textured-point (font) so a window = a few draws.
//
// Usage per frame (between BeginFrame and EndFrame):
//   canvas.Reset();
//   canvas.DrawTexturedQuad(...);            // chrome
//   canvas.DrawText(x, y, "MAIN MENU", ...); // font
//   canvas.Render(cl, width, height);

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d12.h>
#include <winrt/base.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "FontAtlasLayout.h"
#include "TextureGpu.h"

namespace Neuron::Render
{

class DeviceResources;

class CanvasRenderer
{
public:
    bool Initialize(DeviceResources* dr);
    void Uninitialize();

    void Reset();

    // Register the bitmap font atlas (texture + fixed-grid metrics). Call once
    // after loading; DrawText then renders real glyphs.
    void SetFont(TextureGpu font, er::format::FontAtlasConfig cfg);
    [[nodiscard]] bool hasFont() const noexcept { return m_font.valid(); }

    // Primitives — screen-pixel coordinates, top-left origin.
    void DrawRect(float x, float y, float w, float h, float r, float g, float b, float a = 1.0f);
    // Vertical two-colour gradient quad (top row -> bottom row), e.g. the title bar.
    void DrawVGradient(float x, float y, float w, float h,
                       float tr, float tg, float tb, float ta,
                       float br, float bg, float bb, float ba);
    void DrawTriangle(float x0, float y0, float x1, float y1, float x2, float y2,
                      float r, float g, float b, float a = 1.0f);
    // Textured quad: 'uv' is the source sub-rect in [0,1]; tint multiplies the texel.
    void DrawTexturedQuad(float x, float y, float w, float h,
                          float u0, float v0, float u1, float v1,
                          const TextureGpu& tex, float r = 1, float g = 1, float b = 1, float a = 1);
    // Monospace bitmap text. scale 1 = native cell px (16). Spaces draw nothing.
    void DrawText(float x, float y, const char* text, float r, float g, float b, float scale = 1.0f);

    // Layout helpers (monospace).
    [[nodiscard]] float TextWidth(const char* text, float scale = 1.0f) const;
    [[nodiscard]] float TextHeight(float scale = 1.0f) const;

    void Render(ID3D12GraphicsCommandList* cl, UINT screenWidth, UINT screenHeight);

    static constexpr UINT kMaxQuads = 4096;
    static constexpr UINT kMaxVerts = kMaxQuads * 6;
    static constexpr UINT kMaxTextures = 16; // SRV-heap slots (font + chrome + ...)

private:
    struct CanvasVertex { float x, y, u, v, r, g, b, a; };
    static_assert(sizeof(CanvasVertex) == 32, "CanvasVertex size");

    // Per-draw render mode (matches the CanvasPS branch + sampler selection).
    enum class Mode : uint32_t { Solid = 0, TexLinear = 1, TexPoint = 2 };
    struct Batch { Mode mode; UINT srvIndex; UINT start; UINT count; };

    UINT  EnsureSrv(const TextureGpu& tex); // lazy SRV slot for a texture
    void  Prim(Mode mode, UINT srvIndex);   // begin/extend the current batch
    void  Vtx(float x, float y, float u, float v, float r, float g, float b, float a);

    ID3D12Device*                       m_device{ nullptr };
    winrt::com_ptr<ID3D12RootSignature> m_rootSig;
    winrt::com_ptr<ID3D12PipelineState> m_pso;
    winrt::com_ptr<ID3D12Resource>      m_vtxBuf; // upload heap, persistent map
    CanvasVertex*                       m_vtxPtr{ nullptr };
    UINT                                m_vtxCount{ 0 };

    winrt::com_ptr<ID3D12DescriptorHeap> m_srvHeap; // shader-visible, kMaxTextures
    UINT m_srvSize{ 0 };
    UINT m_nextSrv{ 0 };
    std::unordered_map<ID3D12Resource*, UINT> m_srvByTex;

    TextureGpu                 m_font;
    er::format::FontAtlasConfig m_fontCfg{};
    UINT                       m_fontSrv{ 0 };

    std::vector<Batch> m_batches;
};

} // namespace Neuron::Render
