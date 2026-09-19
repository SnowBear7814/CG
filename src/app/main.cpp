#include "Application.h"

#include <exception>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    try {
        Application app;
        return app.Run(instance);
    } catch (const std::exception& ex) {
        MessageBoxA(nullptr, ex.what(), "CG DX12 Error", MB_OK | MB_ICONERROR);
        return 1;
    }
}
