// ParticleRenderer.cpp — CPU-simulated additive billboard particles.

#include "pch.h"
#include "ParticleRenderer.h"
#include "PixMarkers.h"

#include "CompiledShaders/ParticleVS.h"
#include "CompiledShaders/ParticlePS.h"

#include <cmath>
#include <cstring>

namespace Neuron::Render
{
  namespace
  {
    // Tiny deterministic LCG so the field is reproducible without <random> deps.
    struct Rng
    {
      uint32_t s;
      float next() { s = s * 1664525u + 1013904223u; return (s >> 8) * (1.0f / 16777216.0f); } // [0,1)
      float range(float a, float b) { return a + (b - a) * next(); }
    };
  } // namespace

  bool ParticleRenderer::Initialize(DeviceResources* dr, DXGI_FORMAT sceneColorFormat)
  {
    m_dr = dr;
    m_device = dr->Device();

    // Root signature: b0 root constants (viewProj, 16 floats, VS) + SRV table t0
    // (PS) + static linear-clamp sampler s0.
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = 16;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srvRange;

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderRegister = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 2;
    rs.pParameters = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &samp;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    winrt::com_ptr<ID3DBlob> sigBlob, errBlob;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, sigBlob.put(), errBlob.put())))
      return false;
    if (FAILED(m_device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
                                             IID_PPV_ARGS(m_rootSig.put()))))
      return false;

    static constexpr D3D12_INPUT_ELEMENT_DESC kLayout[] = {
      { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
      { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
      { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_rootSig.get();
    pso.VS = { g_pParticleVS, sizeof(g_pParticleVS) };
    pso.PS = { g_pParticlePS, sizeof(g_pParticlePS) };
    pso.InputLayout = { kLayout, static_cast<UINT>(std::size(kLayout)) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = sceneColorFormat;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleMask = 0xFFFFFFFFu;
    pso.SampleDesc.Count = 1;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

    // Additive blend: src.a * src + dst (transparent adds nothing, cores glow).
    auto& rt = pso.BlendState.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D12_BLEND_ONE;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlendAlpha = D3D12_BLEND_ONE;
    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // Depth-test against the scene (occluded by geometry) but no depth write, so
    // overlapping particles all accumulate.
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

    if (FAILED(m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(m_pso.put()))))
      return false;

    // SRV heap (1 descriptor for the glow sprite).
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = 1;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(m_srvHeap.put()))))
      return false;

    // Per-frame CPU-mapped vertex buffers (6 verts/particle; ambient + emitter).
    constexpr UINT64 vbSize = static_cast<UINT64>(kMaxVerts) * sizeof(Vertex);
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = vbSize;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (UINT i = 0; i < DeviceResources::kFrameCount; ++i)
    {
      if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(m_vb[i].put()))))
        return false;
      D3D12_RANGE rr{};
      if (FAILED(m_vb[i]->Map(0, &rr, reinterpret_cast<void**>(&m_vbPtr[i]))))
        return false;
    }

    // Seed the drifting field (offsets in a box around the focus).
    m_particles.resize(kMaxParticles);
    Rng rng{ 0x1234567u };
    for (auto& p : m_particles)
    {
      p.ox = rng.range(-kFieldHalf, kFieldHalf);
      p.oy = rng.range(-kFieldHalf, kFieldHalf);
      p.oz = rng.range(-kFieldHalf, kFieldHalf);
      p.vx = rng.range(-4.f, 4.f);
      p.vy = rng.range(-2.f, 2.f);
      p.vz = rng.range(-4.f, 4.f);
      p.size = rng.range(1.4f, 4.0f);
      const float bright = rng.range(0.25f, 0.7f);
      p.r = bright * 1.0f;            // warm motes (R>G>B), faint
      p.g = bright * 0.72f;
      p.b = bright * 0.45f;
      p.a = rng.range(0.3f, 0.7f);
    }
    m_emit.assign(kMaxEmitParticles, EmitP{}); // all dead (age==life==0 → not alive)
    return true;
  }

  void ParticleRenderer::Uninitialize()
  {
    for (UINT i = 0; i < DeviceResources::kFrameCount; ++i)
      if (m_vb[i] && m_vbPtr[i]) { m_vb[i]->Unmap(0, nullptr); m_vbPtr[i] = nullptr; }
  }

  void ParticleRenderer::SetTexture(TextureGpu tex)
  {
    if (!tex.valid() || !m_srvHeap || !m_device) return;
    m_tex = std::move(tex);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = m_tex.format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = m_tex.mipCount ? m_tex.mipCount : 1u;
    m_device->CreateShaderResourceView(m_tex.resource.get(), &srv,
                                       m_srvHeap->GetCPUDescriptorHandleForHeapStart());
  }

  void ParticleRenderer::Update(float dt, float focusX, float focusY, float focusZ)
  {
    m_fx = focusX; m_fy = focusY; m_fz = focusZ; // field follows the camera focus
    if (dt < 0.f) dt = 0.f;
    if (dt > 0.1f) dt = 0.1f; // clamp hitches
    for (auto& p : m_particles)
    {
      p.ox += p.vx * dt;
      p.oy += p.vy * dt;
      p.oz += p.vz * dt;
      // Wrap the offset into [-half, half] so the field is effectively infinite
      // and stays centred on the focus.
      auto wrap = [](float v, float half) {
        const float span = 2.f * half;
        while (v >  half) v -= span;
        while (v < -half) v += span;
        return v;
      };
      p.ox = wrap(p.ox, kFieldHalf);
      p.oy = wrap(p.oy, kFieldHalf);
      p.oz = wrap(p.oz, kFieldHalf);
    }

    // Emitter particles: age + integrate the live ones.
    for (auto& e : m_emit)
    {
      if (!e.alive()) continue;
      e.age += dt;
      e.px += e.vx * dt;
      e.py += e.vy * dt;
      e.pz += e.vz * dt;
    }

    // Spawn from each emitter (probabilistic, rate · dt per frame) into dead slots.
    Rng rng{ m_rng };
    for (const auto& em : m_emitters)
    {
      float expected = em.rate * dt;
      while (expected > 0.f)
      {
        if (expected < 1.f && rng.next() >= expected) break;
        expected -= 1.f;
        // Find the next dead slot via a rolling cursor.
        EmitP* slot = nullptr;
        for (uint32_t scan = 0; scan < kMaxEmitParticles; ++scan)
        {
          EmitP& cand = m_emit[m_spawnCursor];
          m_spawnCursor = (m_spawnCursor + 1) % kMaxEmitParticles;
          if (!cand.alive()) { slot = &cand; break; }
        }
        if (!slot) break; // pool full
        slot->px = em.x; slot->py = em.y; slot->pz = em.z;
        slot->vx = rng.range(-1.f, 1.f);
        slot->vy = rng.range(0.2f, 1.4f);   // gentle upward/outward drift
        slot->vz = rng.range(-1.f, 1.f);
        slot->age = 0.f;
        slot->life = rng.range(0.9f, 1.8f);
        slot->size = em.size * rng.range(0.6f, 1.1f);
        slot->r = em.r; slot->g = em.g; slot->b = em.b;
      }
    }
    m_rng = rng.s;
  }

  void ParticleRenderer::SetEmitters(const EmitterDesc* emitters, int count)
  {
    m_emitters.assign(emitters, emitters + (count > 0 ? count : 0));
  }

  void ParticleRenderer::Render(ID3D12GraphicsCommandList* cl, const DirectX::XMFLOAT4X4& viewProj,
                                float rightX, float rightY, float rightZ,
                                float upX, float upY, float upZ)
  {
    if (!m_tex.valid()) return;
    const UINT fi = m_dr->FrameIndex();
    if (!m_vbPtr[fi]) return;

    NEURON_PIX_SCOPED(cl, PixColors::Scene, "Particles");

    Vertex* v = m_vbPtr[fi];
    UINT n = 0;
    // Append one camera-facing additive quad (6 verts) at a world centre.
    auto billboard = [&](float cxp, float cyp, float czp, float hs,
                         float r, float g, float b, float a) {
      if (n + 6 > kMaxVerts) return;
      const float rX = rightX * hs, rY = rightY * hs, rZ = rightZ * hs;
      const float uX = upX * hs, uY = upY * hs, uZ = upZ * hs;
      const float blX = cxp - rX - uX, blY = cyp - rY - uY, blZ = czp - rZ - uZ;
      const float brX = cxp + rX - uX, brY = cyp + rY - uY, brZ = czp + rZ - uZ;
      const float trX = cxp + rX + uX, trY = cyp + rY + uY, trZ = czp + rZ + uZ;
      const float tlX = cxp - rX + uX, tlY = cyp - rY + uY, tlZ = czp - rZ + uZ;
      v[n++] = { blX, blY, blZ, 0.f, 1.f, r, g, b, a };
      v[n++] = { tlX, tlY, tlZ, 0.f, 0.f, r, g, b, a };
      v[n++] = { trX, trY, trZ, 1.f, 0.f, r, g, b, a };
      v[n++] = { blX, blY, blZ, 0.f, 1.f, r, g, b, a };
      v[n++] = { trX, trY, trZ, 1.f, 0.f, r, g, b, a };
      v[n++] = { brX, brY, brZ, 1.f, 1.f, r, g, b, a };
    };

    // Ambient dust (offsets around the camera focus); density scales the count.
    const size_t ambientN = static_cast<size_t>(m_particles.size() * m_density);
    for (size_t i = 0; i < ambientN; ++i)
    {
      const auto& p = m_particles[i];
      billboard(m_fx + p.ox, m_fy + p.oy, m_fz + p.oz, p.size, p.r, p.g, p.b, p.a);
    }

    // Emitter glow (absolute render-space; alpha fades over the particle's life).
    for (const auto& e : m_emit)
    {
      if (!e.alive()) continue;
      const float fade = 1.f - e.age / e.life;
      billboard(e.px, e.py, e.pz, e.size, e.r, e.g, e.b, fade * 0.7f);
    }

    if (n == 0) return;

    cl->SetGraphicsRootSignature(m_rootSig.get());
    cl->SetPipelineState(m_pso.get());
    cl->SetGraphicsRoot32BitConstants(0, 16, &viewProj.m[0][0], 0);
    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.get() };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetGraphicsRootDescriptorTable(1, m_srvHeap->GetGPUDescriptorHandleForHeapStart());
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_VERTEX_BUFFER_VIEW vbView{
        m_vb[fi]->GetGPUVirtualAddress(), n * static_cast<UINT>(sizeof(Vertex)),
        static_cast<UINT>(sizeof(Vertex)) };
    cl->IASetVertexBuffers(0, 1, &vbView);
    cl->DrawInstanced(n, 1, 0, 0);
  }
} // namespace Neuron::Render
