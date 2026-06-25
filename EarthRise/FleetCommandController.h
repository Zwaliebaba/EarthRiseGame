#pragma once
// FleetCommandController — the client-side RTS selection + command layer (§23.1/§23.4,
// M3 area G): which of the player's own units are selected, control groups, the last
// picked target, and turning radar/viewport clicks into validated FleetCommand intents
// sent to the server (which re-checks ownership/target, §8.4). Extracted from App.cpp so
// the IFrameworkView host stays focused on lifecycle + per-frame orchestration.
//
// It owns the selection/control-group/drag state and exposes it read-only for the HUD
// (TacticalHud reads Selection()/TargetNetId()/drag rectangle). The seam back to App is a
// per-call FleetInput (pointer state, screen size, the menu hit-test result, the menu
// scale, the session + interpolated replica) so this stays decoupled from MenuUi and the
// renderer. Platform-independent (DirectXMath only); lives in the client app for now.

#include "SessionImpl.h"   // Neuron::Client::SessionImpl (PlayerNetId + SendFleetCommand)
#include "Replica.h"       // Neuron::Client::ReplicaSet
#include "FleetControl.h"  // ControlGroups / ClassifyTarget / MakeSmartCommand / SmartTarget
#include "Picking.h"       // ScreenPoint / IsDrag / PickBox / PickNearest
#include "Command.h"       // Neuron::Sim::FleetCommand / IntentType / EncodeFleetCommand

#include <DirectXMath.h>
#include <cstdint>
#include <vector>

namespace er
{

// Everything a click handler needs for one frame. App builds this from its own state.
struct FleetInput
{
    Neuron::Client::SessionImpl*      session{ nullptr };
    const Neuron::Client::ReplicaSet* replica{ nullptr };
    UINT  screenW{ 0 }, screenH{ 0 };
    float ptrX{ 0.f }, ptrY{ 0.f };
    bool  ptrPressed{ false }, ptrReleased{ false };
    float menuScale{ 1.f };       // App's MenuScale(screenH)
    bool  overMenuWindow{ false }; // m_menu.PointerOverWindow(ptrX, ptrY, screenH)
    // Right-click context menu: RMB is also camera-orbit, so a stationary right-click
    // (release with < CTX_CLICK_PX travel from the press anchor) opens the menu.
    bool  rmbReleased{ false };               // one-frame RMB-up edge
    float rmbDownX0{ 0.f }, rmbDownY0{ 0.f };  // RMB press anchor (click vs orbit-drag)
};

class FleetCommandController
{
public:
    // ── Read-only state for the HUD ───────────────────────────────────────────
    [[nodiscard]] const std::vector<uint32_t>& Selection() const noexcept { return m_selection; }
    [[nodiscard]] uint32_t TargetNetId() const noexcept { return m_targetNetId; }
    [[nodiscard]] bool  DragActive() const noexcept { return m_selDragging; }
    [[nodiscard]] float DragStartX() const noexcept { return m_selX0; }
    [[nodiscard]] float DragStartY() const noexcept { return m_selY0; }

    // ── Control groups (Ctrl+# set / # recall, §23.2) ─────────────────────────
    void SetControlGroup(int group) { m_controlGroups.Set(group, m_selection); }
    void RecallControlGroup(int group) { m_selection = m_controlGroups.Recall(group); }

    // Re-select all of the local player's own ships from the current replica (the
    // overview "all ships" smart-select, §23.1). Bases are not commandable units.
    void SelectAllOwnShips(Neuron::Client::SessionImpl* session, const Neuron::Client::ReplicaSet& rs)
    {
        m_selection.clear();
        const uint32_t self = session ? session->PlayerNetId() : 0;
        for (uint32_t i = 0; i < rs.count; ++i) {
            const auto& e = rs.entities[i];
            if (e.valid && e.ownerPlayer == self &&
                static_cast<Neuron::Sim::EntityKind>(e.entityType) == Neuron::Sim::EntityKind::Ship)
                m_selection.push_back(e.networkId);
        }
    }

    // Enqueue a ship build at the player's base (hotkey B). Independent of the selection.
    void SendBuild(Neuron::Client::SessionImpl* session)
    {
        if (!session) return;
        Neuron::Sim::FleetCommand c;
        c.intent = Neuron::Sim::IntentType::Build;
        c.units = { session->PlayerNetId() };
        session->SendFleetCommand(Neuron::Sim::EncodeFleetCommand(c));
    }

