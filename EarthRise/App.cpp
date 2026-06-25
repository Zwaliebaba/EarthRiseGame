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

  // UI chrome textures (Darwinia menu skins, docs/design/darwinia-menu-ui.md).
  Neuron::Render::TextureGpu m_uiGrey; // InterfaceGrey — title bars, highlight beam
  Neuron::Render::TextureGpu m_uiRed;  // InterfaceRed  — buttons, window body
  bool m_uiReady{false};

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
  // Left-drag selection state (playable slice).
  bool  m_selDragging{false};
  float m_selX0{0.f}, m_selY0{0.f};
  static constexpr float kCamYawPerPx   = 0.005f;
  static constexpr float kCamPitchPerPx = 0.005f;
  static constexpr float kCamPanFrac    = 0.02f; // pan step as a fraction of zoom distance

  // ---- windowed UI (docs/design/darwinia-menu-ui.md) ----
  enum Win { Win_MainMenu, Win_Options, Win_Screen, Win_Graphics, Win_Other, Win_Count };
  bool  m_winOpen[Win_Count]{ false, false, false, false, false };
  float m_winX[Win_Count]{}, m_winY[Win_Count]{};
  bool  m_uiPlaced{false};
  int   m_dragWin{-1};
  float m_dragDX{0.f}, m_dragDY{0.f};
  int   m_hoverWin{-1}, m_hoverItem{-1}; // item = button index or panel row
  int   m_pressWin{-1}, m_pressItem{-1};
  int   m_ddWin{-1}, m_ddRow{-1};        // open dropdown (window, row); -1 none
  int   m_ddHover{-1};                    // hovered popup row
  int   m_sel[Win_Count][16]{};           // dropdown selection per panel row
  int   m_zorder[Win_Count]{ Win_MainMenu, Win_Options, Win_Screen, Win_Graphics, Win_Other }; // back→front
  UINT  m_lastW{0}, m_lastH{0}; // last screen size (for window cascade clamping)
  float m_fps{0.f};
  // Live-applied settings state (area G).
  float m_particleDensity{1.0f}; // scales emitter rate + ambient field
  float m_uiScaleMul{1.0f};      // "Large Menus" HUD scale multiplier

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
  Neuron::Client::ControlGroups m_controlGroups;
  std::vector<uint32_t> m_selection;     // locally-selected owned net ids
  uint32_t m_targetNetId{0};             // last picked target (overview/radar)
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
    InitSettings(); // default selections, then restore any persisted ones
    LoadSettings();
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
    // Anchor the orbit so the first frame of a right-drag has a zero delta.
    if (m_rmbDown) { m_prevPtrX = m_ptrX; m_prevPtrY = m_ptrY; }
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
    m_ptrDown = props.IsLeftButtonPressed();
    m_rmbDown = props.IsRightButtonPressed();
    m_ptrReleased = true;
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
  void LoadUiAssets()
  {
    if (auto dds = LoadPackagedAsset(L"Assets\\Fonts\\EditorFont-ENG.dds"); !dds.empty())
    {
      auto font = Neuron::Render::DdsLoader::Load(m_dr.Device(), dds);
      if (font.valid())
        m_canvas.SetFont(std::move(font), er::format::FontAtlasConfig{}); // 256x224, 16x16, cp32
    }
    if (auto dds = LoadPackagedAsset(L"Assets\\Textures\\InterfaceGrey.dds"); !dds.empty())
      m_uiGrey = Neuron::Render::DdsLoader::Load(m_dr.Device(), dds);
    if (auto dds = LoadPackagedAsset(L"Assets\\Textures\\InterfaceRed.dds"); !dds.empty())
      m_uiRed = Neuron::Render::DdsLoader::Load(m_dr.Device(), dds);

    m_uiReady = m_canvas.hasFont() && m_uiGrey.valid() && m_uiRed.valid();
    char buf[96];
    std::snprintf(buf, sizeof(buf), "[EarthRise] UI assets: font=%d grey=%d red=%d\n",
                  static_cast<int>(m_canvas.hasFont()), static_cast<int>(m_uiGrey.valid()),
                  static_cast<int>(m_uiRed.valid()));
    OutputDebugStringA(buf);
  }

  using Rect = Neuron::Render::Ui::Rect;
  struct UiRow { const char* label; const char* const* opts; int optCount; };

  float MenuScale(UINT screenH) const noexcept
  {
    return (screenH > 0 ? static_cast<float>(screenH) : 1080.f) / 1080.f * m_uiScaleMul;
  }

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

  static int Clamp(int v, int n) { return v < 0 ? 0 : (v >= n ? n - 1 : v); }
  float SettingFovRadians() const
  {
    static const float deg[4] = { 60.f, 75.f, 90.f, 110.f };
    return deg[Clamp(m_sel[Win_Graphics][0], 4)] * 0.01745329f;
  }

  // Push the current selections to the live engine knobs (called each frame).
  void ApplySettings()
  {
    m_dr.SetVSync(m_sel[Win_Screen][2] != 0);

    static const float bloom[4] = { 0.f, 0.6f, 1.1f, 1.7f };
    m_post.SetBloomIntensity(bloom[Clamp(m_sel[Win_Graphics][1], 4)]);
    m_post.SetPixelEffect(m_sel[Win_Graphics][6] != 0);

    static const float dens[4] = { 0.f, 0.33f, 0.66f, 1.0f };
    m_particleDensity = dens[Clamp(m_sel[Win_Graphics][2], 4)];
    m_particles.SetDensity(m_particleDensity);

    m_uiScaleMul = (m_sel[Win_Other][5] != 0) ? 1.3f : 1.0f; // Large Menus
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

  // Per-frame UI interaction. Run before DrawUi; consumes pointer edges.
  void UpdateUi(UINT screenW, UINT screenH)
  {
    m_lastW = screenW; m_lastH = screenH;
    if (!m_uiPlaced) { PlaceWindows(screenW, screenH); m_uiPlaced = true; }
    if (!m_uiReady) { m_ptrPressed = m_ptrReleased = false; return; }
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
        if (m_ddHover >= 0) { m_sel[m_ddWin][m_ddRow] = m_ddHover; PlayUi(m_clipSelect); }
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
          if (L.closeBox.Contains(m_ptrX, m_ptrY)) { m_winOpen[win] = false; PlayUi(m_clipClick); }
          else if (L.footerClose.Contains(m_ptrX, m_ptrY)) { m_winOpen[win] = false; PlayUi(m_clipClick); }
          else if (L.footerApply.Contains(m_ptrX, m_ptrY)) { SaveSettings(); PlayUi(m_clipClick); }
          else if (m_hoverItem >= 0) { m_ddWin = win; m_ddRow = m_hoverItem; PlayUi(m_clipSelect); }
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

  void OnButtonAction(int win, int idx, int count)
  {
    PlayUi(m_clipClick);
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
      else if (idx == 5) m_running = false;         // Quit EarthRise
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

  // Radar IFF colour per entity kind.
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

  // 2D radar disc (bottom-left): top-down blips of nearby entities relative to
  // the camera focus, with range rings (masterplan §22).
  void DrawRadar(UINT screenW, UINT screenH, const Neuron::Render::SceneEntity* ents, UINT count,
                 float fx, float fy, float fz)
  {
    (void)screenW; (void)fy;
    if (!m_uiReady) return;
    const float s = MenuScale(screenH);
    const float R = 95.f * s;
    const float cxr = 20.f * s + R;
    const float cyr = static_cast<float>(screenH) - 20.f * s - R;
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

  // --- fleet command interaction (§23.1/§23.4; M3 area G) --------------------

  // Encode + send a validated FleetCommand to the server (the server re-checks
  // ownership/target; this is purely the client request path, §8.4).
  void SendFleet(const Neuron::Sim::FleetCommand& cmd)
  {
    if (!m_session || m_selection.empty()) return;
    m_session->SendFleetCommand(Neuron::Sim::EncodeFleetCommand(cmd));
  }

  // Re-select all of the local player's own ships from the current replica (the
  // overview "all ships" smart-select, §23.1). Bases are not commandable units.
  void SelectAllOwnShips()
  {
    m_selection.clear();
    const uint32_t self = m_session ? m_session->PlayerNetId() : 0;
    const auto& rs = m_interp.curr;
    for (uint32_t i = 0; i < rs.count; ++i) {
      const auto& e = rs.entities[i];
      if (e.valid && e.ownerPlayer == self &&
          static_cast<Neuron::Sim::EntityKind>(e.entityType) == Neuron::Sim::EntityKind::Ship)
        m_selection.push_back(e.networkId);
    }
  }

  // True if the pointer is over any open windowed-UI panel (so a click there
  // should not also reach the radar/HUD beneath it). Mirrors UpdateUi's hit-test.
  bool PointerOverOpenWindow(float s) const
  {
    for (int win = 0; win < Win_Count; ++win) {
      if (!m_winOpen[win]) continue;
      if (WinIsPanel(win)) {
        const UiRow* rows; const int dd = WinRows(win, rows);
        const bool hasLabel = WinLabel(win) != nullptr;
        const auto L = Neuron::Render::Ui::BuildPanel(m_winX[win], m_winY[win], s, PanelWidth(s),
                                                      dd + (hasLabel ? 1 : 0), true);
        if (L.window.Contains(m_ptrX, m_ptrY)) return true;
      } else {
        const char* const* items; const int n = WinButtons(win, items);
        const auto L = Neuron::Render::Ui::BuildMainMenu(m_winX[win], m_winY[win], s, n);
        if (L.window.Contains(m_ptrX, m_ptrY)) return true;
      }
    }
    return false;
  }

  // Resolve a radar click into a smart action against the nearest contact (or
  // empty space → move). Mirrors DrawRadar's projection so the pick lines up.
  void HandleRadarCommand(UINT screenH, float fx, float fz)
  {
    if (!m_ptrPressed || m_selection.empty()) return;
    if (PointerOverOpenWindow(MenuScale(screenH))) return; // UI window owns this click
    const float s = MenuScale(screenH);
    const float R = 95.f * s;
    const float cxr = 20.f * s + R;
    const float cyr = static_cast<float>(screenH) - 20.f * s - R;
    constexpr float kRange = 1800.f;

    const float ddx = m_ptrX - cxr, ddy = m_ptrY - cyr;
    if (ddx * ddx + ddy * ddy > R * R) return; // click outside the radar disc

    // World point under the click (radar +X right, +Z up).
    const float wx = fx + (ddx / R) * kRange;
    const float wz = fz - (ddy / R) * kRange;

    // Nearest contact to the click (in render space) within a small pick radius.
    const uint32_t self = m_session ? m_session->PlayerNetId() : 0;
    const auto& rs = m_interp.curr;
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

    // Radar picks are entity-targeted: a click within ~120 m of a contact issues
    // the smart action on it. An empty-space *move* needs an absolute UniversePos
    // (the radar only has render-space, with no render→universe inverse here), so
    // empty clicks are ignored — precise destinations come from the starmap /
    // overview, which carry world coordinates (M3; a render→universe unproject is a
    // small follow-up).
    using namespace Neuron::Client;
    if (bestId == 0 || bestD2 >= 120.f * 120.f) return;
    const SmartTarget tt = ClassifyTarget(bestKind, bestOwner, self);
    m_targetNetId = bestId;
    // Beacon jumps need the beacon *name* (server keys jumps by name) — the radar
    // doesn't carry it, so those go through the starmap UI. Skip beacon picks here.
    if (tt == SmartTarget::Beacon) return;
    SendFleet(MakeSmartCommand(tt, m_selection, bestId, {}, /*queue*/false));
  }

  // Left-click / drag-box selection in the 3D viewport (playable slice). Projects
  // owned units to screen with the same view-proj the scene drew, then resolves a
  // click (nearest unit) or a drag (box) via the platform-independent Picking helper.
  // Runs after the radar handler and before UpdateUi consumes the pointer edge, and
  // ignores clicks over the radar disc or an open UI window. NOTE: the CPU projection
  // mirrors the renderer's column-major cbuffer convention (XMVector3Transform with
  // the un-transposed view-proj) — verify the pick lines up on the Windows build.
  void HandleViewportSelection(UINT screenW, UINT screenH, const DirectX::XMFLOAT4X4& vpf)
  {
    using namespace DirectX;
    const float s = MenuScale(screenH);
    const float R = 95.f * s, cxr = 20.f * s + R, cyr = static_cast<float>(screenH) - 20.f * s - R;
    auto overRadar = [&](float x, float y) {
      const float dx = x - cxr, dy = y - cyr; return dx * dx + dy * dy <= R * R;
    };

    if (m_ptrPressed && !PointerOverOpenWindow(s) && !overRadar(m_ptrX, m_ptrY))
    {
      m_selDragging = true; m_selX0 = m_ptrX; m_selY0 = m_ptrY;
    }
    if (!m_selDragging || !m_ptrReleased) return;
    m_selDragging = false;

    const uint32_t self = m_session ? m_session->PlayerNetId() : 0;
    if (self == 0) return;

    // Project owned ships + base to screen (same vpf the scene was rendered with).
    const XMMATRIX vp = XMLoadFloat4x4(&vpf);
    std::vector<Neuron::Client::ScreenPoint> pts;
    const Neuron::Client::ReplicaSet& rs = m_interp.curr;
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
        sp.x = (ndcx * 0.5f + 0.5f) * static_cast<float>(screenW);
        sp.y = (1.f - (ndcy * 0.5f + 0.5f)) * static_cast<float>(screenH);
        sp.visible = ndcx >= -1.f && ndcx <= 1.f && ndcy >= -1.f && ndcy <= 1.f;
      }
      pts.push_back(sp);
    }

    if (Neuron::Client::IsDrag(m_selX0, m_selY0, m_ptrX, m_ptrY))
    {
      Neuron::Client::PickBox(pts, m_selX0, m_selY0, m_ptrX, m_ptrY, m_selection);
    }
    else
    {
      const uint32_t hit = Neuron::Client::PickNearest(pts, m_ptrX, m_ptrY, 28.f * s);
      m_selection.clear();
      if (hit) m_selection.push_back(hit); // empty click clears selection
    }
  }

  // Project a render-space point to screen pixels via the cached view-proj. Returns
  // false when behind the camera or off-screen. Mirrors the renderer's column-major
  // cbuffer convention (un-transposed view-proj) — verify alignment on Windows.
  bool ProjectToScreen(float x, float y, float z, UINT screenW, UINT screenH,
                       float& sx, float& sy) const
  {
    using namespace DirectX;
    const XMMATRIX vp = XMLoadFloat4x4(&m_viewProj);
    const XMVECTOR clip = XMVector3Transform(XMVectorSet(x, y, z, 1.f), vp);
    const float wc = XMVectorGetW(clip);
    if (wc <= 1e-4f) return false;
    const float ndcx = XMVectorGetX(clip) / wc, ndcy = XMVectorGetY(clip) / wc;
    if (ndcx < -1.f || ndcx > 1.f || ndcy < -1.f || ndcy > 1.f) return false;
    sx = (ndcx * 0.5f + 0.5f) * static_cast<float>(screenW);
    sy = (1.f - (ndcy * 0.5f + 0.5f)) * static_cast<float>(screenH);
    return true;
  }

  // In-world feedback (playable slice): a green bracket under each selected unit and
  // a health bar over every combat unit (own = green, hostile = red). Drawn in the
  // 2D HUD pass by projecting each replica to screen.
  void DrawWorldOverlays(UINT screenW, UINT screenH)
  {
    if (!m_uiReady) return;
    const uint32_t self = m_session ? m_session->PlayerNetId() : 0;
    const float s = (screenH > 0 ? static_cast<float>(screenH) : 1080.f) / 1080.f;
    const Neuron::Client::ReplicaSet& rs = m_interp.curr;
    for (uint32_t i = 0; i < rs.count; ++i)
    {
      const auto& e = rs.entities[i];
      if (!e.valid) continue;
      const auto kind = static_cast<Neuron::Sim::EntityKind>(e.entityType);
      const bool isMine = (e.ownerPlayer == self && self != 0);
      const bool isNpc = (kind == Neuron::Sim::EntityKind::NpcUnit);
      const bool selected =
          std::find(m_selection.begin(), m_selection.end(), e.networkId) != m_selection.end();
      const bool bar = Neuron::Client::ShowsHealthBar(kind);
      if (!selected && !bar) continue;

      float sx = 0.f, sy = 0.f;
      if (!ProjectToScreen(e.x, e.y, e.z, screenW, screenH, sx, sy)) continue;

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
        const float f = Neuron::Client::HealthFraction(kind, e.hp);
        if (f >= 0.f)
        {
          const float bw = 24.f * s, bh = 3.f * s, bx = sx - bw * 0.5f, by = sy - 18.f * s;
          m_canvas.DrawRect(bx, by, bw, bh, 0.f, 0.f, 0.f, 0.6f);
          const float r = isMine ? 0.3f : (isNpc ? 0.95f : 0.9f);
          const float g = isMine ? 0.85f : (isNpc ? 0.3f : 0.8f);
          const float b = isMine ? 0.4f : 0.3f;
          m_canvas.DrawRect(bx, by, bw * f, bh, r, g, b, 0.9f);
        }
      }
    }
  }

  // Minimal command HUD (§22.2/§22.3; M3 area G): the overview contact list (fog-
  // filtered, IFF-coloured, nearest-first) plus the current selection + target.
  void DrawCommandHud(UINT screenW, UINT screenH)
  {
    if (!m_uiReady) return;
    const float s = MenuScale(screenH);

    // Objective banner (playable-slice onboarding) — amber, top-left.
    if (const char* obj = m_onboarding.CurrentText(); obj && obj[0])
      m_canvas.DrawText(40.f * s, 18.f * s, obj, 1.0f, 0.85f, 0.4f, s * 0.95f);

    // Live drag-select rectangle (playable slice).
    if (m_selDragging && m_ptrDown)
    {
      const float lx = m_selX0 < m_ptrX ? m_selX0 : m_ptrX;
      const float ly = m_selY0 < m_ptrY ? m_selY0 : m_ptrY;
      m_canvas.DrawRect(lx, ly, std::fabs(m_ptrX - m_selX0), std::fabs(m_ptrY - m_selY0),
                        0.4f, 0.8f, 1.0f, 0.15f);
    }
    const uint32_t self = m_session ? m_session->PlayerNetId() : 0;
    const auto overview = Neuron::Client::BuildOverview(m_interp.curr, self,
                                                        m_focus[0], m_focus[1], m_focus[2]);
    const float ts = s * 0.8f;
    float y = 20.f * s;
    const float x = static_cast<float>(screenW) - 230.f * s;
    char line[96];
    std::snprintf(line, sizeof(line), "OVERVIEW  sel:%u", static_cast<unsigned>(m_selection.size()));
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
                    (c.netId == m_targetNetId) ? '>' : ' ',
                    static_cast<unsigned>(c.netId), c.distance, c.hp);
      m_canvas.DrawText(x, y, line, r, g, b, ts);
      y += 12.f * s;
    }
  }

  void DrawUi(UINT screenW, UINT screenH)
  {
    (void)screenW;
    if (!m_uiReady) return;
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
          m_interp.curr, self, static_cast<uint32_t>(m_selection.size())));
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
    const float fov = SettingFovRadians();
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

    // Fleet command: a radar click issues a smart action against the nearest
    // contact (must run before UpdateUi consumes the pointer-press, §23.1/area G).
    m_focus[0] = cx; m_focus[1] = cy; m_focus[2] = cz;
    HandleRadarCommand(h, cx, cz);
    HandleViewportSelection(w, h, vpf); // 3D-viewport click / box selection

    // HUD + windowed UI. Interaction (hover/press/drag/dropdowns) runs first.
    UpdateUi(w, h);
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

    // 2D radar disc (under the windowed UI).
    DrawRadar(w, h, entities, entCount, cx, cy, cz);

    // In-world selection brackets + health bars (playable slice).
    DrawWorldOverlays(w, h);

    // Overview contact list + selection/target readout (M3 area G).
    DrawCommandHud(w, h);

    // Darwinia windowed UI (Main Menu / Options / settings panels + dropdowns).
    DrawUi(w, h);

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
    if (key == VK::Escape) { CloseTopWindow(); return; }

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
      if (ctrl) m_controlGroups.Set(group, m_selection);
      else      m_selection = m_controlGroups.Recall(group);
      return;
    }

    switch (key) {
    case VK::A: // select all own ships (smart-select)
      SelectAllOwnShips();
      return;
    case VK::B: { // enqueue a ship build at the player's base
      if (m_session) {
        Neuron::Sim::FleetCommand c; c.intent = Neuron::Sim::IntentType::Build;
        c.units = { m_session->PlayerNetId() };
        m_session->SendFleetCommand(Neuron::Sim::EncodeFleetCommand(c));
      }
      return;
    }
    case VK::S: { // stop the current selection
      Neuron::Sim::FleetCommand c; c.intent = Neuron::Sim::IntentType::Stop; c.units = m_selection;
      SendFleet(c);
      return;
    }
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

  // Close an open dropdown first; else close the frontmost open window; if
  // nothing is open, bring the Main Menu back.
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
