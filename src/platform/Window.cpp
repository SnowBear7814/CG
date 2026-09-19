#include "Window.h"

#include <algorithm>
#include <windowsx.h>

namespace {
constexpr wchar_t kWindowClassName[] = L"CGDx12WindowClass";
}

bool Window::Create(HINSTANCE instance, int width, int height, const std::wstring& title) {
    m_instance = instance;
    m_width = static_cast<UINT>(width);
    m_height = static_cast<UINT>(height);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kWindowClassName;

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    RECT rect{0, 0, width, height};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    m_hwnd = CreateWindowExW(
        0,
        kWindowClassName,
        title.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr,
        nullptr,
        instance,
        this);

    if (!m_hwnd) {
        return false;
    }

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);

    RECT client{};
    GetClientRect(m_hwnd, &client);
    m_width = static_cast<UINT>((std::max)(1L, client.right - client.left));
    m_height = static_cast<UINT>((std::max)(1L, client.bottom - client.top));
    return true;
}

void Window::Destroy() {
    EndMouseLook();
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

void Window::SetTitle(const std::wstring& title) {
    if (m_hwnd) {
        SetWindowTextW(m_hwnd, title.c_str());
    }
}

void Window::SetResizeCallback(ResizeCallback callback) {
    m_resizeCallback = std::move(callback);
}

void Window::SetMouseLookCallback(MouseLookCallback callback) {
    m_mouseLookCallback = std::move(callback);
}

void Window::BeginMouseLook(int x, int y) {
    m_lastMousePos.x = x;
    m_lastMousePos.y = y;
    m_skipNextMouseLook = true;
    m_mouseLookActive = true;
    SetCapture(m_hwnd);
}

void Window::EndMouseLook() {
    if (m_mouseLookActive) {
        m_mouseLookActive = false;
        ReleaseCapture();
    }
}

void Window::ProcessMessages() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            Destroy();
            return;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

LRESULT CALLBACK Window::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Window* window = nullptr;
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        window = static_cast<Window*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
        window->m_hwnd = hwnd;
    } else {
        window = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (window) {
        return window->HandleMessage(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT Window::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            m_width = static_cast<UINT>(LOWORD(lParam));
            m_height = static_cast<UINT>(HIWORD(lParam));
            if (m_resizeCallback && m_width > 0 && m_height > 0) {
                m_resizeCallback(m_width, m_height);
            }
        }
        return 0;

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        BeginMouseLook(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        EndMouseLook();
        return 0;

    case WM_MOUSEMOVE:
        if (m_mouseLookActive && (wParam & (MK_LBUTTON | MK_RBUTTON)) != 0) {
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            if (m_skipNextMouseLook) {
                m_skipNextMouseLook = false;
            } else if (m_mouseLookCallback) {
                const float dx = static_cast<float>(x - m_lastMousePos.x);
                const float dy = static_cast<float>(y - m_lastMousePos.y);
                m_mouseLookCallback(dx, dy);
            }
            m_lastMousePos.x = x;
            m_lastMousePos.y = y;
        }
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            EndMouseLook();
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(m_hwnd, msg, wParam, lParam);
    }
}
