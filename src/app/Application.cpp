#include "Application.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>

namespace {
void EnableDpiAwareness() {
    using SetDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        auto setContext = reinterpret_cast<SetDpiAwarenessContextFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (setContext) {
            setContext(reinterpret_cast<HANDLE>(-4)); // PER_MONITOR_AWARE_V2
            return;
        }
    }
    SetProcessDPIAware();
}
} // namespace

int Application::Run(HINSTANCE instance) {
    EnableDpiAwareness();

    constexpr int kWidth = 1280;
    constexpr int kHeight = 720;

    if (!m_window.Create(instance, kWidth, kHeight, L"CG DX12 — loading Sponza...")) {
        throw std::runtime_error("Failed to create window");
    }

    m_window.SetResizeCallback([this](UINT width, UINT height) {
        m_renderer.Resize(width, height);
    });

    // PCG-style mouse look: hold LMB/RMB, yaw += dx, pitch -= dy
    m_window.SetMouseLookCallback([this](float dxPixels, float dyPixels) {
        m_camera.AddYawPitch(
            m_camera.mouseSensitivity * dxPixels,
            -m_camera.mouseSensitivity * dyPixels);
    });

    RECT client{};
    GetClientRect(m_window.GetHwnd(), &client);
    const UINT clientW = static_cast<UINT>((std::max)(1L, client.right - client.left));
    const UINT clientH = static_cast<UINT>((std::max)(1L, client.bottom - client.top));

    if (!m_renderer.Initialize(m_window.GetHwnd(), clientW, clientH)) {
        throw std::runtime_error("Failed to initialize DX12 renderer");
    }

    const std::wstring sponzaPath = std::wstring(CONTENT_DIR) + L"/models/sponza/sponza.obj";
    const std::wstring rockPath = std::wstring(CONTENT_DIR) + L"/models/rock_07/rock_07.obj";
    const std::wstring skyboxPath = std::wstring(CONTENT_DIR) + L"/skybox/skybox.dds";
    m_renderer.LoadModel(sponzaPath);
    m_renderer.LoadRocks(rockPath);
    m_renderer.LoadSkybox(skyboxPath);
    SetupCameraForModel();

    QueryPerformanceFrequency(&m_frequency);
    QueryPerformanceCounter(&m_previousTime);

    while (m_window.IsOpen()) {
        m_window.ProcessMessages();
        if (!m_window.IsOpen()) {
            break;
        }

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const float deltaSeconds =
            static_cast<float>(now.QuadPart - m_previousTime.QuadPart) / static_cast<float>(m_frequency.QuadPart);
        m_previousTime = now;

        Update(deltaSeconds);
        Render();
        UpdateTitle();
    }

    m_renderer.Shutdown();
    m_window.Destroy();
    return 0;
}

void Application::SetupCameraForModel() {
    // PCG FitCameraToScene: stand south of courtyard, look +Z into the scene.
    const auto bmin = m_renderer.GetModelBoundsMin();
    const auto bmax = m_renderer.GetModelBoundsMax();
    const float cx = 0.5f * (bmin.x + bmax.x);
    const float cz = 0.5f * (bmin.z + bmax.z);
    const float floorY = bmin.y;
    const float cameraY = floorY + 8.0f;
    const float dist = 95.0f;

    m_camera.SetPosition({cx, cameraY, cz - dist});
    m_camera.SetYawPitch(0.0f, 0.12f);
    m_camera.moveSpeed = Camera::kDefaultSpeed;
    m_camera.boostMultiplier = Camera::kBoostMultiplier;
    m_camera.mouseSensitivity = Camera::kMouseSensitivity;
}

