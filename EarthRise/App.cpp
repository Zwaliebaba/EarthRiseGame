// App.cpp — EarthRise UWP client shell (M1b tech slice).
//
// IFrameworkView game loop:
//   SetWindow → DeviceResources::Initialize
//   Run       → BeginFrame / session.Tick / InterpBuffer / SceneRenderer / CanvasRenderer / EndFrame
//
// Uses NeuronClient (session + ReplicaManager + InterpBuffer) and
// NeuronRender (DeviceResources + SceneRenderer + CanvasRenderer).
//
// M1b: CngCrypto + WinRT DatagramSocket (§8.1) injected; three bots connect from ERHeadless.
// No int64_t propagated to the GPU — all world positions pass through
// FloatingOriginHelper before reaching SceneRenderer.

#include "pch.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <winrt/Windows.ApplicationModel.h> // Package (install location)
#include <winrt/Windows.Graphics.Display.h> // DisplayInformation (DPI scale)
#include <winrt/Windows.Storage.h>          // StorageFolder::Path
#include <winrt/Windows.UI.Core.h>          // CoreWindow, PointerEventArgs
#include <winrt/Windows.UI.Input.h>         // PointerPoint / button state
#include <winrt/Windows.UI.Popups.h>        // MessageDialog (server-unreachable notice)
#include <winrt/Windows.Foundation.Collections.h> // LocalSettings IPropertySet

// NeuronRender
#include "DeviceResources.h"
#include "SceneRenderer.h"
#include "CanvasRenderer.h"
#include "ParticleRenderer.h"
#include "PostProcess.h"
#include "UiLayout.h"

// NeuronAudio
#include "AudioEngine.h"

// EarthRise client
#include "StringTable.h"
#include "CmoLoader.h"
#include "DdsLoader.h"
#include "TacticalHud.h"      // in-space radar / overlays / overview / connection banner
#include "MenuUi.h"           // Darwinia windowed menus + options/settings
#include "FleetCommandController.h" // selection / control groups / smart commands

// NeuronClient
#include "SessionImpl.h"
#include "ReplicaManager.h"
#include "Interpolator.h"
#include "FleetControl.h"      // smart action, control groups, overview (M3 area G)
#include "HudOverlay.h"        // in-world selection rings / health bars (playable slice)
#include "Onboarding.h"        // objective/hint chain (playable slice)
#include "Picking.h"           // viewport click / box selection (playable slice)
#include "RtsCamera.h"         // free orbit/zoom/pan camera (playable slice)
#include "Starmap.h"           // beacon route solver (M3 area G)
#include "Command.h"           // FleetCommand encode (M3 area B)

// NeuronCore platform impls (compiled into NeuronClient.lib / accessible via link)
#include "CngCrypto.h"
#include "DatagramSocketAdapter.h"
#include "Protocol.h"
#include "ShapeCatalog.h"
#include "Snapshot.h"
#ifdef _DEBUG
#include "ServerStatusClient.h" // F3 server-status overlay (debug builds only, §21)
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────
static constexpr uint16_t kServerPort = 7777;
static constexpr float kInterpAlpha = 0.0f; // M1b: snap-on-ack (alpha unused)

// How long to wait for the very first connection before telling the player the server
// can't be reached (§8.5 handshake is sub-second on a live server; this is the "server
// is down / wrong address / loopback not exempt" budget). After this the client keeps
// retrying in the background — the notice is informational, not a hard stop.
static constexpr long long kConnectTimeoutMs = 8000;

#ifdef _DEBUG
// The server's SEPARATE diagnostic status port (must match server.statusPort in the
// ERServer config). Debug-only; the F3 overlay polls this UDP port for the read-only
// server status. 0 in the server config disables it (then the overlay stays empty).
static constexpr uint16_t kStatusPort = 7778;
// How many frames between status queries (~0.5 s at 60 Hz) — the port is low-traffic.
static constexpr int kStatusQueryIntervalFrames = 30;
#endif

// M5 real-auth credentials (§14). The server (server.devAuthStub = false) only spawns
// and replicates a player's world after a successful account login, so the client must
// authenticate. These dev defaults auto-register on first run and log in thereafter.
// TODO(M6 UX): replace with a login/registration screen that collects these from the
// player (SessionImpl::SetCredentials + LastAuthResult() already expose the seam).
static constexpr const char* kDevAuthUser     = "player1";
static constexpr const char* kDevAuthPassword = "player1-devpass";

