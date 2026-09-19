#pragma once

#include <d3d12.h>
#include <stdexcept>

inline void ThrowIfFailed(HRESULT hr, const char* message) {
    if (FAILED(hr)) {
        throw std::runtime_error(message);
    }
}

inline UINT AlignUp(UINT value, UINT alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}