    // Stop the current selection (hotkey S).
    void SendStop(Neuron::Client::SessionImpl* session)
    {
        Neuron::Sim::FleetCommand c;
        c.intent = Neuron::Sim::IntentType::Stop;
        c.units = m_selection;
        SendFleet(session, c);
    }

    // Resolve a radar click into a smart action against the nearest contact (or empty
    // space → move). Mirrors the radar's projection so the pick lines up.
    void HandleRadarClick(const FleetInput& in, float fx, float fz)
    {
        if (!in.ptrPressed || m_selection.empty()) return;
        if (in.overMenuWindow) return; // UI window owns this click
        const float s = in.menuScale;
        const float R = 95.f * s;
        const float cxr = 20.f * s + R;
        const float cyr = static_cast<float>(in.screenH) - 20.f * s - R;
        constexpr float RANGE = 1800.f;

        const float ddx = in.ptrX - cxr, ddy = in.ptrY - cyr;
        if (ddx * ddx + ddy * ddy > R * R) return; // click outside the radar disc

        // World point under the click (radar +X right, +Z up).
        const float wx = fx + (ddx / R) * RANGE;
        const float wz = fz - (ddy / R) * RANGE;

        // Nearest contact to the click (in render space) within a small pick radius.
        const uint32_t self = in.session ? in.session->PlayerNetId() : 0;
        const auto& rs = *in.replica;
        uint32_t bestId = 0; float bestD2 = 0.f;
        Neuron::Sim::EntityKind bestKind = Neuron::Sim::EntityKind::Unknown;
        uint32_t bestOwner = 0;
        for (uint32_t i = 0; i < rs.count; ++i) {
            const auto& e = rs.entities[i];
            if (!e.valid || e.networkId == self) continue;
            const float dx = e.x - wx, dz = e.z - wz, d2 = dx * dx + dz * dz;
            if (bestId == 0 || d2 < bestD2) {
                bestId = e.networkId; bestD2 = d2;
                bestKind = static_cast<Neuron::Sim::EntityKind>(e.entityType); bestOwner = e.ownerPlayer;
            }
        }

        // Radar picks are entity-targeted: a click within ~120 m of a contact issues the
        // smart action on it. An empty-space move needs an absolute UniversePos (the radar
        // only has render-space, with no render→universe inverse here), so empty clicks are
        // ignored — precise destinations come from the starmap / overview (M3).
        using namespace Neuron::Client;
        if (bestId == 0 || bestD2 >= 120.f * 120.f) return;
        const SmartTarget tt = ClassifyTarget(bestKind, bestOwner, self);
        m_targetNetId = bestId;
        // Beacon jumps need the beacon *name* (server keys jumps by name) — the radar
        // doesn't carry it, so those go through the starmap UI. Skip beacon picks here.
        if (tt == SmartTarget::Beacon) return;
        SendFleet(in.session, MakeSmartCommand(tt, m_selection, bestId, {}, /*queue*/false));
    }

