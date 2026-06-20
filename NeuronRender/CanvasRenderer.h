#pragma once
// CanvasRenderer — 2D HUD pass: immediate-mode batched quads over the 3D scene.
//
// M1b: solid-color rectangles (DrawRect) and placeholder text bars (DrawText).
// M2: bitmap monospace font atlas; UV mapping in CanvasVS/PS; real readable HUD.
//
// Usage per frame (between BeginFrame and EndFrame):
//   canvas.Reset();
//   canvas.DrawText(10, 10, "EarthRise", 0, 1, 0);
//   canvas.DrawRect(400, 300, 20, 20, 1, 0.5f, 0);
//   canvas.Render(cl, width, height);

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d12.h>
#include <winrt/base.h>

#include <cstdint>
#include <vector>

namespace Neuron::Render
{

class DeviceResources;

class CanvasRenderer
{
public:
    bool Initialize(DeviceResources* dr);
    void Uninitialize();

    void Reset();
    void DrawRect(float x, float y, float w, float h, float r, float g, float b, float a = 1.0f);
    void DrawText(float x, float y, const char* text, float r, float g, float b);

    void Render(ID3D12GraphicsCommandList* cl, UINT screenWidth, UINT screenHeight);

    static constexpr UINT kMaxQuads = 4096;

private:
    struct CanvasVertex { float x, y, r, g, b, a; };

    void AddQuad(float x, float y, float w, float h, float r, float g, float b, float a);

    ID3D12Device*                       m_device{ nullptr };
    winrt::com_ptr<ID3D12RootSignature> m_rootSig;
    winrt::com_ptr<ID3D12PipelineState> m_pso;
    winrt::com_ptr<ID3D12Resource>      m_vtxBuf; // upload heap, persistent map
    CanvasVertex*                       m_vtxPtr{ nullptr };
    UINT                                m_vtxCount{ 0 };
};

} // namespace Neuron::Render
