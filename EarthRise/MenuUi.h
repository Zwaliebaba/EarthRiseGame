#pragma once
// MenuUi — the Darwinia windowed menu/options system (docs/design/darwinia-menu-ui.md):
// the Main Menu / Options button lists and the Screen / Graphics / Other settings panels
// with draggable chrome, dropdowns, z-order, and UWP-local-settings persistence.
// Extracted from App.cpp so the IFrameworkView host stays focused on lifecycle +
// per-frame orchestration.
//
// MenuUi owns all the window/dropdown/selection state and draws + handles input itself.
// The seam back to App is deliberately thin: App feeds pointer state into Update(),
// reads the derived setting getters (VSyncOn/BloomIntensity/.../FovRadians) and pushes
// them to the engine, supplies the chrome textures + a sound callback, and polls
// QuitRequested(). It holds a reference to App's CanvasRenderer (shared text/2D pass).

#include "CanvasRenderer.h"   // Neuron::Render::CanvasRenderer
#include "UiLayout.h"         // Neuron::Render::Ui::{Rect, BuildMainMenu, BuildPanel, ...}
#include "TextureGpu.h"       // Neuron::Render::TextureGpu
#include "StringTable.h"      // er::ui::str

#include <winrt/Windows.Storage.h>             // ApplicationData::Current().LocalSettings()
#include <winrt/Windows.Foundation.h>          // box_value / unbox_value

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>

namespace er
{

// Sound cues the menu emits; App maps these to its NeuronAudio clips.
enum class MenuSound { Click, Select };

class MenuUi
{
public:
    explicit MenuUi(Neuron::Render::CanvasRenderer& canvas) noexcept : m_canvas(canvas) {}

    // Wire the click/select feedback to App's audio (MenuUi has no audio dependency).
    void SetSoundCallback(std::function<void(MenuSound)> cb) { m_onSound = std::move(cb); }

    // Hand over the loaded chrome skins (InterfaceGrey/Red). App loads the DDS (it owns
    // the package-asset loader); MenuUi just holds the GPU textures it draws with.
    void SetChrome(Neuron::Render::TextureGpu grey, Neuron::Render::TextureGpu red)
    {
        m_uiGrey = std::move(grey);
        m_uiRed  = std::move(red);
    }

    // Drawable once the shared font is loaded and both chrome skins are valid.
    [[nodiscard]] bool Ready() const { return m_canvas.hasFont() && m_uiGrey.valid() && m_uiRed.valid(); }

    // True once the player picked Quit (App maps this to its run loop).
    [[nodiscard]] bool QuitRequested() const noexcept { return m_quit; }

    // ── Settings (area G) ─────────────────────────────────────────────────────
    // Sensible default selections so the wired knobs start in a good state.
    void InitSettings()
    {
        // Graphics: FOV 75, Bloom High, Particles Medium, RenderScale 100%,
        //           Shadow High, Entity High, Pixel Effect On.
        const int g[] = { 1, 3, 2, 2, 2, 2, 1 };
        for (int i = 0; i < 7; ++i) m_sel[Win_Graphics][i] = g[i];
        // Screen: 1920x1080, Borderless, VSync On, Limit Off, Aniso 8x, FXAA On, Tex High, Brightness Normal.
        const int sc[] = { 1, 1, 1, 0, 3, 1, 2, 1 };
        for (int i = 0; i < 8; ++i) m_sel[Win_Screen][i] = sc[i];
        // Other: Help On, Controller Off, Intro On, English, Normal, Large Off, Auto Camera On.
        const int ot[] = { 1, 0, 1, 0, 1, 0, 1 };
        for (int i = 0; i < 7; ++i) m_sel[Win_Other][i] = ot[i];
    }

    // Persist / restore selections in UWP local settings (best-effort).
    void SaveSettings()
    {
        try
        {
            auto vals = Windows::Storage::ApplicationData::Current().LocalSettings().Values();
            for (int win = Win_Screen; win <= Win_Other; ++win)
                for (int row = 0; row < 16; ++row)
                {
                    const winrt::hstring key{ L"set." + std::to_wstring(win) + L"." + std::to_wstring(row) };
                    vals.Insert(key, winrt::box_value(m_sel[win][row]));
                }
        }
        catch (...) {}
    }
    void LoadSettings()
    {
        try
        {
            auto vals = Windows::Storage::ApplicationData::Current().LocalSettings().Values();
            for (int win = Win_Screen; win <= Win_Other; ++win)
                for (int row = 0; row < 16; ++row)
                {
                    const winrt::hstring key{ L"set." + std::to_wstring(win) + L"." + std::to_wstring(row) };
                    if (vals.HasKey(key)) m_sel[win][row] = winrt::unbox_value<int>(vals.Lookup(key));
                }
        }
        catch (...) {}
    }