void Application::Update(float deltaSeconds) {
    for (int i = 0; i < 256; ++i) {
        m_keys[i] = (GetAsyncKeyState(i) & 0x8000) != 0;
    }

    // Lab 4: F5 frustum on/off, F3 octree vs linear (when frustum ON)
    const bool f5Down = m_keys[VK_F5];
    if (f5Down && !m_prevF5) {
        m_renderer.ToggleFrustumCulling();
    }
    m_prevF5 = f5Down;

    const bool f3Down = m_keys[VK_F3];
    if (f3Down && !m_prevF3) {
        m_renderer.ToggleOctreeCulling();
    }
    m_prevF3 = f3Down;

    const bool f4Down = m_keys[VK_F4];
    if (f4Down && !m_prevF4) {
        m_renderer.ToggleShadows();
    }
    m_prevF4 = f4Down;

    const bool f6Down = m_keys[VK_F6];
    if (f6Down && !m_prevF6) {
        m_renderer.ToggleParticles();
    }
    m_prevF6 = f6Down;

    const bool f7Down = m_keys[VK_F7];
    if (f7Down && !m_prevF7) {
        m_renderer.ToggleDof();
    }
    m_prevF7 = f7Down;

    const bool f8Down = m_keys[VK_F8];
    if (f8Down && !m_prevF8) {
        m_renderer.ToggleChromaticAberration();
    }
    m_prevF8 = f8Down;

    const bool f9Down = m_keys[VK_F9];
    if (f9Down && !m_prevF9) {
        m_renderer.TogglePbr();
    }
    m_prevF9 = f9Down;

    // PCG movement: W/S along look, A/D along right, Space/Ctrl up/down, Shift boost
    float forward = 0.0f;
    float right = 0.0f;
    float up = 0.0f;

    if (m_keys['W']) forward += 1.0f;
    if (m_keys['S']) forward -= 1.0f;
    if (m_keys['D']) right += 1.0f;
    if (m_keys['A']) right -= 1.0f;
    if (m_keys[VK_SPACE]) up += 1.0f;
    if (m_keys[VK_CONTROL]) up -= 1.0f;

    const float length = std::sqrt(forward * forward + right * right + up * up);
    if (length > 0.0f) {
        forward /= length;
        right /= length;
        up /= length;

        float speed = m_camera.moveSpeed;
        if (m_keys[VK_SHIFT]) {
            speed *= m_camera.boostMultiplier;
        }

        m_camera.MoveLocal(forward * speed * deltaSeconds, right * speed * deltaSeconds, up * speed * deltaSeconds);
    }

    m_deltaSeconds = deltaSeconds;
    m_timeSeconds += deltaSeconds;
}

void Application::Render() {
    if (!m_renderer.IsInitialized() || m_window.GetWidth() == 0 || m_window.GetHeight() == 0) {
        return;
    }

    m_renderer.DrawFrame(m_camera, m_timeSeconds, m_deltaSeconds);
}

void Application::UpdateTitle() {
    const auto pos = m_camera.GetPosition();
    std::wstringstream title;
    title.setf(std::ios::fixed);
    title.precision(1);
    title << L"CG DX12 | Lab1-8"
          << L" | rocks " << m_renderer.GetRocksDrawnLastFrame()
          << L"/" << m_renderer.GetRockInstanceCount()
          << L" | frustum " << (m_renderer.IsFrustumCullingEnabled() ? L"ON" : L"OFF")
          << L" (F5)"
          << L" | cull "
          << (m_renderer.IsFrustumCullingEnabled()
                  ? (m_renderer.IsOctreeCullingEnabled() ? L"octree" : L"linear")
                  : L"off")
          << L" (F3)"
          << L" | shadow " << (m_renderer.AreShadowsEnabled() ? L"ON" : L"OFF")
          << L" (F4)"
          << L" | particles " << (m_renderer.AreParticlesEnabled() ? L"ON" : L"OFF")
          << L" (F6)"
          << L" | DoF " << (m_renderer.IsDofEnabled() ? L"ON" : L"OFF")
          << L" (F7)"
          << L" | CA " << (m_renderer.IsChromaticAberrationEnabled() ? L"ON" : L"OFF")
          << L" (F8)"
          << L" | PBR " << (m_renderer.IsPbrEnabled() ? L"ON" : L"OFF")
          << L" (F9)"
          << L" | pos (" << pos.x << L", " << pos.y << L", " << pos.z << L")";
    m_window.SetTitle(title.str());
}
