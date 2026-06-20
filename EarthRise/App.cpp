// App.cpp — EarthRise UWP client shell (M1b tech slice).
//
// IFrameworkView game loop:
//   SetWindow → DeviceResources::Initialize
//   Run       → BeginFrame / session.Tick / InterpBuffer / SceneRenderer / CanvasRenderer / EndFrame
//
// Uses NeuronClient (session + ReplicaManager + InterpBuffer) and
// NeuronRender (DeviceResources + SceneRenderer + CanvasRenderer).
//
// M1b: CngCrypto + WinsockSocket injected; three bots connect from ERHeadless.
// No int64_t propagated to the GPU — all world positions pass through
// FloatingOriginHelper before reaching SceneRenderer.

#include "pch.h"

// NeuronRender
#include "DeviceResources.h"
#include "SceneRenderer.h"
#include "CanvasRenderer.h"

// NeuronClient
#include "SessionImpl.h"
#include "ReplicaManager.h"
#include "Interpolator.h"

// NeuronCore platform impls (compiled into NeuronClient.lib / accessible via link)
#include "CngCrypto.h"
#include "WinsockSocket.h"
#include "Protocol.h"
#include "Snapshot.h"

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────
static constexpr uint16_t kServerPort = 7777;
static constexpr float kInterpAlpha = 0.0f; // M1b: snap-on-ack (alpha unused)

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
  std::unique_ptr<Neuron::Net::WinsockSocket> m_socket;
  Neuron::Net::EcPubKey m_pinnedPub{}; // zeroed = dev/test
  std::unique_ptr<Neuron::Client::SessionImpl> m_session;
  Neuron::Client::ReplicaManager m_replica;
  Neuron::Client::InterpBuffer m_interp;

  // ---- state ----
  std::array<uint8_t, 4096> m_snapBuf{};
  bool m_running{true};

  // ── IFrameworkViewSource ─────────────────────────────────────────────────
  Windows::ApplicationModel::Core::IFrameworkView CreateView() { return *this; }

  // ── IFrameworkView ───────────────────────────────────────────────────────
  void Initialize(const Windows::ApplicationModel::Core::CoreApplicationView&)
  {
    // Winsock init happens inside WinsockSocket constructor.
    m_socket = std::make_unique<Neuron::Net::WinsockSocket>();
    m_socket->Bind(0); // ephemeral port

    m_session = std::make_unique<Neuron::Client::SessionImpl>(&m_crypto, m_pinnedPub, m_socket.get(), "player1");
  }

  void SetWindow(const Windows::UI::Core::CoreWindow& window)
  {
    auto bounds = window.Bounds();
    const UINT w = static_cast<UINT>(bounds.Width);
    const UINT h = static_cast<UINT>(bounds.Height);

    check_bool(m_dr.Initialize(winrt::get_unknown(window), w, h));
    check_bool(m_scene.Initialize(&m_dr));
    check_bool(m_canvas.Initialize(&m_dr));

    // Start connecting to ERServer.
    m_session->Connect("127.0.0.1", kServerPort);

    window.KeyDown({this, &App::OnKeyDown});
    window.Closed({this, &App::OnClosed});
  }

  void Load(const hstring&) {}

  void Run()
  {
    auto window = Windows::UI::Core::CoreWindow::GetForCurrentThread();
    window.Activate();

    while (m_running)
    {
      window.Dispatcher().ProcessEvents(Windows::UI::Core::CoreProcessEventsOption::ProcessAllIfPresent);

      Tick();
    }
  }

  void Uninitialize()
  {
    m_canvas.Uninitialize();
    m_session.reset();
    m_socket.reset();
  }

  // ── Game tick ────────────────────────────────────────────────────────────
  void Tick()
  {
    // 1. Network I/O.
    m_session->Tick();

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
    const XMVECTOR eye = XMVectorSet(0.f, 50.f, -120.f, 0.f);
    const XMVECTOR at = XMVectorSet(0.f, 0.f, 0.f, 0.f);
    const XMVECTOR up = XMVectorSet(0.f, 1.f, 0.f, 0.f);
    constexpr float fov = XM_PIDIV4;
    const float asp = (h > 0) ? static_cast<float>(w) / static_cast<float>(h) : 1.f;

    XMMATRIX view = XMMatrixLookAtRH(eye, at, up);
    XMMATRIX proj = XMMatrixPerspectiveFovRH(fov, asp, 0.1f, 10000.f);
    // Transpose so columns match HLSL column-major layout expected by root constants.
    XMMATRIX viewProjT = XMMatrixTranspose(view * proj);

    float vpf[16];
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(vpf), viewProjT);

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
      se.scale = (re.entityType == 0) ? 4.f : 1.f; // base = larger
      // Darwinia palette: base = neon blue, ship = neon orange.
      if (re.entityType == 0)
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

  void OnClosed(const Windows::UI::Core::CoreWindow&, const Windows::UI::Core::CoreWindowEventArgs&) { m_running = false; }
};

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) { Windows::ApplicationModel::Core::CoreApplication::Run(winrt::make<App>()); }