    // Derived settings — pure functions of the current selections. App reads these each
    // frame and pushes them to the live engine knobs (it owns m_dr/m_post/m_particles).
    [[nodiscard]] bool  VSyncOn()        const noexcept { return m_sel[Win_Screen][2] != 0; }
    [[nodiscard]] float BloomIntensity() const noexcept
        { static const float b[4] = { 0.f, 0.6f, 1.1f, 1.7f }; return b[Clamp(m_sel[Win_Graphics][1], 4)]; }
    [[nodiscard]] bool  PixelEffect()    const noexcept { return m_sel[Win_Graphics][6] != 0; }
    [[nodiscard]] float ParticleDensity() const noexcept
        { static const float d[4] = { 0.f, 0.33f, 0.66f, 1.0f }; return d[Clamp(m_sel[Win_Graphics][2], 4)]; }
    [[nodiscard]] float UiScaleMul()     const noexcept { return (m_sel[Win_Other][5] != 0) ? 1.3f : 1.0f; }
    [[nodiscard]] float FovRadians()     const noexcept
        { static const float deg[4] = { 60.f, 75.f, 90.f, 110.f }; return deg[Clamp(m_sel[Win_Graphics][0], 4)] * 0.01745329f; }

    // ── Per-frame interaction + draw ──────────────────────────────────────────
    // Update consumes the pointer state for this frame (App clears its own edges after).
    void Update(UINT screenW, UINT screenH, float ptrX, float ptrY,
                bool ptrDown, bool ptrPressed, bool ptrReleased)
    {
        m_ptrX = ptrX; m_ptrY = ptrY; m_ptrDown = ptrDown;
        m_ptrPressed = ptrPressed; m_ptrReleased = ptrReleased;

        m_lastW = screenW; m_lastH = screenH;
        if (!m_uiPlaced) { PlaceWindows(screenW, screenH); m_uiPlaced = true; }
        if (!Ready()) { m_ptrPressed = m_ptrReleased = false; return; }
        const float s = MenuScale(screenH);
        m_hoverWin = m_hoverItem = -1; m_ddHover = -1;

        // Drag continuation.
        if (m_dragWin >= 0)
        {
            if (m_ptrDown)
            {
                m_winX[m_dragWin] = m_ptrX - m_dragDX;
                m_winY[m_dragWin] = m_ptrY - m_dragDY;
                if (m_winX[m_dragWin] < 0) m_winX[m_dragWin] = 0;
                if (m_winY[m_dragWin] < 0) m_winY[m_dragWin] = 0;
            }
            else m_dragWin = -1;
        }

        // Open dropdown popup takes all input until closed.
        if (m_ddWin >= 0)
        {
            const UiRow* rows; WinRows(m_ddWin, rows);
            const auto L = Neuron::Render::Ui::BuildPanel(m_winX[m_ddWin], m_winY[m_ddWin], s, PanelWidth(s),
                                                          WinRows(m_ddWin, rows) + (WinLabel(m_ddWin) ? 1 : 0), true);
            const Rect vb = Neuron::Render::Ui::ValueBox(L.rows[m_ddRow]);
            const int n = rows[m_ddRow].optCount;
            for (int i = 0; i < n; ++i)
                if (Neuron::Render::Ui::PopupRow(vb, i).Contains(m_ptrX, m_ptrY)) { m_ddHover = i; break; }
            if (m_ptrPressed)
            {
                if (m_ddHover >= 0) { m_sel[m_ddWin][m_ddRow] = m_ddHover; Sound(MenuSound::Select); }
                m_ddWin = m_ddRow = -1; // any click closes the popup
            }
            m_ptrPressed = m_ptrReleased = false;
            return;
        }

        // Topmost window under the pointer consumes the interaction (front = last in
        // z-order, so iterate it backwards).
        for (int oi = Win_Count - 1; oi >= 0; --oi)
        {
            const int win = m_zorder[oi];
            if (!m_winOpen[win]) continue;

            if (WinIsPanel(win))
            {
                const UiRow* rows; const int dd = WinRows(win, rows);
                const bool hasLabel = WinLabel(win) != nullptr;
                const auto L = Neuron::Render::Ui::BuildPanel(m_winX[win], m_winY[win], s, PanelWidth(s),
                                                              dd + (hasLabel ? 1 : 0), true);
                if (!L.window.Contains(m_ptrX, m_ptrY)) continue;
                if (m_ptrPressed) RaiseWindow(win);
                for (int i = 0; i < dd; ++i)
                    if (Neuron::Render::Ui::ValueBox(L.rows[i]).Contains(m_ptrX, m_ptrY)) { m_hoverWin = win; m_hoverItem = i; break; }
                if (m_hoverItem < 0)
                {
                    if (L.footerClose.Contains(m_ptrX, m_ptrY)) { m_hoverWin = win; m_hoverItem = -2; }
                    else if (L.footerApply.Contains(m_ptrX, m_ptrY)) { m_hoverWin = win; m_hoverItem = -3; }
                }
                if (m_ptrPressed)
                {
                    if (L.closeBox.Contains(m_ptrX, m_ptrY)) { m_winOpen[win] = false; Sound(MenuSound::Click); }
                    else if (L.footerClose.Contains(m_ptrX, m_ptrY)) { m_winOpen[win] = false; Sound(MenuSound::Click); }
                    else if (L.footerApply.Contains(m_ptrX, m_ptrY)) { SaveSettings(); Sound(MenuSound::Click); }
                    else if (m_hoverItem >= 0) { m_ddWin = win; m_ddRow = m_hoverItem; Sound(MenuSound::Select); }
                    else if (L.titleBar.Contains(m_ptrX, m_ptrY)) { m_dragWin = win; m_dragDX = m_ptrX - m_winX[win]; m_dragDY = m_ptrY - m_winY[win]; }
                }
                break;
            }
            else
            {
                const char* const* items; const int n = WinButtons(win, items);
                const auto L = Neuron::Render::Ui::BuildMainMenu(m_winX[win], m_winY[win], s, n);
                if (!L.window.Contains(m_ptrX, m_ptrY)) continue;
                if (m_ptrPressed) RaiseWindow(win);
                for (int i = 0; i < L.count; ++i)
                    if (L.buttons[i].Contains(m_ptrX, m_ptrY)) { m_hoverWin = win; m_hoverItem = i; break; }
                if (m_ptrPressed)
                {
                    if (L.closeBox.Contains(m_ptrX, m_ptrY)) m_winOpen[win] = false;
                    else if (m_hoverItem >= 0) { m_pressWin = win; m_pressItem = m_hoverItem; }
                    else if (L.titleBar.Contains(m_ptrX, m_ptrY)) { m_dragWin = win; m_dragDX = m_ptrX - m_winX[win]; m_dragDY = m_ptrY - m_winY[win]; }
                }
                if (m_ptrReleased && m_pressWin == win && m_pressItem >= 0 && L.buttons[m_pressItem].Contains(m_ptrX, m_ptrY))
                    OnButtonAction(win, m_pressItem, L.count);
                break;
            }
        }

        if (m_ptrReleased) { m_pressWin = m_pressItem = -1; }
        m_ptrPressed = m_ptrReleased = false;
    }

