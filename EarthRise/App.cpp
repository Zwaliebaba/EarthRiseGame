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

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <winrt/Windows.ApplicationModel.h> // Package (install location)
#include <winrt/Windows.Graphics.Display.h> // DisplayInformation (DPI scale)
#include <winrt/Windows.Storage.h>          // StorageFolder::Path

// NeuronRender
#include "DeviceResources.h"
#include "SceneRenderer.h"
#include "CanvasRenderer.h"
#include "CmoLoader.h"

// NeuronClient
#include "SessionImpl.h"
#include "ReplicaManager.h"
#include "Interpolator.h"

// NeuronCore platform impls (compiled into NeuronClient.lib / accessible via link)
#include "CngCrypto.h"
#include "DatagramSocketAdapter.h"
#include "Protocol.h"
#include "Snapshot.h"

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────
static constexpr uint16_t kServerPort = 7777;
static constexpr float kInterpAlpha = 0.0f; // M1b: snap-on-ack (alpha unused)

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

  // ---- network ----
  Neuron::Net::CngCrypto m_crypto;
  std::unique_ptr<Neuron::Net::DatagramSocketAdapter> m_socket; // WinRT DatagramSocket (§8.1)
  Neuron::Net::EcPubKey m_pinnedPub{}; // zeroed = dev/test
  std::unique_ptr<Neuron::Client::SessionImpl> m_session;
  Neuron::Client::ReplicaManager m_replica;
  Neuron::Client::InterpBuffer m_interp;

  // ---- state ----
  std::array<uint8_t, 4096> m_snapBuf{};
  bool m_running{true};

  // ---- window / dpi / timing ----
  Windows::UI::Core::CoreWindow m_window{nullptr};
  float m_dpiScale{1.0f};
  std::chrono::steady_clock::time_point m_connectStart{};
  bool m_connectedLogged{false};
  float m_meshRadius{0.0f}; // loaded CMO bounding radius (0 = cube fallback)

  // ── IFrameworkViewSource ─────────────────────────────────────────────────
  Windows::ApplicationModel::Core::IFrameworkView CreateView() { return *this; }

  // ── IFrameworkView ───────────────────────────────────────────────────────
  void Initialize(const Windows::ApplicationModel::Core::CoreApplicationView&)
  {
    check_bool(m_crypto.Initialize());

    // WinRT DatagramSocket (§8.1) — no WSAStartup; binds asynchronously off the ASTA.
    m_socket = std::make_unique<Neuron::Net::DatagramSocketAdapter>();
    check_bool(m_socket->Open(0)); // ephemeral port

    m_session = std::make_unique<Neuron::Client::SessionImpl>(&m_crypto, m_pinnedPub, m_socket.get(), "player1");
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
    check_bool(m_scene.Initialize(&m_dr));
    check_bool(m_canvas.Initialize(&m_dr));
    {
      const long long initMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - initStart)
                                   .count();
      char buf[112];
      std::snprintf(buf, sizeof(buf), "[EarthRise] DX init %lld ms (backbuffer %ux%u, dpi %.2f)\n",
                    initMs, w, h, m_dpiScale);
      OutputDebugStringA(buf);
    }

    // Load the real CMO mesh (fail-safe: SceneRenderer keeps the cube if the
    // asset is missing or unparseable).
    if (auto cmo = LoadPackagedAsset(L"Assets\\Shapes\\Jumpgates\\Jumpgate.cmo"); !cmo.empty())
    {
      auto mesh = Neuron::Render::CmoLoader::Load(m_dr.Device(), cmo);
      char buf[128];
      std::snprintf(buf, sizeof(buf),
                    "[EarthRise] CMO load: %zu bytes, valid=%d verts=%u idx=%u radius=%.1f\n",
                    cmo.size(), static_cast<int>(mesh.valid()), mesh.vertexCount, mesh.indexCount,
                    mesh.boundingRadius);
      OutputDebugStringA(buf);
      if (mesh.valid())
        m_meshRadius = mesh.boundingRadius;
      m_scene.SetMesh(std::move(mesh));
    }
    else
    {
      OutputDebugStringA("[EarthRise] CMO load: asset not found (using placeholder cube)\n");
    }

    // Start connecting to ERServer.
    m_connectStart = std::chrono::steady_clock::now();
    m_session->Connect("127.0.0.1", kServerPort);

    // Keep the back buffer matched to the window across resize / maximize.
    window.SizeChanged([this](Windows::UI::Core::CoreWindow const&,
                              Windows::UI::Core::WindowSizeChangedEventArgs const&) { ApplyResize(); });
    window.KeyDown({this, &App::OnKeyDown});
    window.Closed({this, &App::OnClosed});
  }

  // Logical DIP -> physical pixels for the swap chain.
  UINT PhysPx(float dip) const noexcept { return static_cast<UINT>(dip * m_dpiScale + 0.5f); }

  void ApplyResize()
  {
    if (!m_window)
      return;
    const auto b = m_window.Bounds();
    m_dr.Resize(PhysPx(b.Width), PhysPx(b.Height));
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
    m_canvas.Uninitialize();
    if (m_session)
      m_session->Disconnect(); // best-effort graceful notice while the socket is alive
    m_session.reset();
    m_socket.reset();
  }

  // ── Game tick ────────────────────────────────────────────────────────────
  void Tick()
  {
    // 1. Network I/O.
    m_session->Tick();

    // One-shot: how long from Connect() to fully connected (handshake cost).
    if (!m_connectedLogged && m_session->GetState() == Neuron::Client::SessionState::Connected)
    {
      m_connectedLogged = true;
      const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - m_connectStart)
                               .count();
      char buf[80];
      std::snprintf(buf, sizeof(buf), "[EarthRise] connected in %lld ms\n", ms);
      OutputDebugStringA(buf);
    }

    // 2. Consume any new snapshots.
    size_t snapSize = 0;
    while (m_session->PollSnapshot(m_snapBuf, snapSize))
    {
      if (m_replica.Update(std::span<const uint8_t>(m_snapBuf.data(), snapSize), m_session->PlayerNetId()))
        m_interp.Advance(m_replica.Current());
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
    const XMVECTOR eye = XMVectorSet(cx, cy + 50.f, cz - 120.f, 0.f);
    const XMVECTOR at = XMVectorSet(cx, cy, cz, 0.f);
    const XMVECTOR up = XMVectorSet(0.f, 1.f, 0.f, 0.f);
    constexpr float fov = XM_PIDIV4;
    const float asp = (h > 0) ? static_cast<float>(w) / static_cast<float>(h) : 1.f;

    XMMATRIX view = XMMatrixLookAtRH(eye, at, up);
    XMMATRIX proj = XMMatrixPerspectiveFovRH(fov, asp, 0.1f, 10000.f);
    // SceneVS reads viewProj from a (HLSL-default) COLUMN-MAJOR cbuffer and
    // applies it as mul(viewProj, worldPos) (column-vector). Storing view*proj
    // row-major lets the column-major load reinterpret it as the transpose that
    // multiply needs — so we must NOT pre-transpose here. (Transposing in C++ as
    // well cancels out and corrupts the transform — the old "thin box" bug.)
    XMMATRIX viewProj = view * proj;

    float vpf[16];
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(vpf), viewProj);

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
      // entityType carries EntityKind (Components.h): Base = 1, Ship = 2.
      const bool isBase = (re.entityType == 1);
      // With a CMO mesh loaded, normalize by its bounding radius so the on-screen
      // size is sensible regardless of the model's native units (Jumpgate r~333);
      // otherwise use the unit-cube half-extents.
      const float targetMetres = isBase ? 40.f : 15.f;
      se.scale = (m_meshRadius > 0.f) ? (targetMetres / m_meshRadius) : (isBase ? 10.f : 3.f);
      // Darwinia palette: base = neon blue, ship = neon orange.
      if (isBase)
      {
        se.r = 0.2f;
        se.g = 0.6f;
        se.b = 1.0f;
      }
      else
      {
        se.r = 1.0f;
        se.g = 0.5f;
        se.b = 0.1f;
      }
      se.kind = re.entityType;
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

    m_scene.Render(cl, vpf, entities, entCount);

    // HUD.
    m_canvas.Reset();
    const Neuron::Client::SessionState st = m_session->GetState();
    const char* stateStr = (st == Neuron::Client::SessionState::Connected)
                             ? "CONNECTED"
                             : (st == Neuron::Client::SessionState::Handshaking)
                             ? "HANDSHAKE"
                             : (st == Neuron::Client::SessionState::Authenticating)
                             ? "AUTH"
                             : "OFFLINE";
    m_canvas.DrawText(10.f, 10.f, "EarthRise", 0.2f, 1.0f, 0.4f);
    m_canvas.DrawText(10.f, 28.f, stateStr, 0.8f, 0.8f, 0.2f);
    m_canvas.Render(cl, w, h);

    m_dr.EndFrame();
  }

  // ── Input handlers ───────────────────────────────────────────────────────
  void OnKeyDown(const Windows::UI::Core::CoreWindow&, const Windows::UI::Core::KeyEventArgs& args)
  {
    if (args.VirtualKey() == Windows::System::VirtualKey::Escape)
      m_running = false;
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
