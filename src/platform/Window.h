#pragma once

#include <Windows.h>
#include <functional>
#include <string>

class Window {
public:
    using ResizeCallback = std::function<void(UINT width, UINT height)>;
    // Client-space pixel deltas while LMB/RMB held (PCG-style mouse look).
    using MouseLookCallback = std::function<void(float dxPixels, float dyPixels)>;

    bool Create(HINSTANCE instance, int width, int height, const std::wstring& title);
    void Destroy();

    HWND GetHwnd() const { return m_hwnd; }
    UINT GetWidth() const { return m_width; }
    UINT GetHeight() const { return m_height; }
    bool IsOpen() const { return m_hwnd != nullptr && IsWindow(m_hwnd); }

    void SetTitle(const std::wstring& title);
    void SetResizeCallback(ResizeCallback callback);
    void SetMouseLookCallback(MouseLookCallback callback);

    bool IsMouseLookActive() const { return m_mouseLookActive; }

    void ProcessMessages();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void BeginMouseLook(int x, int y);
    void EndMouseLook();

    HWND m_hwnd = nullptr;
    HINSTANCE m_instance = nullptr;
    UINT m_width = 0;
    UINT m_height = 0;
    bool m_mouseLookActive = false;
    bool m_skipNextMouseLook = false;
    POINT m_lastMousePos{};
    ResizeCallback m_resizeCallback;
    MouseLookCallback m_mouseLookCallback;
};