    void Draw(UINT screenW, UINT screenH, float fps)
    {
        (void)screenW;
        if (!Ready()) return;
        m_fps = fps;
        const float s = MenuScale(screenH);
        // Draw back-to-front in z-order (front = last); popup last.
        for (int oi = 0; oi < Win_Count; ++oi)
        {
            const int win = m_zorder[oi];
            if (!m_winOpen[win]) continue;
            if (WinIsPanel(win)) DrawPanelWindow(win, s);
            else                 DrawButtonListWindow(win, s);
        }
        DrawDropdownPopup(s);
    }

    // Close an open dropdown first; else close the frontmost open window; if nothing is
    // open, bring the Main Menu back. (Bound to Esc by App.)
    void CloseTopWindow()
    {
        if (m_ddWin >= 0) { m_ddWin = m_ddRow = -1; return; }
        for (int oi = Win_Count - 1; oi >= 0; --oi)
        {
            const int w = m_zorder[oi];
            if (m_winOpen[w]) { m_winOpen[w] = false; return; }
        }
        m_winOpen[Win_MainMenu] = true;
        RaiseWindow(Win_MainMenu);
    }

    // True if the pointer is over any open windowed-UI panel (so a click there should not
    // also reach the radar/HUD beneath it). Mirrors Update's hit-test.
    [[nodiscard]] bool PointerOverWindow(float ptrX, float ptrY, UINT screenH) const
    {
        const float s = MenuScale(screenH);
        for (int win = 0; win < Win_Count; ++win) {
            if (!m_winOpen[win]) continue;
            if (WinIsPanel(win)) {
                const UiRow* rows; const int dd = WinRows(win, rows);
                const bool hasLabel = WinLabel(win) != nullptr;
                const auto L = Neuron::Render::Ui::BuildPanel(m_winX[win], m_winY[win], s, PanelWidth(s),
                                                              dd + (hasLabel ? 1 : 0), true);
                if (L.window.Contains(ptrX, ptrY)) return true;
            } else {
                const char* const* items; const int n = WinButtons(win, items);
                const auto L = Neuron::Render::Ui::BuildMainMenu(m_winX[win], m_winY[win], s, n);
                if (L.window.Contains(ptrX, ptrY)) return true;
            }
        }
        return false;
    }

private:
    enum Win { Win_MainMenu, Win_Options, Win_Screen, Win_Graphics, Win_Other, Win_Count };
    using Rect = Neuron::Render::Ui::Rect;
    struct UiRow { const char* label; const char* const* opts; int optCount; };

