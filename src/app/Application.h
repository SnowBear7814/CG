#pragma once

#include "Camera.h"
#include "Renderer.h"
#include "Window.h"

#include <Windows.h>

class Application {
public:
    int Run(HINSTANCE instance);

private:
    void Update(float deltaSeconds);
    void Render();
    void UpdateTitle();
    void SetupCameraForModel(); // PCG FitCameraToScene-style

    Window m_window;
    Renderer m_renderer;
    Camera m_camera;

    bool m_keys[256]{};
    bool m_prevF5 = false;
    bool m_prevF3 = false;
    bool m_prevF4 = false;
    bool m_prevF6 = false;
    bool m_prevF7 = false;
    bool m_prevF8 = false;
    bool m_prevF9 = false;
    float m_timeSeconds = 0.0f;
    float m_deltaSeconds = 0.0f;
    LARGE_INTEGER m_frequency{};
    LARGE_INTEGER m_previousTime{};
};