    // Left-click / drag-box selection in the 3D viewport (playable slice). Projects owned
    // units to screen with the same view-proj the scene drew, then resolves a click
    // (nearest unit) or a drag (box) via the platform-independent Picking helper. Ignores
    // clicks over the radar disc or an open UI window. The CPU projection mirrors the
    // renderer's column-major cbuffer convention (un-transposed view-proj).
    void HandleViewportSelection(const FleetInput& in, const DirectX::XMFLOAT4X4& vpf)
    {
        using namespace DirectX;
        const float s = in.menuScale;
        const float R = 95.f * s, cxr = 20.f * s + R, cyr = static_cast<float>(in.screenH) - 20.f * s - R;
        auto overRadar = [&](float x, float y) {
            const float dx = x - cxr, dy = y - cyr; return dx * dx + dy * dy <= R * R;
        };

        if (in.ptrPressed && !in.overMenuWindow && !overRadar(in.ptrX, in.ptrY))
        {
            m_selDragging = true; m_selX0 = in.ptrX; m_selY0 = in.ptrY;
        }
        if (!m_selDragging || !in.ptrReleased) return;
        m_selDragging = false;

        const uint32_t self = in.session ? in.session->PlayerNetId() : 0;
        if (self == 0) return;

        // Project owned ships + base to screen (same vpf the scene was rendered with).
        const XMMATRIX vp = XMLoadFloat4x4(&vpf);
        std::vector<Neuron::Client::ScreenPoint> pts;
        const Neuron::Client::ReplicaSet& rs = *in.replica;
        for (uint32_t i = 0; i < rs.count; ++i)
        {
            const auto& e = rs.entities[i];
            if (!e.valid || e.ownerPlayer != self) continue;
            const auto kind = static_cast<Neuron::Sim::EntityKind>(e.entityType);
            if (kind != Neuron::Sim::EntityKind::Ship && kind != Neuron::Sim::EntityKind::Base) continue;
            const XMVECTOR clip = XMVector3Transform(XMVectorSet(e.x, e.y, e.z, 1.f), vp);
            const float wclip = XMVectorGetW(clip);
            Neuron::Client::ScreenPoint sp; sp.id = e.networkId;
            if (wclip > 1e-4f)
            {
                const float ndcx = XMVectorGetX(clip) / wclip, ndcy = XMVectorGetY(clip) / wclip;
                sp.x = (ndcx * 0.5f + 0.5f) * static_cast<float>(in.screenW);
                sp.y = (1.f - (ndcy * 0.5f + 0.5f)) * static_cast<float>(in.screenH);
                sp.visible = ndcx >= -1.f && ndcx <= 1.f && ndcy >= -1.f && ndcy <= 1.f;
            }
            pts.push_back(sp);
        }

        if (Neuron::Client::IsDrag(m_selX0, m_selY0, in.ptrX, in.ptrY))
        {
            Neuron::Client::PickBox(pts, m_selX0, m_selY0, in.ptrX, in.ptrY, m_selection);
        }
        else
        {
            const uint32_t hit = Neuron::Client::PickNearest(pts, in.ptrX, in.ptrY, 28.f * s);
            m_selection.clear();
            if (hit) m_selection.push_back(hit); // empty click clears selection
        }
    }

    // ── Right-click context menu (§23.4) ──────────────────────────────────────
    // Read-only state for the HUD to draw the open menu.
    [[nodiscard]] bool  MenuOpen() const noexcept { return m_ctxOpen; }
    [[nodiscard]] float MenuX() const noexcept { return m_ctxX; }
    [[nodiscard]] float MenuY() const noexcept { return m_ctxY; }
    [[nodiscard]] const std::vector<Neuron::Client::MenuAction>& MenuActions() const noexcept { return m_ctxActions; }

    // Drive the context menu. Returns true if it consumed this frame's left-click (so
    // App skips the radar/selection handlers and clears the pointer edges). A stationary
    // right-click opens the menu on the entity under the cursor; a left-click on an open
    // menu issues the chosen row's intent to the selection (or dismisses it).
    bool HandleContextMenu(const FleetInput& in, const DirectX::XMFLOAT4X4& vpf)
    {
        const float s = in.menuScale;

        // An open menu: a left press resolves it (pick a row, else dismiss) and is consumed.
        if (m_ctxOpen && in.ptrPressed) {
            const int row = CtxRowAt(in.ptrX, in.ptrY, s);
            if (row >= 0 && row < static_cast<int>(m_ctxActions.size())) IssueMenuAction(in.session, m_ctxActions[row]);
            m_ctxOpen = false;
            return true;
        }

        // A stationary right-click (not an orbit-drag) opens the menu on the picked entity.
        if (in.rmbReleased) {
            const float dx = in.ptrX - in.rmbDownX0, dy = in.ptrY - in.rmbDownY0;
            if (dx * dx + dy * dy <= CTX_CLICK_PX * CTX_CLICK_PX && !in.overMenuWindow) {
                Neuron::Sim::EntityKind kind = Neuron::Sim::EntityKind::Unknown; uint32_t owner = 0;
                const uint32_t hit = PickAnyEntity(in, vpf, kind, owner);
                if (hit) {
                    const uint32_t self = in.session ? in.session->PlayerNetId() : 0;
                    const Neuron::Client::SmartTarget tt = Neuron::Client::ClassifyTarget(kind, owner, self);
                    auto actions = Neuron::Client::BuildContextMenu(tt, !m_selection.empty());
                    if (!actions.empty()) {
                        m_ctxActions = std::move(actions);
                        m_ctxTarget = hit; m_ctxX = in.ptrX; m_ctxY = in.ptrY; m_ctxOpen = true;
                    }
                } else {
                    m_ctxOpen = false; // right-click on empty space closes any open menu
                }
            }
        }
        return false;
    }

private:
    // Encode + send a validated FleetCommand to the server (the server re-checks
    // ownership/target; this is purely the client request path, §8.4).
    void SendFleet(Neuron::Client::SessionImpl* session, const Neuron::Sim::FleetCommand& cmd)
    {
        if (!session || m_selection.empty()) return;
        session->SendFleetCommand(Neuron::Sim::EncodeFleetCommand(cmd));
    }