    float MenuScale(UINT screenH) const noexcept
    {
        return (screenH > 0 ? static_cast<float>(screenH) : 1080.f) / 1080.f * UiScaleMul();
    }
    static int Clamp(int v, int n) { return v < 0 ? 0 : (v >= n ? n - 1 : v); }
    void Sound(MenuSound s) { if (m_onSound) m_onSound(s); }

    static bool WinIsPanel(int win) { return win == Win_Screen || win == Win_Graphics || win == Win_Other; }
    static float PanelWidth(float s) { return 460.f * s; }

    static const char* WinTitle(int win)
    {
        switch (win)
        {
            case Win_MainMenu: return er::ui::str("ui.mainmenu.title");
            case Win_Options:  return er::ui::str("ui.options.title");
            case Win_Screen:   return er::ui::str("ui.screen.title");
            case Win_Graphics: return er::ui::str("ui.graphics.title");
            case Win_Other:    return er::ui::str("ui.other.title");
        }
        return "";
    }

    // Move a window to the front of the draw/pick order.
    void RaiseWindow(int win)
    {
        int idx = -1;
        for (int i = 0; i < Win_Count; ++i)
            if (m_zorder[i] == win) { idx = i; break; }
        if (idx < 0) return;
        for (int i = idx; i < Win_Count - 1; ++i) m_zorder[i] = m_zorder[i + 1];
        m_zorder[Win_Count - 1] = win;
    }

    // Button-list windows (Main Menu, Options). 'Close' is appended by the layout.
    static int WinButtons(int win, const char* const*& out)
    {
        static const char* mm[] = { "Profile", "Mods", "Options", "Visit Website",
                                    "Play Tutorial", "Quit EarthRise" };
        static const char* op[] = { "Screen Options", "Graphics Options", "Sound Options",
                                    "Control Options", "Other Options" };
        if (win == Win_Options) { out = op; return 5; }
        out = mm; return 6;
    }
    // Caption for button index i (i == listCount → the trailing Close).
    static const char* WinButtonCaption(int win, int i)
    {
        const char* const* items; const int n = WinButtons(win, items);
        return (i >= 0 && i < n) ? items[i] : "Close";
    }