// Read a file packaged with the app (relative to the install location) into a
// byte buffer. Read-only access to the package folder is permitted on UWP.
// Returns empty on any failure (callers fail-safe).
static std::vector<std::byte> LoadPackagedAsset(const wchar_t* relativePath)
{
  try
  {
    const auto folder = Windows::ApplicationModel::Package::Current().InstalledLocation().Path();
    const std::wstring full = std::wstring(folder.c_str()) + L"\\" + relativePath;
    std::ifstream f(full.c_str(), std::ios::binary | std::ios::ate);
    if (!f)
      return {};
    const std::streamsize n = f.tellg();
    if (n <= 0)
      return {};
    std::vector<std::byte> buf(static_cast<size_t>(n));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf.data()), n);
    return buf;
  }
  catch (...)
  {
    return {};
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// App
// ─────────────────────────────────────────────────────────────────────────────
struct App : implements<App, Windows::ApplicationModel::Core::IFrameworkViewSource, Windows::ApplicationModel::Core::IFrameworkView>
{
  // ---- render ----
  Neuron::Render::DeviceResources m_dr;
  Neuron::Render::SceneRenderer m_scene;
  Neuron::Render::CanvasRenderer m_canvas;
  Neuron::Render::PostProcess m_post;
  Neuron::Render::ParticleRenderer m_particles;
  bool m_bloom{false}; // true once PostProcess initialized (HDR path active)
  std::chrono::steady_clock::time_point m_lastFrame{}; // for particle dt
  // In-space HUD overlays (radar / brackets / overview / connection banner). Holds a
  // reference to m_canvas (declared above), so it must be initialized after it.
  er::TacticalHud m_hud{ m_canvas };
  // Darwinia windowed menus + options/settings (owns its own window/selection state +
  // chrome textures). Also references m_canvas, so likewise initialized after it.
  er::MenuUi m_menu{ m_canvas };

  // ---- audio (NeuronAudio) ----
  Neuron::Audio::AudioEngine m_audio;
  Neuron::Audio::ClipId      m_clipAmbient, m_clipClick, m_clipSelect;
  Neuron::Audio::VoiceId     m_ambientVoice{};
  bool                       m_audioReady{false};

  // Pointer snapshot (physical pixels). *_pressed/_released are one-frame edges.
  float m_ptrX{0.f}, m_ptrY{0.f};
  bool  m_ptrDown{false}, m_ptrPressed{false}, m_ptrReleased{false};

  // Camera input (playable slice): right-drag orbits, wheel zooms, arrows pan.
  bool  m_rmbDown{false};
  float m_prevPtrX{0.f}, m_prevPtrY{0.f};
  int   m_wheelAccum{0};
  // Right-click context menu: a stationary right-click (vs orbit-drag) opens it. App
  // detects the RMB-click edge; the menu state/logic lives in m_fleet, the draw in m_hud.
  bool  m_rmbReleased{false};               // one-frame RMB-up edge
  float m_rmbDownX0{0.f}, m_rmbDownY0{0.f};  // RMB press anchor (click vs orbit-drag)
  static constexpr float kCamYawPerPx   = 0.005f;
  static constexpr float kCamPitchPerPx = 0.005f;
  static constexpr float kCamPanFrac    = 0.02f; // pan step as a fraction of zoom distance

  // ---- HUD / settings-applied state ----
  float m_fps{0.f};
  float m_particleDensity{1.0f}; // scales the particle emitter rate (from MenuUi each frame)

  // ---- network ----
  Neuron::Net::CngCrypto m_crypto;
  std::unique_ptr<Neuron::Net::DatagramSocketAdapter> m_socket; // WinRT DatagramSocket (§8.1)
  Neuron::Net::EcPubKey m_pinnedPub{}; // zeroed = dev/test

#ifdef _DEBUG
  // Server-status overlay (F3, debug builds only, §21): a SEPARATE socket polls the
  // server's out-of-band diagnostic port and ServerStatusClient parses the reply into
  // the lines drawn by DrawServerStatusOverlay. Compiled out of retail entirely.
  std::unique_ptr<Neuron::Net::DatagramSocketAdapter>  m_statusSocket;
  std::unique_ptr<Neuron::Client::ServerStatusClient>  m_statusClient;
  bool m_showServerStatus{ false };
  int  m_statusReqCountdown{ 0 };
#endif
  std::unique_ptr<Neuron::Client::SessionImpl> m_session;
  Neuron::Client::ReplicaManager m_replica;
  Neuron::Client::InterpBuffer m_interp;

  // ---- fleet command (§23.4; M3 area G) ----
  er::FleetCommandController m_fleet;    // selection / control groups / smart commands
  float m_focus[3]{0.f, 0.f, 0.f};       // render-space camera focus (own base)
  Neuron::Client::RtsCamera m_camera;    // free orbit/zoom/pan camera (playable slice)
  Neuron::Client::Onboarding m_onboarding; // objective/hint chain (playable slice)
  DirectX::XMFLOAT4X4 m_viewProj{};       // cached view-proj for HUD overlays (playable slice)

  // ---- state ----
  std::array<uint8_t, 4096> m_snapBuf{};
  bool m_running{true};

  // ---- window / dpi / timing ----
  Windows::UI::Core::CoreWindow m_window{nullptr};
  float m_dpiScale{1.0f};
  std::chrono::steady_clock::time_point m_connectStart{};
  bool m_connectedLogged{false};
  // Server-unreachable notice (graceful MessageDialog instead of a silent hang/crash
  // when ERServer is down or the address is wrong). Latched so it shows once per
  // attempt; m_dialogOpen guards against stacking dialogs while one is on screen.
  bool m_serverUnreachableNotified{false};
  bool m_dialogOpen{false};
  // Per-shape bounding radius (indexed by ShapeCatalog id; 0 = not loaded / cube
  // fallback) used to normalize each mesh to a per-kind on-screen size.
  std::array<float, Neuron::Sim::kShapeCount> m_shapeRadius{};

  // Bounding radius of a shape (0 if unknown). Safe for any id.
  float ShapeRadius(uint16_t shapeId) const noexcept
  {
    return (shapeId < Neuron::Sim::kShapeCount) ? m_shapeRadius[shapeId] : 0.f;
  }

  // Target on-screen size (metres) the mesh is normalized to, per entity kind.
  static float TargetMetresForKind(uint8_t kind) noexcept
  {
    using K = Neuron::Sim::EntityKind;
    switch (static_cast<K>(kind))
    {
      case K::Base:          return 40.f;
      case K::Station:       return 70.f;
      case K::Structure:     return 80.f; // jumpgates, large platforms
      case K::Asteroid:      return 30.f;
      case K::Decoration:    return 25.f;
      case K::Debris:        return 12.f;
      case K::LootContainer: return 8.f;
      case K::Ship:
      default:               return 20.f;
    }
  }

  // Per-kind emitter glow colour; returns false for kinds that don't emit.
  static bool EmitterGlow(uint8_t kind, float& r, float& g, float& b) noexcept
  {
    using K = Neuron::Sim::EntityKind;
    switch (static_cast<K>(kind))
    {
      case K::Base:      r = 0.30f; g = 0.55f; b = 1.00f; return true; // blue
      case K::Ship:      r = 1.00f; g = 0.50f; b = 0.15f; return true; // orange
      case K::Station:   r = 0.40f; g = 0.80f; b = 0.90f; return true; // cyan
      case K::Structure: r = 0.60f; g = 0.40f; b = 1.00f; return true; // violet (jumpgate)
      default:           return false; // asteroids / debris / crates don't glow
    }
  }

  // ── IFrameworkViewSource ─────────────────────────────────────────────────
  Windows::ApplicationModel::Core::IFrameworkView CreateView() { return *this; }

  // ── IFrameworkView ───────────────────────────────────────────────────────
  void Initialize(const Windows::ApplicationModel::Core::CoreApplicationView&)
  {
    check_bool(m_crypto.Initialize());

    // WinRT DatagramSocket (§8.1) — no WSAStartup; binds asynchronously off the ASTA.
    m_socket = std::make_unique<Neuron::Net::DatagramSocketAdapter>();
    check_bool(m_socket->Open(0)); // ephemeral port

    m_session = std::make_unique<Neuron::Client::SessionImpl>(&m_crypto, m_pinnedPub, m_socket.get());
    // Real account auth (§14): bind to an account so the server spawns + replicates our
    // world. Requires the server to run real auth (server.devAuthStub = false); for a
    // dev-stub server, drop this line and pass a name to the ctor instead.
    m_session->SetCredentials(kDevAuthUser, kDevAuthPassword);

#ifdef _DEBUG
    // Server-status overlay (F3): its own ephemeral socket so status traffic never
    // interleaves with the game session's reliable-UDP. Points at the server's
    // separate diagnostic port (kStatusPort); if the server has statusPort 0 the
    // queries simply go unanswered and the overlay shows "no data".
    m_statusSocket = std::make_unique<Neuron::Net::DatagramSocketAdapter>();
    if (m_statusSocket->Open(0))
      m_statusClient = std::make_unique<Neuron::Client::ServerStatusClient>(
          m_statusSocket.get(), "127.0.0.1", kStatusPort);
#endif
  }

  void SetWindow(const Windows::UI::Core::CoreWindow& window)
  {
    m_window = window;

    // CoreWindow::Bounds() is in logical DIPs; the swap chain needs PHYSICAL
    // pixels. On a scaled display (e.g. 125%) using DIPs makes the back buffer
    // smaller than the window → the rendered region sits in the top-left with
    // black margins. Convert via the display DPI scale.
    auto disp = Windows::Graphics::Display::DisplayInformation::GetForCurrentView();
    m_dpiScale = disp.LogicalDpi() / 96.0f;

    const auto bounds = window.Bounds();
    const UINT w = PhysPx(bounds.Width);
    const UINT h = PhysPx(bounds.Height);

    const auto initStart = std::chrono::steady_clock::now();
    check_bool(m_dr.Initialize(winrt::get_unknown(window), w, h));

    // Post-process (HDR + bloom). If it initializes, the scene renders into the
    // HDR target and is composited to the back buffer; otherwise we fall back to
    // rendering the scene straight to the LDR back buffer (no glow, no crash).
    m_bloom = m_post.Initialize(&m_dr);
    const DXGI_FORMAT sceneFmt = m_bloom ? Neuron::Render::PostProcess::kHdrFormat
                                         : DXGI_FORMAT_B8G8R8A8_UNORM;
    OutputDebugStringA(m_bloom ? "[EarthRise] PostProcess: HDR+bloom enabled\n"
                               : "[EarthRise] PostProcess: init failed (LDR direct path)\n");

    check_bool(m_scene.Initialize(&m_dr, sceneFmt));
    check_bool(m_particles.Initialize(&m_dr, sceneFmt));
    if (auto dds = LoadPackagedAsset(L"Assets\\Textures\\Particle.dds"); !dds.empty())
      m_particles.SetTexture(Neuron::Render::DdsLoader::Load(m_dr.Device(), dds));
    check_bool(m_canvas.Initialize(&m_dr));
    LoadUiAssets(); // bitmap font + menu chrome (fail-safe: HUD falls back to blocks)
    LoadAudio();    // XAudio2 engine + clips + ambient bed (fail-safe: silent)
    // Route menu click/select feedback to the loaded UI clips (no audio dep in MenuUi).
    m_menu.SetSoundCallback([this](er::MenuSound s) {
      PlayUi(s == er::MenuSound::Click ? m_clipClick : m_clipSelect);
    });
    m_menu.InitSettings(); // default selections, then restore any persisted ones
    m_menu.LoadSettings();
    {
      const long long initMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - initStart)
                                   .count();
      char buf[112];
      std::snprintf(buf, sizeof(buf), "[EarthRise] DX init %lld ms (backbuffer %ux%u, dpi %.2f)\n",
                    initMs, w, h, m_dpiScale);
      OutputDebugStringA(buf);
    }

    // Load the whole shape catalog into the renderer (fail-safe per shape: a
    // missing/unparseable asset just leaves that shape on the cube fallback).
    LoadShapeCatalog();

    // Start connecting to ERServer.
    m_connectStart = std::chrono::steady_clock::now();
    m_session->Connect("127.0.0.1", kServerPort);

    // Keep the back buffer matched to the window across resize / maximize.
    window.SizeChanged([this](Windows::UI::Core::CoreWindow const&,
                              Windows::UI::Core::WindowSizeChangedEventArgs const&) { ApplyResize(); });
    window.KeyDown({this, &App::OnKeyDown});
    window.Closed({this, &App::OnClosed});
    window.PointerMoved({this, &App::OnPointerMoved});
    window.PointerPressed({this, &App::OnPointerPressed});
    window.PointerReleased({this, &App::OnPointerReleased});
    window.PointerWheelChanged({this, &App::OnPointerWheel});
  }

  // ── Pointer input (physical pixels via the DPI scale) ─────────────────────
  void OnPointerMoved(const Windows::UI::Core::CoreWindow&,
                      const Windows::UI::Core::PointerEventArgs& e)
  {
    const auto p = e.CurrentPoint().Position();
    m_ptrX = p.X * m_dpiScale;
    m_ptrY = p.Y * m_dpiScale;
    const auto props = e.CurrentPoint().Properties();
    m_ptrDown = props.IsLeftButtonPressed();
    m_rmbDown = props.IsRightButtonPressed(); // right-drag orbits the camera
  }
  void OnPointerPressed(const Windows::UI::Core::CoreWindow&,
                        const Windows::UI::Core::PointerEventArgs& e)
  {
    const auto p = e.CurrentPoint().Position();
    m_ptrX = p.X * m_dpiScale;
    m_ptrY = p.Y * m_dpiScale;
    const auto props = e.CurrentPoint().Properties();
    m_ptrDown = props.IsLeftButtonPressed();
    m_rmbDown = props.IsRightButtonPressed();
    // Anchor the orbit so the first frame of a right-drag has a zero delta, and record the
    // RMB press point so release can tell a click from an orbit-drag.
    if (m_rmbDown) { m_prevPtrX = m_ptrX; m_prevPtrY = m_ptrY; m_rmbDownX0 = m_ptrX; m_rmbDownY0 = m_ptrY; }
    m_ptrPressed = true;
  }
  // Mouse wheel → camera zoom (accumulated; consumed in UpdateCameraInput).
  void OnPointerWheel(const Windows::UI::Core::CoreWindow&,
                      const Windows::UI::Core::PointerEventArgs& e)
  {
    m_wheelAccum += e.CurrentPoint().Properties().MouseWheelDelta();
  }
  void OnPointerReleased(const Windows::UI::Core::CoreWindow&,
                         const Windows::UI::Core::PointerEventArgs& e)
  {
    const auto p = e.CurrentPoint().Position();
    m_ptrX = p.X * m_dpiScale;
    m_ptrY = p.Y * m_dpiScale;
    const auto props = e.CurrentPoint().Properties();
    const bool wasRmb = m_rmbDown;
    m_ptrDown = props.IsLeftButtonPressed();
    m_rmbDown = props.IsRightButtonPressed();
    m_ptrReleased = true;
    if (wasRmb && !m_rmbDown) m_rmbReleased = true; // RMB-up edge (consumed each frame)
  }

  // Apply camera input each frame: right-drag orbit, wheel zoom, arrow-key pan.
  // Platform-independent math lives in Neuron::Client::RtsCamera; this just maps
  // UWP pointer/keyboard state onto it. (Pan speed scales with zoom distance.)
  void UpdateCameraInput()
  {
    if (m_rmbDown)
    {
      const float dx = m_ptrX - m_prevPtrX;
      const float dy = m_ptrY - m_prevPtrY;
      m_camera.Rotate(dx * kCamYawPerPx, -dy * kCamPitchPerPx);
    }
    m_prevPtrX = m_ptrX;
    m_prevPtrY = m_ptrY;

    if (m_wheelAccum != 0)
    {
      const float notches = static_cast<float>(m_wheelAccum) / 120.0f; // one detent = 120
      m_camera.Zoom(std::pow(0.88f, notches));                          // wheel up zooms in
      m_wheelAccum = 0;
    }

    if (m_window)
    {
      using VK = Windows::System::VirtualKey;
      using KS = Windows::UI::Core::CoreVirtualKeyStates;
      auto down = [&](VK k) { return (m_window.GetKeyState(k) & KS::Down) == KS::Down; };
      const float step = m_camera.Distance() * kCamPanFrac;
      float r = 0.0f, f = 0.0f;
      if (down(VK::Left))  r -= step;
      if (down(VK::Right)) r += step;
      if (down(VK::Up))    f += step;
      if (down(VK::Down))  f -= step;
      if (r != 0.0f || f != 0.0f) m_camera.PanWorld(r, f); // (also releases base-follow)
    }
  }

  // Logical DIP -> physical pixels for the swap chain.
  UINT PhysPx(float dip) const noexcept { return static_cast<UINT>(dip * m_dpiScale + 0.5f); }

  void ApplyResize()
  {
    if (!m_window)
      return;
    const auto b = m_window.Bounds();
    m_dr.Resize(PhysPx(b.Width), PhysPx(b.Height));
    if (m_bloom)
      m_post.Resize(); // recreate HDR/bloom targets at the new back-buffer size
  }

  void Load(const hstring&) {}

  void Run()
  {
    auto window = Windows::UI::Core::CoreWindow::GetForCurrentThread();
    window.Activate();

    // Pump the handshake to completion up front so connect isn't paced one
    // round-trip per vsynced frame. Each Tick() sends the next step; the brief
    // sleep lets the server reply over loopback. Bounded (~400 ms) so a missing
    // server just falls through to the normal retrying render loop.
    for (int i = 0; i < 200 && m_running &&
                    m_session->GetState() != Neuron::Client::SessionState::Connected;
         ++i)
    {
      window.Dispatcher().ProcessEvents(Windows::UI::Core::CoreProcessEventsOption::ProcessAllIfPresent);
      m_session->Tick();
      if (m_session->GetState() == Neuron::Client::SessionState::Connected)
        break;
      Sleep(2);
    }

    while (m_running)
    {
      window.Dispatcher().ProcessEvents(Windows::UI::Core::CoreProcessEventsOption::ProcessAllIfPresent);

      Tick();
    }
  }

  void Uninitialize()
  {
    m_audio.shutdown();
    m_canvas.Uninitialize();
    m_particles.Uninitialize();
    m_post.Uninitialize();
    if (m_session)
      m_session->Disconnect(); // best-effort graceful notice while the socket is alive
    m_session.reset();
    m_socket.reset();
#ifdef _DEBUG
    m_statusClient.reset();
    m_statusSocket.reset();
#endif
  }

  // Bring up XAudio2, load the PCM-16 clips, and start the looping ambient bed.
  // Fail-safe: any failure leaves the game silent (m_audioReady stays false).
  void LoadAudio()
  {
    if (FAILED(m_audio.initialize()))
    {
      OutputDebugStringA("[EarthRise] audio: XAudio2 init failed (silent)\n");
      return;
    }
    auto load = [&](const wchar_t* path, Neuron::Audio::ClipId& out) {
      if (auto w = LoadPackagedAsset(path); !w.empty())
        m_audio.loadClip(w, out);
    };
    load(L"Assets\\Audio\\ambient_space.wav", m_clipAmbient);
    load(L"Assets\\Audio\\ui_click.wav", m_clipClick);
    load(L"Assets\\Audio\\ui_select.wav", m_clipSelect);

    Neuron::Audio::MixSnapshot mix{}; // defaults to 1.0 across master + buses
    m_audio.setMix(mix);

    if (m_clipAmbient.valid())
    {
      Neuron::Audio::PlayParams pp;
      pp.bus = Neuron::Audio::Bus::Ambient;
      pp.loop = true;
      pp.volume = 0.7f;
      m_ambientVoice = m_audio.play(m_clipAmbient, pp);
    }
    m_audioReady = m_audio.initialized();
    char buf[96];
    std::snprintf(buf, sizeof(buf), "[EarthRise] audio: ready=%d amb=%d click=%d select=%d\n",
                  static_cast<int>(m_audioReady), static_cast<int>(m_clipAmbient.valid()),
                  static_cast<int>(m_clipClick.valid()), static_cast<int>(m_clipSelect.valid()));
    OutputDebugStringA(buf);
  }

  // Fire a one-shot UI sound on the Ui bus (no-op if audio is off).
  void PlayUi(Neuron::Audio::ClipId clip)
  {
    if (!m_audioReady || !clip.valid()) return;
    Neuron::Audio::PlayParams pp;
    pp.bus = Neuron::Audio::Bus::Ui;
    pp.volume = 0.8f;
    m_audio.play(clip, pp);
  }

  // Load the UI font atlas + menu chrome textures (uncompressed BGRA DDS). The
  // font drives real DrawText; the grey/red gradient strips skin the windows.
  // Fail-safe: if anything is missing the HUD just won't render the menu.
  // Load the shared bitmap font into the Canvas, then hand the Darwinia chrome skins to
  // MenuUi (which owns them). The font drives all 2D text; the chrome is menu-only.
  void LoadUiAssets()
  {
    if (auto dds = LoadPackagedAsset(L"Assets\\Fonts\\EditorFont-ENG.dds"); !dds.empty())
    {
      auto font = Neuron::Render::DdsLoader::Load(m_dr.Device(), dds);
      if (font.valid())
        m_canvas.SetFont(std::move(font), er::format::FontAtlasConfig{}); // 256x224, 16x16, cp32
    }
    Neuron::Render::TextureGpu grey, red;
    if (auto dds = LoadPackagedAsset(L"Assets\\Textures\\InterfaceGrey.dds"); !dds.empty())
      grey = Neuron::Render::DdsLoader::Load(m_dr.Device(), dds);
    if (auto dds = LoadPackagedAsset(L"Assets\\Textures\\InterfaceRed.dds"); !dds.empty())
      red = Neuron::Render::DdsLoader::Load(m_dr.Device(), dds);

    char buf[96];
    std::snprintf(buf, sizeof(buf), "[EarthRise] UI assets: font=%d grey=%d red=%d\n",
                  static_cast<int>(m_canvas.hasFont()), static_cast<int>(grey.valid()),
                  static_cast<int>(red.valid()));
    OutputDebugStringA(buf);
    m_menu.SetChrome(std::move(grey), std::move(red));
  }

  // HUD/menu scale: physical-height ratio × the "Large Menus" multiplier (owned by
  // MenuUi). Used by the HUD (TacticalHud frame) and the fleet-command hit-tests.
  float MenuScale(UINT screenH) const noexcept
  {
    return (screenH > 0 ? static_cast<float>(screenH) : 1080.f) / 1080.f * m_menu.UiScaleMul();
  }

  // Push the current menu selections to the live engine knobs (called each frame). The
  // selection→value mapping lives in MenuUi; App just applies the derived values.
  void ApplySettings()
  {
    m_dr.SetVSync(m_menu.VSyncOn());
    m_post.SetBloomIntensity(m_menu.BloomIntensity());
    m_post.SetPixelEffect(m_menu.PixelEffect());
    m_particleDensity = m_menu.ParticleDensity();
    m_particles.SetDensity(m_particleDensity);
  }




  // Load every ShapeCatalog entry into the renderer: the CMO mesh plus its
  // derived diffuse (<stem>1.dds). Per-shape fail-safe — a missing/unparseable
  // asset is skipped (that shape draws the placeholder cube). Logs a summary.
  void LoadShapeCatalog()
  {
    const auto t0 = std::chrono::steady_clock::now();
    uint32_t loaded = 0, textured = 0, failed = 0;

    for (const auto& def : Neuron::Sim::kShapes)
    {
      // Widen the (ASCII) package-relative paths for the file APIs.
      const std::wstring cmoW(def.cmoPath.begin(), def.cmoPath.end());

      auto cmoBytes = LoadPackagedAsset(cmoW.c_str());
      if (cmoBytes.empty()) { ++failed; continue; }

      auto mesh = Neuron::Render::CmoLoader::Load(m_dr.Device(), cmoBytes);
      if (!mesh.valid()) { ++failed; continue; }
      m_shapeRadius[def.id] = mesh.boundingRadius;

      // Diffuse path: replace the ".cmo" suffix with "1.dds".
      std::string diffuse(def.cmoPath);
      diffuse.replace(diffuse.size() - 4, 4, "1.dds");
      const std::wstring diffW(diffuse.begin(), diffuse.end());

      Neuron::Render::TextureGpu tex;
      if (auto dds = LoadPackagedAsset(diffW.c_str()); !dds.empty())
      {
        tex = Neuron::Render::DdsLoader::Load(m_dr.Device(), dds);
        if (tex.valid()) ++textured;
      }

      m_scene.SetShape(def.id, std::move(mesh), std::move(tex));
      ++loaded;
    }

    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "[EarthRise] ShapeCatalog: %u/%u meshes loaded (%u textured, %u failed) in %lld ms\n",
                  loaded, static_cast<unsigned>(Neuron::Sim::kShapeCount), textured, failed, ms);
    OutputDebugStringA(buf);
  }

  // Graceful "can't reach the server" notice. A CoreWindow (no-XAML) app uses
  // Windows.UI.Popups.MessageDialog, not a XAML ContentDialog. ShowAsync must be awaited
  // on the UI/ASTA thread — which is where Tick() (the caller) runs — so this is a
  // fire_and_forget coroutine. Retry restarts the handshake; Exit closes the app.
  winrt::fire_and_forget ShowServerUnreachable()
  {
    if (m_dialogOpen)
      co_return;
    m_dialogOpen = true;
    auto lifetime = get_strong(); // keep this App alive across the co_await

    Windows::UI::Popups::MessageDialog dlg{
        L"EarthRise can't reach the game server.\n\n"
        L"Make sure ERServer is running and reachable, then choose Retry. "
        L"The client keeps trying to connect in the background.",
        L"Server unavailable" };
    dlg.Commands().Append(Windows::UI::Popups::UICommand{ L"Retry" });
    dlg.Commands().Append(Windows::UI::Popups::UICommand{ L"Exit" });
    dlg.DefaultCommandIndex(0); // Retry on Enter
    dlg.CancelCommandIndex(1);  // Exit on Esc

    Windows::UI::Popups::IUICommand chosen{ nullptr };
    try
    {
      chosen = co_await dlg.ShowAsync();
    }
    catch (...)
    {
      // Couldn't show (e.g. a dialog is already up) — keep retrying silently.
      m_dialogOpen = false;
      co_return;
    }
    m_dialogOpen = false;

    if (chosen && std::wstring_view{ chosen.Label() } == L"Exit")
    {
      m_running = false;
      Windows::ApplicationModel::Core::CoreApplication::Exit();
      co_return;
    }

    // Retry: re-arm the notice and restart the handshake — but only if we still aren't
    // connected (the server may have come up while the dialog sat open, and the session
    // reconnects on its own; don't tear down a live link).
    m_serverUnreachableNotified = false;
    if (m_session && m_session->GetState() != Neuron::Client::SessionState::Connected)
    {
      m_connectStart = std::chrono::steady_clock::now();
      try { m_session->Connect("127.0.0.1", kServerPort); } catch (...) {}
    }
  }

  // ── Game tick ────────────────────────────────────────────────────────────
  void Tick()
  {
    // Push current settings selections to the live engine knobs.
    ApplySettings();

    // 1. Network I/O. Defensive: a transport-layer fault (e.g. an unreachable host or a
    //    WinRT socket error surfacing here) must degrade to "not connected", never crash
    //    the client — the unreachable notice below tells the player gracefully.
    try
    {
      m_session->Tick();
    }
    catch (...)
    {
      OutputDebugStringA("[EarthRise] session tick threw; treating as disconnected\n");
    }

#ifdef _DEBUG
    // 1b. Server-status overlay (F3): poll replies every frame, and re-query the
    //     diagnostic port on a slow cadence while the overlay is visible.
    if (m_statusClient)
    {
      m_statusClient->Poll();
      if (m_showServerStatus && --m_statusReqCountdown <= 0)
      {
        m_statusClient->RequestStatus();
        m_statusReqCountdown = kStatusQueryIntervalFrames;
      }
    }
#endif

    // One-shot: how long from Connect() to fully connected (handshake cost).
    const bool connected = m_session->GetState() == Neuron::Client::SessionState::Connected;
    if (!m_connectedLogged && connected)
    {
      m_connectedLogged = true;
      const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - m_connectStart)
                               .count();
      char buf[80];
      std::snprintf(buf, sizeof(buf), "[EarthRise] connected in %lld ms\n", ms);
      OutputDebugStringA(buf);
    }

    // Server-unreachable notice: if the first connection hasn't established within the
    // budget, tell the player gracefully (once) instead of hanging silently. The session
    // keeps retrying with backoff in the background (M5 area G), so this is informational.
    if (!connected && !m_connectedLogged && !m_serverUnreachableNotified && !m_dialogOpen)
    {
      const long long waitedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - m_connectStart)
                                     .count();
      if (waitedMs >= kConnectTimeoutMs)
      {
        m_serverUnreachableNotified = true;
        ShowServerUnreachable();
      }
    }

    // 2. Consume any new snapshots.
    size_t snapSize = 0;
    while (m_session->PollSnapshot(m_snapBuf, snapSize))
    {
      if (m_replica.Update(std::span<const uint8_t>(m_snapBuf.data(), snapSize), m_session->PlayerNetId()))
        m_interp.Advance(m_replica.Current());
    }

    // 2.5 Camera input (orbit/zoom/pan) + onboarding objective chain.
    UpdateCameraInput();
    {
      const uint32_t self = m_session ? m_session->PlayerNetId() : 0;
      m_onboarding.Observe(Neuron::Client::ObserveWorld(
          m_interp.curr, self, static_cast<uint32_t>(m_fleet.Selection().size())));
    }

    // 3. Render.
    const UINT w = m_dr.Width();
    const UINT h = m_dr.Height();

    // Build per-frame view-proj: simple fixed look-at for M1b.
    using namespace DirectX;
    // Center the camera on the player's own base — it drifts, so a fixed look-at
    // at the world origin lets it slide off screen. Fall back to first entity.
    float cx = 0.f, cy = 0.f, cz = 0.f;
    {
      const Neuron::Client::ReplicaSet& cam = m_interp.curr;
      const uint32_t pid = m_session->PlayerNetId();
      for (uint32_t i = 0; i < cam.count; ++i)
      {
        const auto& ce = cam.entities[i];
        if (ce.valid && (ce.networkId == pid || pid == 0))
        {
          cx = ce.x;
          cy = ce.y;
          cz = ce.z;
          break;
        }
      }
    }
    // Free RTS camera (playable slice): while "follow" is on it tracks the player's
    // drifting base; right-drag/wheel/arrows orbit, zoom and pan it (UpdateCameraInput).
    if (m_camera.Follow()) m_camera.SetFocus({ cx, cy, cz });
    const XMFLOAT3 eyeF = m_camera.Eye();
    const XMFLOAT3 atF = m_camera.At();
    const XMFLOAT3 upF = m_camera.Up();
    const XMVECTOR eye = XMLoadFloat3(&eyeF);
    const XMVECTOR at = XMLoadFloat3(&atF);
    const XMVECTOR up = XMLoadFloat3(&upF);
    const float fov = m_menu.FovRadians();
    const float asp = (h > 0) ? static_cast<float>(w) / static_cast<float>(h) : 1.f;

    XMMATRIX view = XMMatrixLookAtRH(eye, at, up);
    XMMATRIX proj = XMMatrixPerspectiveFovRH(fov, asp, 0.1f, 10000.f);
    // SceneVS reads viewProj from a (HLSL-default) COLUMN-MAJOR cbuffer and
    // applies it as mul(viewProj, worldPos) (column-vector). Storing view*proj
    // row-major lets the column-major load reinterpret it as the transpose that
    // multiply needs — so we must NOT pre-transpose here. (Transposing in C++ as
    // well cancels out and corrupts the transform — the old "thin box" bug.)
    XMMATRIX viewProj = view * proj;

    XMFLOAT4X4 vpf;
    XMStoreFloat4x4(&vpf, viewProj);
    m_viewProj = vpf; // cache for the HUD overlay projection (playable slice)

    // Camera basis for billboard particles (world space).
    float camRight[3], camUp[3];
    {
      const XMVECTOR fwd = XMVector3Normalize(XMVectorSubtract(at, eye));
      const XMVECTOR rgt = XMVector3Normalize(XMVector3Cross(up, fwd));
      const XMVECTOR cup = XMVector3Cross(fwd, rgt);
      XMFLOAT3 v;
      XMStoreFloat3(&v, rgt); camRight[0] = v.x; camRight[1] = v.y; camRight[2] = v.z;
      XMStoreFloat3(&v, cup); camUp[0] = v.x; camUp[1] = v.y; camUp[2] = v.z;
    }
    // Per-frame delta time for the particle drift.
    const auto nowT = std::chrono::steady_clock::now();
    float dt = (m_lastFrame.time_since_epoch().count() == 0)
                   ? 0.f
                   : std::chrono::duration<float>(nowT - m_lastFrame).count();
    m_lastFrame = nowT;
    if (dt > 0.f) m_fps = m_fps * 0.9f + (1.f / dt) * 0.1f; // smoothed FPS for the HUD

    // (Particle emitters are built + Update() runs after the scene-entity list is
    // gathered below, then drawn in the scene pass.)

    // Audio listener = the camera (positions are camera-relative, so the
    // listener sits at the origin facing the view). Update the engine once/frame.
    if (m_audioReady)
    {
      Neuron::Audio::Listener lis;
      XMFLOAT3 fwd;
      XMStoreFloat3(&fwd, XMVector3Normalize(XMVectorSubtract(at, eye)));
      lis.forward = fwd;
      lis.up = { 0.f, 1.f, 0.f };
      m_audio.setListener(lis);
      m_audio.update(dt);
    }

    // Scene lighting (see Lighting.hlsli): a single WORLD-FIXED warm sun (key),
    // cool fill + ambient to lift the shadow side, and a per-frame view-based
    // Fresnel rim. World-fixed key/fill give consistent lit/shadow sides across
    // the whole scene (natural depth); only the rim's view direction tracks the
    // camera. The warm-key / cool-fill split is the core Darwinia colour cue.
    {
      const XMVECTOR viewDir = XMVector3Normalize(XMVectorSubtract(eye, at)); // surface->camera

      Neuron::Render::SceneRenderer::Lighting lit{};
      // World-fixed sun: up / right / toward the camera side (-Z) so camera-facing
      // surfaces catch the key. Fill comes from the opposite side, cooler.
      const XMVECTOR keyDir = XMVector3Normalize(XMVectorSet(0.45f, 0.55f, -0.70f, 0.f));
      const XMVECTOR fillDir = XMVector3Normalize(XMVectorSet(-0.55f, 0.20f, -0.45f, 0.f));
      XMFLOAT3 v;
      XMStoreFloat3(&v, keyDir);  lit.keyDir[0] = v.x;  lit.keyDir[1] = v.y;  lit.keyDir[2] = v.z;
      XMStoreFloat3(&v, fillDir); lit.fillDir[0] = v.x; lit.fillDir[1] = v.y; lit.fillDir[2] = v.z;
      XMStoreFloat3(&v, viewDir); lit.viewDir[0] = v.x; lit.viewDir[1] = v.y; lit.viewDir[2] = v.z;
      // Warm orange "sun" key; warm-neutral fill + ambient (no blue cast); warm
      // rim. Darwinia's world is warm against a near-black void, so the fill and
      // ambient lean warm-neutral rather than cool.
      lit.keyColor[0] = 1.40f; lit.keyColor[1] = 1.08f; lit.keyColor[2] = 0.74f;
      lit.fillColor[0] = 0.30f; lit.fillColor[1] = 0.26f; lit.fillColor[2] = 0.22f;
      lit.ambient[0] = 0.13f; lit.ambient[1] = 0.105f; lit.ambient[2] = 0.085f;
      lit.rimColor[0] = 0.60f; lit.rimColor[1] = 0.40f; lit.rimColor[2] = 0.20f;
      lit.rimPower = 2.5f;
      m_scene.SetLighting(lit);
    }

    // Gather scene entities from the interp buffer.
    const Neuron::Client::ReplicaSet& rs = m_interp.curr;
    Neuron::Render::SceneEntity entities[Neuron::Client::ReplicaSet::kMaxEntities];
    UINT entCount = 0;

    for (uint32_t i = 0; i < rs.count && entCount < Neuron::Render::SceneRenderer::kMaxEntities; ++i)
    {
      const auto& re = rs.entities[i];
      if (!re.valid)
        continue;
      // Positions already in render-space floats via ReplicaManager::Update.
      Neuron::Render::SceneEntity& se = entities[entCount++];
      se.x = re.x;
      se.y = re.y;
      se.z = re.z;
      se.yaw = 0.f;
      se.kind = re.entityType;
      se.shapeId = re.shapeId;

      // Normalize the mesh by its bounding radius to a per-kind display size so
      // models with different authored units sit at a sensible relative scale.
      const float radius = ShapeRadius(re.shapeId);
      const float targetMetres = TargetMetresForKind(re.entityType);
      se.scale = (radius > 0.f) ? (targetMetres / radius) : (targetMetres / 20.f);

      // Textured shapes sample their diffuse; the colour is only a fallback tint
      // for shapes that loaded without a texture. Negative r = "use kind default".
      se.r = -1.f;
      se.g = se.b = 0.f;
    }

    // Per-entity emitter glow (engine/structure auras) from the scene list, then
    // advance the particle field. Drawn in the scene pass below.
    {
      Neuron::Render::ParticleRenderer::EmitterDesc ems[Neuron::Render::SceneRenderer::kMaxEntities];
      int emCount = 0;
      for (UINT i = 0; i < entCount; ++i)
      {
        const auto& se = entities[i];
        float gr, gg, gb;
        if (!EmitterGlow(se.kind, gr, gg, gb)) continue;
        auto& e = ems[emCount++];
        e.x = se.x; e.y = se.y; e.z = se.z;
        e.r = gr; e.g = gg; e.b = gb;
        e.size = TargetMetresForKind(se.kind) * 0.30f;
        e.rate = 16.f * m_particleDensity;
      }
      m_particles.SetEmitters(ems, emCount);
      m_particles.Update(dt, cx, cy, cz);
    }

    // Once-per-second diagnostic (VS Output / DebugView; the on-screen HUD font
    // is still placeholder blocks). Shows where the player's base actually is.
    {
      static auto lastDbg = std::chrono::steady_clock::now();
      const auto nowDbg = std::chrono::steady_clock::now();
      if (nowDbg - lastDbg >= std::chrono::seconds(1))
      {
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "[EarthRise] state=%d entities=%u playerNet=%u cameraCenter=(%.1f,%.1f,%.1f)\n",
                      static_cast<int>(m_session->GetState()), entCount, m_session->PlayerNetId(),
                      cx, cy, cz);
        OutputDebugStringA(buf);
        lastDbg = nowDbg;
      }
    }

    m_dr.BeginFrame();
    auto* cl = m_dr.CmdList();

    // 3D scene. With bloom, render into the HDR target then composite to the
    // back buffer; otherwise draw straight to the (already-bound) back buffer.
    if (m_bloom)
    {
      m_post.BeginScene(cl);
      m_scene.Render(cl, vpf, entities, entCount);
      m_particles.Render(cl, vpf, camRight[0], camRight[1], camRight[2], camUp[0], camUp[1], camUp[2]);
      m_post.Resolve(cl);
    }
    else
    {
      m_scene.Render(cl, vpf, entities, entCount);
      m_particles.Render(cl, vpf, camRight[0], camRight[1], camRight[2], camUp[0], camUp[1], camUp[2]);
    }

    // Fleet command: a radar click issues a smart action against the nearest contact
    // (must run before the menu consumes the pointer-press, §23.1/area G).
    m_focus[0] = cx; m_focus[1] = cy; m_focus[2] = cz;
    er::FleetInput fin{};
    fin.session = m_session.get();
    fin.replica = &m_interp.curr;
    fin.screenW = w; fin.screenH = h;
    fin.ptrX = m_ptrX; fin.ptrY = m_ptrY;
    fin.ptrPressed = m_ptrPressed; fin.ptrReleased = m_ptrReleased;
    fin.menuScale = MenuScale(h);
    fin.overMenuWindow = m_menu.PointerOverWindow(m_ptrX, m_ptrY, h);
    fin.rmbReleased = m_rmbReleased; fin.rmbDownX0 = m_rmbDownX0; fin.rmbDownY0 = m_rmbDownY0;
    // Right-click context menu first: a left-click it consumes pre-empts radar/selection.
    if (m_fleet.HandleContextMenu(fin, vpf)) {
      m_ptrPressed = m_ptrReleased = false; // eat the click so it doesn't also select
    } else {
      m_fleet.HandleRadarClick(fin, cx, cz);
      m_fleet.HandleViewportSelection(fin, vpf); // 3D-viewport click / box selection
    }
    m_rmbReleased = false; // consume the RMB edge after the menu had its chance

    // HUD + windowed UI. Interaction (hover/press/drag/dropdowns) runs first; the menu
    // consumes the pointer edges, App clears them, and a Quit selection ends the loop.
    m_menu.Update(w, h, m_ptrX, m_ptrY, m_ptrDown, m_ptrPressed, m_ptrReleased);
    m_ptrPressed = m_ptrReleased = false;
    if (m_menu.QuitRequested()) m_running = false;
    m_canvas.Reset();
    const Neuron::Client::SessionState st = m_session->GetState();
    const char* stateStr = (st == Neuron::Client::SessionState::Connected)
                             ? "CONNECTED"
                             : (st == Neuron::Client::SessionState::Handshaking)
                             ? "HANDSHAKE"
                             : (st == Neuron::Client::SessionState::Authenticating)
                             ? "AUTH"
                             : "OFFLINE";
    const float hudS = (h > 0 ? static_cast<float>(h) : 1080.f) / 1080.f;
    m_canvas.DrawText(12.f * hudS, 10.f * hudS, er::ui::str("app.title"), 0.35f, 0.85f, 1.0f, hudS);
    m_canvas.DrawText(12.f * hudS, 30.f * hudS, stateStr, 0.95f, 0.80f, 0.35f, hudS);

    // Perf gate readout (§16.3): GPU (timestamp queries) + CPU ms + FPS, red
    // when over the ~16.6 ms / 60 Hz frame budget.
    {
      const double gpuMs = m_dr.GpuFrameMs();
      const float cpuMs = (m_fps > 0.f) ? (1000.f / m_fps) : 0.f;
      char perf[64];
      std::snprintf(perf, sizeof(perf), "GPU %.1f  CPU %.1f  %d FPS",
                    gpuMs, static_cast<double>(cpuMs), static_cast<int>(m_fps + 0.5f));
      const bool over = gpuMs > 16.6 || cpuMs > 16.6f;
      m_canvas.DrawText(12.f * hudS, 50.f * hudS, perf,
                        over ? 1.0f : 0.4f, over ? 0.4f : 0.9f, over ? 0.35f : 0.5f, hudS * 0.85f);
    }

    // In-space HUD overlays (TacticalHud): build the per-frame view of App's state once,
    // then delegate the radar / brackets / overview / banner draws to it.
    er::TacticalHudFrame hud{};
    hud.screenW = w; hud.screenH = h;
    hud.hudScale = MenuScale(h);
    hud.uiReady = m_menu.Ready();
    hud.replica = &m_interp.curr;
    hud.selection = &m_fleet.Selection();
    hud.selfNetId = m_session ? m_session->PlayerNetId() : 0;
    hud.targetNetId = m_fleet.TargetNetId();
    hud.focus = m_focus;
    hud.viewProj = &m_viewProj;
    hud.objectiveText = m_onboarding.CurrentText();
    hud.selDragging = m_fleet.DragActive(); hud.ptrDown = m_ptrDown;
    hud.selX0 = m_fleet.DragStartX(); hud.selY0 = m_fleet.DragStartY(); hud.ptrX = m_ptrX; hud.ptrY = m_ptrY;
    hud.sessionState = m_session ? m_session->GetState()
                                 : Neuron::Client::SessionState::Connected;
    hud.serverUnreachable = m_serverUnreachableNotified;
    hud.menuOpen = m_fleet.MenuOpen(); hud.menuX = m_fleet.MenuX(); hud.menuY = m_fleet.MenuY();
    hud.menuActions = &m_fleet.MenuActions();

    m_hud.DrawRadar(hud, entities, entCount, cx, cy, cz); // 2D radar disc (under the windowed UI)
    m_hud.DrawWorldOverlays(hud);                         // selection brackets + health bars
    m_hud.DrawCommandHud(hud);                            // overview contact list (M3 area G)

    // Darwinia windowed UI (Main Menu / Options / settings panels + dropdowns).
    m_menu.Draw(w, h, m_fps);

    // Non-modal connection-status banner (hidden once connected) — on top of the HUD.
    m_hud.DrawConnectionBanner(hud);

    // Right-click command menu — topmost so it sits over the HUD and windowed UI.
    m_hud.DrawContextMenu(hud);

