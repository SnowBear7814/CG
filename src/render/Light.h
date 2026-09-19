#pragma once

#include <DirectXMath.h>

#include <cstdint>

enum class LightType : uint32_t {
    Directional = 0,
    Point = 1,
    Spot = 2,
};

// GPU layout (must match deferred_lighting_pass.hlsl)
struct GpuLight {
    DirectX::XMFLOAT3 position;
    uint32_t type = static_cast<uint32_t>(LightType::Point);
    DirectX::XMFLOAT3 direction;
    float range = 100.0f;
    DirectX::XMFLOAT3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float spotInnerCos = 0.9f;
    float spotOuterCos = 0.7f;
    float padding0 = 0.0f;
    float padding1 = 0.0f;
};