    // Panel dropdown rows. Returns the dropdown-row count (excludes the label row).
    static int WinRows(int win, const UiRow*& out)
    {
        static const char* onoff[]   = { "Off", "On" };
        static const char* q3[]      = { "Low", "Medium", "High" };
        static const char* q4[]      = { "Off", "Low", "Medium", "High" };
        static const char* res[]     = { "1280x720", "1920x1080", "2560x1440", "3840x2160" };
        static const char* wmode[]   = { "Windowed", "Borderless", "Fullscreen" };
        static const char* fpscap[]  = { "Off", "30", "60", "120", "144" };
        static const char* aniso[]   = { "Off", "2x", "4x", "8x", "16x" };
        static const char* fov[]     = { "60", "75", "90", "110" };
        static const char* scale[]   = { "50%", "75%", "100%", "125%" };
        static const char* lang[]    = { "English", "Francais", "Italiano", "Russian" };
        static const char* diff[]    = { "Easy", "Normal", "Hard" };

        static const UiRow screen[] = {
            { "Resolution", res, 4 }, { "Window Mode", wmode, 3 }, { "VSync", onoff, 2 },
            { "Limit FPS", fpscap, 5 }, { "Anisotropy", aniso, 5 }, { "FXAA", onoff, 2 },
            { "Texture Detail", q3, 3 }, { "Brightness", q3, 3 },
        };
        static const UiRow graphics[] = {
            { "Field of View", fov, 4 }, { "Bloom", q4, 4 }, { "Particles", q4, 4 },
            { "Render Scale", scale, 4 }, { "Shadow Detail", q3, 3 }, { "Entity Detail", q3, 3 },
            { "Pixel Effect", onoff, 2 },
        };
        static const UiRow other[] = {
            { "Help System", onoff, 2 }, { "Controller Help", onoff, 2 }, { "Intro Movies", onoff, 2 },
            { "Language", lang, 4 }, { "Difficulty", diff, 3 }, { "Large Menus", onoff, 2 },
            { "Automatic Camera", onoff, 2 },
        };
        switch (win)
        {
            case Win_Screen:   out = screen;   return 8;
            case Win_Graphics: out = graphics; return 7;
            case Win_Other:    out = other;    return 7;
        }
        out = nullptr; return 0;
    }
    // Trailing read-only label for a panel (FPS / build version), or nullptr.
    static const char* WinLabel(int win)
    {
        if (win == Win_Graphics) return "FPS";
        if (win == Win_Other)    return "Build";
        return nullptr;
    }

    void PlaceWindows(UINT screenW, UINT screenH)
    {
        const float s = MenuScale(screenH);
        float mmX = 0, mmY = 0;
        Neuron::Render::Ui::CenterMainMenu(static_cast<float>(screenW), static_cast<float>(screenH), s, 6, mmX, mmY);
        const float pw = PanelWidth(s);
        auto clampX = [&](float x) { const float m = screenW - 320.f * s; return x < 10.f * s ? 10.f * s : (x > m ? m : x); };
        m_winX[Win_MainMenu] = mmX;                       m_winY[Win_MainMenu] = mmY;
        m_winX[Win_Options]  = clampX(mmX - 330.f * s);    m_winY[Win_Options]  = mmY;
        m_winX[Win_Graphics] = clampX(mmX + 330.f * s);    m_winY[Win_Graphics] = 70.f * s;
        m_winX[Win_Screen]   = m_winX[Win_Graphics];       m_winY[Win_Screen]   = 70.f * s;
        m_winX[Win_Other]    = m_winX[Win_Options];        m_winY[Win_Other]    = 70.f * s;
        (void)pw;
    }

    void OnButtonAction(int win, int idx, int count)
    {
        Sound(MenuSound::Click);
        if (idx == count - 1) { m_winOpen[win] = false; return; } // trailing Close
        // Open a window; if it was closed, cascade it off the current front window
        // so it never opens exactly on top of another (wraps near the screen edge).
        auto open = [&](int w) {
            if (!m_winOpen[w])
            {
                int front = -1;
                for (int oi = Win_Count - 1; oi >= 0; --oi) { const int x = m_zorder[oi]; if (m_winOpen[x]) { front = x; break; } }
                if (front >= 0)
                {
                    float nx = m_winX[front] + 36.f, ny = m_winY[front] + 36.f;
                    if (m_lastW > 140u && nx > static_cast<float>(m_lastW) - 140.f) nx = 40.f;
                    if (m_lastH > 140u && ny > static_cast<float>(m_lastH) - 140.f) ny = 40.f;
                    m_winX[w] = nx; m_winY[w] = ny;
                }
            }
            m_winOpen[w] = true;
            RaiseWindow(w);
        };
        if (win == Win_MainMenu)
        {
            if (idx == 2) open(Win_Options);              // Options
            else if (idx == 5) m_quit = true;             // Quit EarthRise
            else OutputDebugStringA("[EarthRise] menu item\n");
        }
        else if (win == Win_Options)
        {
            if (idx == 0) open(Win_Screen);
            else if (idx == 1) open(Win_Graphics);
            else if (idx == 4) open(Win_Other);
            else OutputDebugStringA("[EarthRise] options item\n");
        }
    }