    // Pick the nearest replicated entity to the cursor in SCREEN space (any owner/kind),
    // within a small radius. Mirrors HandleViewportSelection's projection convention.
    uint32_t PickAnyEntity(const FleetInput& in, const DirectX::XMFLOAT4X4& vpf,
                           Neuron::Sim::EntityKind& outKind, uint32_t& outOwner) const
    {
        using namespace DirectX;
        const float s = in.menuScale;
        const XMMATRIX vp = XMLoadFloat4x4(&vpf);
        const Neuron::Client::ReplicaSet& rs = *in.replica;
        uint32_t best = 0;
        float bestD2 = (28.f * s) * (28.f * s); // pick radius²
        for (uint32_t i = 0; i < rs.count; ++i) {
            const auto& e = rs.entities[i];
            if (!e.valid) continue;
            const XMVECTOR clip = XMVector3Transform(XMVectorSet(e.x, e.y, e.z, 1.f), vp);
            const float wclip = XMVectorGetW(clip);
            if (wclip <= 1e-4f) continue;
            const float ndcx = XMVectorGetX(clip) / wclip, ndcy = XMVectorGetY(clip) / wclip;
            if (ndcx < -1.f || ndcx > 1.f || ndcy < -1.f || ndcy > 1.f) continue;
            const float sx = (ndcx * 0.5f + 0.5f) * static_cast<float>(in.screenW);
            const float sy = (1.f - (ndcy * 0.5f + 0.5f)) * static_cast<float>(in.screenH);
            const float dx = sx - in.ptrX, dy = sy - in.ptrY, d2 = dx * dx + dy * dy;
            if (d2 < bestD2) {
                bestD2 = d2; best = e.networkId;
                outKind = static_cast<Neuron::Sim::EntityKind>(e.entityType); outOwner = e.ownerPlayer;
            }
        }
        return best;
    }

    // Issue the chosen menu action to the current selection against the menu's target.
    void IssueMenuAction(Neuron::Client::SessionImpl* session, const Neuron::Client::MenuAction& a)
    {
        // Move (needs a world point via screen→universe unproject) and Jump (needs the
        // beacon name from the starmap) aren't wired off an in-world pick yet — same caveat
        // as the radar handler — so those rows are inert for now.
        if (a.needsPoint || a.needsBeacon || m_selection.empty()) return;
        Neuron::Sim::FleetCommand c;
        c.intent      = a.intent;
        c.units       = m_selection;
        c.targetNetId = m_ctxTarget;
        if (a.intent == Neuron::Sim::IntentType::Orbit || a.intent == Neuron::Sim::IntentType::KeepRange)
            c.range = 500.f; // default stand-off (m); a fitted-range default is a balance follow-up
        m_targetNetId = m_ctxTarget;
        SendFleet(session, c);
    }

    // The menu row under (px,py), or -1 if outside. (Deferred rows stay hittable;
    // IssueMenuAction no-ops them, so a click there just closes the menu.)
    int CtxRowAt(float px, float py, float s) const
    {
        const auto m = Neuron::Client::ContextMenuMetrics(s);
        const int n = static_cast<int>(m_ctxActions.size());
        if (px < m_ctxX || px > m_ctxX + m.width || py < m_ctxY || py > m_ctxY + m.rowH * n) return -1;
        return static_cast<int>((py - m_ctxY) / m.rowH);
    }

    static constexpr float CTX_CLICK_PX = 6.0f; // RMB travel under this = a click, not an orbit

    Neuron::Client::ControlGroups m_controlGroups;
    std::vector<uint32_t>         m_selection;     // locally-selected owned net ids
    uint32_t                      m_targetNetId{ 0 }; // last picked target (overview/radar)
    bool                          m_selDragging{ false };
    float                         m_selX0{ 0.f }, m_selY0{ 0.f };

    // Right-click context menu state (presentation reads it via the accessors above).
    bool                                    m_ctxOpen{ false };
    float                                   m_ctxX{ 0.f }, m_ctxY{ 0.f };
    uint32_t                                m_ctxTarget{ 0 };
    std::vector<Neuron::Client::MenuAction> m_ctxActions;
};

} // namespace er
