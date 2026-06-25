#pragma once
// TacticalHud — the in-space 2D HUD overlays: the radar disc, in-world selection
// brackets + IFF health bars, the overview/command contact list, and the connection-
// status banner (masterplan §22.2/§22.3). Extracted from App.cpp so the IFrameworkView
// host stays focused on lifecycle + per-frame orchestration.
//
// Pure rendering off the CanvasRenderer: each draw reads a per-frame TacticalHudFrame
// (the interpolated replica, selection, camera focus, session state) and the HUD owns
// no game state of its own — App still owns all of that and rebuilds the frame each
// tick — so the HUD can never desync from the sim. Header-only, matching the other
// small client components (FleetControl.h / Picking.h / RtsCamera.h).

#include "CanvasRenderer.h"   // Neuron::Render::CanvasRenderer
#include "SceneRenderer.h"    // Neuron::Render::SceneEntity
#include "Replica.h"          // Neuron::Client::ReplicaSet
#include "Session.h"          // Neuron::Client::SessionState
#include "FleetControl.h"     // BuildOverview / ClassifyTarget / ShowsHealthBar / HealthFraction / SmartTarget
#include "StringTable.h"      // er::ui::str

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace er
{

// Everything the HUD needs for one frame. App builds this from its own state each
// tick; the HUD only reads it (raw pointers are non-owning, valid for the call).
struct TacticalHudFrame
{
    UINT  screenW{ 0 }, screenH{ 0 };
    float hudScale{ 1.f };   // App's MenuScale(screenH) — includes the UI-scale multiplier
    bool  uiReady{ false };  // bitmap font + chrome loaded (else skip: no glyphs yet)

    const Neuron::Client::ReplicaSet* replica{ nullptr };   // interpolated entity mirror
    const std::vector<uint32_t>*      selection{ nullptr }; // locally-selected net ids
    uint32_t selfNetId{ 0 };
    uint32_t targetNetId{ 0 };
    const float*               focus{ nullptr };    // render-space camera focus [3]
    const DirectX::XMFLOAT4X4* viewProj{ nullptr }; // view-proj the scene drew with
    const char*                objectiveText{ nullptr };

    // Live drag-select rectangle.
    bool  selDragging{ false }, ptrDown{ false };
    float selX0{ 0.f }, selY0{ 0.f }, ptrX{ 0.f }, ptrY{ 0.f };

    // Connection-status banner.
    Neuron::Client::SessionState sessionState{ Neuron::Client::SessionState::Disconnected };
    bool serverUnreachable{ false };

    // Right-click context menu (read from FleetCommandController; drawn topmost).
    bool  menuOpen{ false };
    float menuX{ 0.f }, menuY{ 0.f };
    const std::vector<Neuron::Client::MenuAction>* menuActions{ nullptr };
};

class TacticalHud
{
public:
    explicit TacticalHud(Neuron::Render::CanvasRenderer& canvas) noexcept : m_canvas(canvas) {}

    // 2D radar disc (bottom-left): top-down blips of nearby entities relative to the
    // camera focus, with range rings (masterplan §22).
    void DrawRadar(const TacticalHudFrame& f, const Neuron::Render::SceneEntity* ents, UINT count,
                   float fx, float fy, float fz)
    {
        (void)fy;
        if (!f.uiReady) return;
        const float s = f.hudScale;
        const float R = 95.f * s;
        const float cxr = 20.f * s + R;
        const float cyr = static_cast<float>(f.screenH) - 20.f * s - R;
        constexpr float kRange = 1800.f; // world metres mapped to the disc edge

        DrawDisc(cxr, cyr, R, 44, 0.30f, 0.11f, 0.12f, 0.85f);           // dark-red disc (menu body tone)
        // Rings + cross-hairs in the window's light grey-blue border colour.
        constexpr float lr = 0.780f, lg = 0.839f, lb = 0.863f;
        DrawRing(cxr, cyr, R, 48, 2.0f * s, lr, lg, lb, 0.90f);           // outer frame (bright)
        DrawRing(cxr, cyr, R * 0.66f, 44, 1.0f * s, lr, lg, lb, 0.40f);   // mid range ring
        DrawRing(cxr, cyr, R * 0.33f, 40, 1.0f * s, lr, lg, lb, 0.32f);   // inner range ring
        m_canvas.DrawLine(cxr - R, cyr, cxr + R, cyr, 1.f * s, lr, lg, lb, 0.30f);
        m_canvas.DrawLine(cxr, cyr - R, cxr, cyr + R, 1.f * s, lr, lg, lb, 0.30f);

        for (UINT i = 0; i < count; ++i)
        {
            const auto& e = ents[i];
            const float dx = e.x - fx, dz = e.z - fz;
            const float dist = std::sqrt(dx * dx + dz * dz);
            if (dist > kRange) continue;
            // World +X → radar right, world +Z → radar up.
            const float bx = cxr + (dx / kRange) * R;
            const float by = cyr - (dz / kRange) * R;
            float r, g, b; RadarColor(e.kind, r, g, b);
            const float bs = 3.0f * s;
            m_canvas.DrawRect(bx - bs * 0.5f, by - bs * 0.5f, bs, bs, r, g, b, 1.f);
        }

        // Player marker (centre, pointing up).
        const float m = 5.f * s;
        m_canvas.DrawTriangle(cxr, cyr - m, cxr - m * 0.7f, cyr + m * 0.6f, cxr + m * 0.7f, cyr + m * 0.6f,
                              0.9f, 0.95f, 1.f, 1.f);
        const float ts = s * 0.8f;
        const char* lbl = er::ui::str("ui.radar");
        m_canvas.DrawText(cxr - m_canvas.TextWidth(lbl, ts) * 0.5f, cyr - R - 16.f * s, lbl, 0.780f, 0.839f, 0.863f, ts);
    }

    // In-world feedback (playable slice): a green bracket under each selected unit and
    // a health bar over every combat unit (own = green, hostile = red). Projects each
    // replica to screen with the same view-proj the scene drew.
    void DrawWorldOverlays(const TacticalHudFrame& f)
    {
        if (!f.uiReady || !f.replica || !f.selection || !f.viewProj) return;
        const uint32_t self = f.selfNetId;
        const float s = (f.screenH > 0 ? static_cast<float>(f.screenH) : 1080.f) / 1080.f;
        const Neuron::Client::ReplicaSet& rs = *f.replica;
        for (uint32_t i = 0; i < rs.count; ++i)
        {
            const auto& e = rs.entities[i];
            if (!e.valid) continue;
            const auto kind = static_cast<Neuron::Sim::EntityKind>(e.entityType);
            const bool isMine = (e.ownerPlayer == self && self != 0);
            const bool isNpc = (kind == Neuron::Sim::EntityKind::NpcUnit);
            const bool selected =
                std::find(f.selection->begin(), f.selection->end(), e.networkId) != f.selection->end();
            const bool bar = Neuron::Client::ShowsHealthBar(kind);
            if (!selected && !bar) continue;

            float sx = 0.f, sy = 0.f;
            if (!ProjectToScreen(*f.viewProj, e.x, e.y, e.z, f.screenW, f.screenH, sx, sy)) continue;

            if (selected) // green selection bracket
            {
                const float rr = 14.f * s, t = 2.f * s;
                m_canvas.DrawRect(sx - rr, sy - rr, 2 * rr, t, 0.4f, 0.9f, 0.5f, 0.9f);
                m_canvas.DrawRect(sx - rr, sy + rr - t, 2 * rr, t, 0.4f, 0.9f, 0.5f, 0.9f);
                m_canvas.DrawRect(sx - rr, sy - rr, t, 2 * rr, 0.4f, 0.9f, 0.5f, 0.9f);
                m_canvas.DrawRect(sx + rr - t, sy - rr, t, 2 * rr, 0.4f, 0.9f, 0.5f, 0.9f);
            }
            if (bar) // health bar, IFF-coloured
            {
                const float fr = Neuron::Client::HealthFraction(kind, e.hp);
                if (fr >= 0.f)
                {
                    const float bw = 24.f * s, bh = 3.f * s, bx = sx - bw * 0.5f, by = sy - 18.f * s;
                    m_canvas.DrawRect(bx, by, bw, bh, 0.f, 0.f, 0.f, 0.6f);
                    const float r = isMine ? 0.3f : (isNpc ? 0.95f : 0.9f);
                    const float g = isMine ? 0.85f : (isNpc ? 0.3f : 0.8f);
                    const float b = isMine ? 0.4f : 0.3f;
                    m_canvas.DrawRect(bx, by, bw * fr, bh, r, g, b, 0.9f);
                }
            }
        }
    }

    // Minimal command HUD (§22.2/§22.3; M3 area G): the overview contact list (fog-
    // filtered, IFF-coloured, nearest-first) plus the current selection + target.
    void DrawCommandHud(const TacticalHudFrame& f)
    {
        if (!f.uiReady || !f.replica || !f.selection || !f.focus) return;
        const float s = f.hudScale;

        // Objective banner (playable-slice onboarding) — amber, top-left.
        if (const char* obj = f.objectiveText; obj && obj[0])
            m_canvas.DrawText(40.f * s, 18.f * s, obj, 1.0f, 0.85f, 0.4f, s * 0.95f);

        // Live drag-select rectangle (playable slice).
        if (f.selDragging && f.ptrDown)
        {
            const float lx = f.selX0 < f.ptrX ? f.selX0 : f.ptrX;
            const float ly = f.selY0 < f.ptrY ? f.selY0 : f.ptrY;
            m_canvas.DrawRect(lx, ly, std::fabs(f.ptrX - f.selX0), std::fabs(f.ptrY - f.selY0),
                              0.4f, 0.8f, 1.0f, 0.15f);
        }
        const uint32_t self = f.selfNetId;
        const auto overview = Neuron::Client::BuildOverview(*f.replica, self,
                                                            f.focus[0], f.focus[1], f.focus[2]);
        const float ts = s * 0.8f;
        float y = 20.f * s;
        const float x = static_cast<float>(f.screenW) - 230.f * s;
        char line[96];
        std::snprintf(line, sizeof(line), "OVERVIEW  sel:%u", static_cast<unsigned>(f.selection->size()));
        m_canvas.DrawText(x, y, line, 0.78f, 0.84f, 0.86f, ts);
        y += 14.f * s;
        int shown = 0;
        for (const auto& c : overview) {
            if (shown++ >= 12) break; // cap the on-screen list
            float r = 0.8f, g = 0.84f, b = 0.86f;
            using ST = Neuron::Client::SmartTarget;
            if (c.iff == ST::Enemy) { r = 0.95f; g = 0.4f; b = 0.35f; }
            else if (c.iff == ST::Ally) { r = 0.45f; g = 0.85f; b = 0.5f; }
            else if (c.iff == ST::ResourceNode) { r = 0.85f; g = 0.8f; b = 0.45f; }
            std::snprintf(line, sizeof(line), "%c #%u  %.0fm  hp:%d",
                          (c.netId == f.targetNetId) ? '>' : ' ',
                          static_cast<unsigned>(c.netId), c.distance, c.hp);
            m_canvas.DrawText(x, y, line, r, g, b, ts);
            y += 12.f * s;
        }
    }

    // Non-modal connection-status banner: a centered line near the top that reads
    // "CONNECTING TO SERVER..." while the handshake is in flight and escalates to
    // "SERVER UNAVAILABLE - RETRYING" (amber → red) once the connect timeout passes.
    // Hidden once connected, so it never clutters normal play.
    void DrawConnectionBanner(const TacticalHudFrame& f)
    {
        if (f.sessionState == Neuron::Client::SessionState::Connected) return;

        const float hudS  = (f.screenH > 0 ? static_cast<float>(f.screenH) : 1080.f) / 1080.f;
        const float scale = 1.1f * hudS;

        const bool  unreachable = f.serverUnreachable;
        const char* msg = unreachable ? er::ui::str("app.net.unavailable")
                                      : er::ui::str("app.net.connecting");
        const float r = unreachable ? 1.00f : 0.95f;
        const float g = unreachable ? 0.35f : 0.80f;
        const float b = unreachable ? 0.30f : 0.35f;

        const float tw = m_canvas.TextWidth(msg, scale);
        const float th = m_canvas.TextHeight(scale);
        const float x  = (static_cast<float>(f.screenW) - tw) * 0.5f;
        const float y  = static_cast<float>(f.screenH) * 0.12f;

        // Subtle dark backing so the text stays legible over a bright scene.
        const float pad = 10.f * hudS;
        m_canvas.DrawRect(x - pad, y - pad * 0.5f, tw + pad * 2.f, th + pad, 0.f, 0.f, 0.f, 0.55f);
        m_canvas.DrawText(x, y, msg, r, g, b, scale);
    }

    // Right-click command menu (§23.4) — a small list of actions at the click point.
    // Geometry comes from FleetControl.h ContextMenuMetrics so it matches the
    // controller's hit-test exactly. Deferred rows (Move/Jump, not yet wired off a pick)
    // are dimmed. Drawn last so it sits over the HUD + windowed UI.
    void DrawContextMenu(const TacticalHudFrame& f)
    {
        if (!f.menuOpen || !f.menuActions || f.menuActions->empty()) return;
        const float s = f.hudScale;
        const auto m = Neuron::Client::ContextMenuMetrics(s);
        const int n = static_cast<int>(f.menuActions->size());
        m_canvas.DrawRect(f.menuX, f.menuY, m.width, m.rowH * n, 0.06f, 0.07f, 0.09f, 0.96f); // backdrop
        for (int i = 0; i < n; ++i) {
            const float ry = f.menuY + i * m.rowH;
            const auto& a = (*f.menuActions)[i];
            const bool dim   = a.needsPoint || a.needsBeacon; // not yet wired off a pick
            const bool hover = f.ptrX >= f.menuX && f.ptrX <= f.menuX + m.width && f.ptrY >= ry && f.ptrY <= ry + m.rowH;
            if (hover && !dim) m_canvas.DrawRect(f.menuX, ry, m.width, m.rowH, 0.20f, 0.24f, 0.30f, 1.f);
            const float g = dim ? 0.45f : 0.92f;
            m_canvas.DrawText(f.menuX + 8.f * s, ry + 4.f * s, a.label, g, g, g, s * 0.8f);
        }
    }

private:
    static void RadarColor(uint8_t kind, float& r, float& g, float& b) noexcept
    {
        using K = Neuron::Sim::EntityKind;
        switch (static_cast<K>(kind))
        {
            case K::Base:      r = 0.35f; g = 0.65f; b = 1.00f; break; // blue (self/allied)
            case K::Ship:      r = 1.00f; g = 0.55f; b = 0.15f; break; // orange
            case K::Station:   r = 0.40f; g = 0.85f; b = 0.90f; break; // cyan
            case K::Structure: r = 0.65f; g = 0.45f; b = 1.00f; break; // violet
            case K::Asteroid:  r = 0.55f; g = 0.50f; b = 0.42f; break; // rock
            default:           r = 0.70f; g = 0.70f; b = 0.70f; break; // grey
        }
    }

    void DrawDisc(float cx, float cy, float rad, int seg, float r, float g, float b, float a)
    {
        float px = cx + rad, py = cy;
        for (int i = 1; i <= seg; ++i)
        {
            const float ang = 6.2831853f * static_cast<float>(i) / static_cast<float>(seg);
            const float x = cx + rad * std::cos(ang), y = cy + rad * std::sin(ang);
            m_canvas.DrawTriangle(cx, cy, px, py, x, y, r, g, b, a);
            px = x; py = y;
        }
    }
    void DrawRing(float cx, float cy, float rad, int seg, float width, float r, float g, float b, float a)
    {
        float px = cx + rad, py = cy;
        for (int i = 1; i <= seg; ++i)
        {
            const float ang = 6.2831853f * static_cast<float>(i) / static_cast<float>(seg);
            const float x = cx + rad * std::cos(ang), y = cy + rad * std::sin(ang);
            m_canvas.DrawLine(px, py, x, y, width, r, g, b, a);
            px = x; py = y;
        }
    }

    // Project a render-space point to screen pixels via the supplied view-proj. Returns
    // false when behind the camera or off-screen. Mirrors the renderer's column-major
    // cbuffer convention (un-transposed view-proj).
    bool ProjectToScreen(const DirectX::XMFLOAT4X4& vpf, float x, float y, float z,
                         UINT screenW, UINT screenH, float& sx, float& sy) const
    {
        using namespace DirectX;
        const XMMATRIX vp = XMLoadFloat4x4(&vpf);
        const XMVECTOR clip = XMVector3Transform(XMVectorSet(x, y, z, 1.f), vp);
        const float wc = XMVectorGetW(clip);
        if (wc <= 1e-4f) return false;
        const float ndcx = XMVectorGetX(clip) / wc, ndcy = XMVectorGetY(clip) / wc;
        if (ndcx < -1.f || ndcx > 1.f || ndcy < -1.f || ndcy > 1.f) return false;
        sx = (ndcx * 0.5f + 0.5f) * static_cast<float>(screenW);
        sy = (1.f - (ndcy * 0.5f + 0.5f)) * static_cast<float>(screenH);
        return true;
    }

    Neuron::Render::CanvasRenderer& m_canvas;
};

} // namespace er