    // ── Drawing ───────────────────────────────────────────────────────────────
    void DrawChromeBody(const Rect& W, const Rect& titleBar)
    {
        m_canvas.DrawTexturedQuad(W.x, W.y, W.w, W.h, 0.f, 0.f, 1.f, 1.f, m_uiRed, 1.f, 1.f, 1.f, 0.96f);
        m_canvas.DrawVGradient(titleBar.x, titleBar.y, titleBar.w, titleBar.h,
                               0.780f, 0.839f, 0.863f, 1.f, 0.439f, 0.553f, 0.659f, 1.f);
    }
    void DrawChromeFrame(const Rect& W, const Rect& titleBar, const Rect& closeBox, const char* title, float s)
    {
        const float bw = 2.f * s;
        constexpr float lbR = 0.780f, lbG = 0.839f, lbB = 0.863f;
        m_canvas.DrawRect(W.x, W.y, W.w, bw, lbR, lbG, lbB, 1.f);
        m_canvas.DrawRect(W.x, W.y + W.h - bw, W.w, bw, lbR, lbG, lbB, 1.f);
        m_canvas.DrawRect(W.x, W.y, bw, W.h, lbR, lbG, lbB, 1.f);
        m_canvas.DrawRect(W.x + W.w - bw, W.y, bw, W.h, lbR, lbG, lbB, 1.f);
        constexpr float dbR = 0.165f, dbG = 0.220f, dbB = 0.322f;
        const float o = 2.f * s;
        m_canvas.DrawRect(W.x - o, W.y - o, W.w + 2 * o, s, dbR, dbG, dbB, 1.f);
        m_canvas.DrawRect(W.x - o, W.y + W.h + o - s, W.w + 2 * o, s, dbR, dbG, dbB, 1.f);
        m_canvas.DrawRect(W.x - o, W.y - o, s, W.h + 2 * o, dbR, dbG, dbB, 1.f);
        m_canvas.DrawRect(W.x + W.w + o - s, W.y - o, s, W.h + 2 * o, dbR, dbG, dbB, 1.f);

        const float tts = s * 0.9f;
        const float tx = titleBar.x + (titleBar.w - m_canvas.TextWidth(title, tts)) * 0.5f;
        const float ty = titleBar.y + (titleBar.h - m_canvas.TextHeight(tts)) * 0.5f;
        m_canvas.DrawText(tx + 1.5f * s, ty + 1.5f * s, title, 0.f, 0.f, 0.f, tts);
        m_canvas.DrawText(tx, ty, title, 0.13f, 0.16f, 0.22f, tts);

        const bool ch = closeBox.Contains(m_ptrX, m_ptrY);
        m_canvas.DrawVGradient(closeBox.x, closeBox.y, closeBox.w, closeBox.h,
                               ch ? 1.f : 0.780f, ch ? 1.f : 0.839f, ch ? 1.f : 0.863f, 1.f,
                               0.439f, 0.553f, 0.659f, 1.f);
    }