#ifdef _DEBUG
    // Server-status overlay (F3, debug builds only) — on top of everything else.
    DrawServerStatusOverlay(w, h);
#endif

    m_canvas.Render(cl, w, h);

    m_dr.EndFrame();
  }


#ifdef _DEBUG
  // Server-status overlay (F3, debug builds only, §21): a translucent panel pinned
  // top-right listing the live server status polled from the diagnostic port. The
  // ServerStatusClient parses the JSON reply into the display lines drawn here.
  void DrawServerStatusOverlay(UINT screenW, UINT screenH)
  {
    if (!m_showServerStatus) return;
    const float s     = (screenH > 0 ? static_cast<float>(screenH) : 1080.f) / 1080.f;
    const float ts    = s * 0.85f;
    const float lineH = m_canvas.TextHeight(ts) + 4.f * s;
    const float pad   = 10.f * s;

    // Parsed status lines, or a single hint line if no reply has arrived yet.
    static const std::vector<std::string> kWaiting{
        "SERVER STATUS", "(no data - is server.statusPort set?)" };
    const std::vector<std::string>& lines =
        (m_statusClient && m_statusClient->Valid()) ? m_statusClient->Lines() : kWaiting;

    // Size the panel from the widest line.
    float maxW = 0.f;
    for (const auto& ln : lines)
      maxW = std::max(maxW, m_canvas.TextWidth(ln.c_str(), ts));
    const float panelW = maxW + 2.f * pad;
    const float panelH = lineH * static_cast<float>(lines.size()) + 2.f * pad;
    const float x = static_cast<float>(screenW) - panelW - 12.f * s;
    const float y = 12.f * s;

    m_canvas.DrawRect(x, y, panelW, panelH, 0.04f, 0.06f, 0.09f, 0.86f);
    const float bw = std::max(1.f, 1.f * s);
    m_canvas.DrawRect(x, y, panelW, bw, 0.35f, 0.85f, 1.0f, 0.9f);              // top accent
    m_canvas.DrawRect(x, y + panelH - bw, panelW, bw, 0.35f, 0.85f, 1.0f, 0.9f); // bottom accent

    float ly = y + pad;
    bool  first = true;
    for (const auto& ln : lines)
    {
      // Title row in cyan; value rows in pale text.
      if (first) m_canvas.DrawText(x + pad, ly, ln.c_str(), 0.35f, 0.85f, 1.0f, ts);
      else       m_canvas.DrawText(x + pad, ly, ln.c_str(), 0.85f, 0.90f, 0.92f, ts);
      first = false;
      ly += lineH;
    }
  }