    void DrawButton(const Rect& b, const char* caption, float s, bool highlighted, bool pressed)
    {
        const float ts = s * 1.0f;
        const float tw = m_canvas.TextWidth(caption, ts);
        const float th = m_canvas.TextHeight(ts);
        const float cx = b.x + (b.w - tw) * 0.5f, cy = b.y + (b.h - th) * 0.5f;
        if (highlighted)
        {
            if (pressed)
                m_canvas.DrawVGradient(b.x, b.y, b.w, b.h, 1.f, 1.f, 1.f, 1.f, 0.635f, 0.749f, 0.816f, 1.f);
            else
                m_canvas.DrawVGradient(b.x, b.y, b.w, b.h, 0.780f, 0.839f, 0.863f, 1.f, 0.439f, 0.553f, 0.659f, 1.f);
            m_canvas.DrawText(cx + 1.f * s, cy + 1.f * s, caption, 0.f, 0.f, 0.f, ts);
            m_canvas.DrawText(cx, cy, caption, 0.12f, 0.14f, 0.18f, ts);
            return;
        }
        m_canvas.DrawRect(b.x, b.y, b.w, b.h, 0.420f, 0.145f, 0.153f, 0.25f);
        const float px = (s > 1.f ? s : 1.f);
        m_canvas.DrawRect(b.x, b.y, b.w, px, 0.392f, 0.133f, 0.133f, 0.78f);
        m_canvas.DrawRect(b.x, b.y, px, b.h, 0.392f, 0.133f, 0.133f, 0.78f);
        m_canvas.DrawRect(b.x + b.w - px, b.y, px, b.h, 0.10f, 0.f, 0.f, 1.f);
        m_canvas.DrawRect(b.x, b.y + b.h - px, b.w, px, 0.10f, 0.f, 0.f, 1.f);
        m_canvas.DrawText(cx, cy, caption, 1.f, 1.f, 1.f, ts);
    }

    // Dropdown row: label (left) + recessed value box (right) showing the current
    // option + a down-arrow. Hovered value boxes brighten.
    void DrawDropDown(const Rect& row, const UiRow& r, int sel, bool hover, float s)
    {
        const float ts = s * 0.9f;
        const float th = m_canvas.TextHeight(ts);
        m_canvas.DrawText(row.x, row.y + (row.h - th) * 0.5f, r.label, 0.92f, 0.88f, 0.80f, ts);

        const Rect vb = Neuron::Render::Ui::ValueBox(row);
        const float a = hover ? 0.5f : 0.32f; // recessed dark-red, brighter on hover
        m_canvas.DrawRect(vb.x, vb.y, vb.w, vb.h, 0.42f, 0.16f, 0.16f, a);
        const float px = (s > 1.f ? s : 1.f);
        m_canvas.DrawRect(vb.x, vb.y, vb.w, px, 0.10f, 0.f, 0.f, 1.f);            // top (recessed)
        m_canvas.DrawRect(vb.x, vb.y, px, vb.h, 0.10f, 0.f, 0.f, 1.f);            // left
        m_canvas.DrawRect(vb.x + vb.w - px, vb.y, px, vb.h, 0.55f, 0.28f, 0.28f, 1.f);
        m_canvas.DrawRect(vb.x, vb.y + vb.h - px, vb.w, px, 0.55f, 0.28f, 0.28f, 1.f);

        const char* val = (sel >= 0 && sel < r.optCount) ? r.opts[sel] : "";
        m_canvas.DrawText(vb.x + 6.f * s, vb.y + (vb.h - th) * 0.5f, val, 1.f, 1.f, 1.f, ts);

        const Rect ar = Neuron::Render::Ui::ArrowBox(vb);
        m_canvas.DrawTriangle(ar.x + ar.w * 0.30f, ar.y + ar.h * 0.40f,
                              ar.x + ar.w * 0.70f, ar.y + ar.h * 0.40f,
                              ar.x + ar.w * 0.50f, ar.y + ar.h * 0.64f,
                              0.92f, 0.88f, 0.80f, 1.f);
    }

    void DrawPanelWindow(int win, float s)
    {
        const UiRow* rows; const int dd = WinRows(win, rows);
        const char* label = WinLabel(win);
        const int total = dd + (label ? 1 : 0);
        const auto L = Neuron::Render::Ui::BuildPanel(m_winX[win], m_winY[win], s, PanelWidth(s), total, true);

        DrawChromeBody(L.window, L.titleBar);
        for (int i = 0; i < dd; ++i)
            DrawDropDown(L.rows[i], rows[i], m_sel[win][i], (m_hoverWin == win && m_hoverItem == i), s);
        if (label)
        {
            const Rect& lr = L.rows[dd];
            const float ts = s * 0.9f;
            char buf[64];
            if (win == Win_Graphics) std::snprintf(buf, sizeof(buf), "%s   %d", label, static_cast<int>(m_fps + 0.5f));
            else                     std::snprintf(buf, sizeof(buf), "%s   v0.17-dev", label);
            m_canvas.DrawText(lr.x, lr.y + (lr.h - m_canvas.TextHeight(ts)) * 0.5f, buf, 0.85f, 0.85f, 0.55f, ts);
        }
        DrawButton(L.footerClose, er::ui::str("ui.close"), s, m_hoverWin == win && m_hoverItem == -2, false);
        DrawButton(L.footerApply, er::ui::str("ui.apply"), s, m_hoverWin == win && m_hoverItem == -3, false);
        DrawChromeFrame(L.window, L.titleBar, L.closeBox, WinTitle(win), s);
    }

    void DrawButtonListWindow(int win, float s)
    {
        const char* const* items; const int n = WinButtons(win, items);
        const auto L = Neuron::Render::Ui::BuildMainMenu(m_winX[win], m_winY[win], s, n);
        DrawChromeBody(L.window, L.titleBar);
        for (int i = 0; i < L.count; ++i)
        {
            const bool hover = (m_hoverWin == win && m_hoverItem == i);
            const bool pressed = hover && (m_pressWin == win && m_pressItem == i) && m_ptrDown;
            DrawButton(L.buttons[i], WinButtonCaption(win, i), s, hover, pressed);
        }
        DrawChromeFrame(L.window, L.titleBar, L.closeBox, WinTitle(win), s);
    }

    void DrawDropdownPopup(float s)
    {
        if (m_ddWin < 0) return;
        const UiRow* rows; const int dd = WinRows(m_ddWin, rows);
        const auto L = Neuron::Render::Ui::BuildPanel(m_winX[m_ddWin], m_winY[m_ddWin], s, PanelWidth(s),
                                                      dd + (WinLabel(m_ddWin) ? 1 : 0), true);
        const Rect vb = Neuron::Render::Ui::ValueBox(L.rows[m_ddRow]);
        const UiRow& r = rows[m_ddRow];
        const Rect panel = Neuron::Render::Ui::PopupPanel(vb, r.optCount);
        m_canvas.DrawRect(panel.x - s, panel.y, panel.w + 2 * s, panel.h + s, 0.06f, 0.02f, 0.03f, 0.97f);
        const float ts = s * 0.9f, th = m_canvas.TextHeight(ts);
        for (int i = 0; i < r.optCount; ++i)
        {
            const Rect pr = Neuron::Render::Ui::PopupRow(vb, i);
            if (i == m_ddHover)
                m_canvas.DrawVGradient(pr.x, pr.y, pr.w, pr.h, 0.780f, 0.839f, 0.863f, 1.f, 0.439f, 0.553f, 0.659f, 1.f);
            const bool dark = (i == m_ddHover);
            m_canvas.DrawText(pr.x + 6.f * s, pr.y + (pr.h - th) * 0.5f, r.opts[i],
                              dark ? 0.10f : 1.f, dark ? 0.12f : 1.f, dark ? 0.16f : 1.f, ts);
        }
    }

    Neuron::Render::CanvasRenderer& m_canvas;
    std::function<void(MenuSound)>  m_onSound;
    Neuron::Render::TextureGpu      m_uiGrey; // InterfaceGrey — title bars, highlight beam
    Neuron::Render::TextureGpu      m_uiRed;  // InterfaceRed  — buttons, window body

    bool  m_winOpen[Win_Count]{ false, false, false, false, false };
    float m_winX[Win_Count]{}, m_winY[Win_Count]{};
    bool  m_uiPlaced{ false };
    int   m_dragWin{ -1 };
    float m_dragDX{ 0.f }, m_dragDY{ 0.f };
    int   m_hoverWin{ -1 }, m_hoverItem{ -1 };
    int   m_pressWin{ -1 }, m_pressItem{ -1 };
    int   m_ddWin{ -1 }, m_ddRow{ -1 }; // open dropdown (window, row); -1 none
    int   m_ddHover{ -1 };               // hovered popup row
    int   m_sel[Win_Count][16]{};        // dropdown selection per panel row
    int   m_zorder[Win_Count]{ Win_MainMenu, Win_Options, Win_Screen, Win_Graphics, Win_Other }; // back→front
    UINT  m_lastW{ 0 }, m_lastH{ 0 };    // last screen size (for window cascade clamping)
    float m_fps{ 0.f };
    bool  m_quit{ false };

    // Pointer snapshot fed by Update each frame (used by the draw hover/press visuals).
    float m_ptrX{ 0.f }, m_ptrY{ 0.f };
    bool  m_ptrDown{ false }, m_ptrPressed{ false }, m_ptrReleased{ false };
};

} // namespace er