#endif

  // ── Input handlers ───────────────────────────────────────────────────────
  void OnKeyDown(const Windows::UI::Core::CoreWindow& win, const Windows::UI::Core::KeyEventArgs& args)
  {
    using VK = Windows::System::VirtualKey;
    const VK key = args.VirtualKey();

    // Esc closes the active (frontmost) window; the previous one becomes active.
    if (key == VK::Escape) { m_menu.CloseTopWindow(); return; }

#ifdef _DEBUG
    // F3 toggles the server-status overlay (debug builds only, §21).
    if (key == VK::F3) { m_showServerStatus = !m_showServerStatus; return; }
#endif

    // --- fleet command hotkeys (§23.2 desktop affordances; M3 area G) ---
    const bool ctrl =
        (win.GetKeyState(VK::Control) & Windows::UI::Core::CoreVirtualKeyStates::Down) ==
        Windows::UI::Core::CoreVirtualKeyStates::Down;

    // Ctrl+# set / # recall control groups (0..9).
    if (key >= VK::Number0 && key <= VK::Number9) {
      const int group = static_cast<int>(key) - static_cast<int>(VK::Number0);
      if (ctrl) m_fleet.SetControlGroup(group);
      else      m_fleet.RecallControlGroup(group);
      return;
    }

    switch (key) {
    case VK::A: // select all own ships (smart-select)
      m_fleet.SelectAllOwnShips(m_session.get(), m_interp.curr);
      return;
    case VK::B: // enqueue a ship build at the player's base
      m_fleet.SendBuild(m_session.get());
      return;
    case VK::S: // stop the current selection
      m_fleet.SendStop(m_session.get());
      return;
    case VK::F: // toggle base-follow camera
      m_camera.SetFollow(!m_camera.Follow());
      return;
    case VK::Space: // recenter on the player's base (re-enables follow)
      m_camera.SetFollow(true);
      return;
    default:
      return;
    }
  }


  void OnClosed(const Windows::UI::Core::CoreWindow&, const Windows::UI::Core::CoreWindowEventArgs&)
  {
    // Tell the server we're leaving before the loop unwinds (UWP may not call
    // Uninitialize on a window close). The 8 s server idle timeout is the backstop.
    if (m_session)
      m_session->Disconnect();
    m_running = false;
  }
};

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) { Windows::ApplicationModel::Core::CoreApplication::Run(winrt::make<App>()); }
