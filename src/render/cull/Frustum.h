#pragma once

#include <DirectXMath.h>

#include <algorithm>
#include <cfloat>

struct Aabb {
    DirectX::XMFLOAT3 Min{FLT_MAX, FLT_MAX, FLT_MAX};
    DirectX::XMFLOAT3 Max{-FLT_MAX, -FLT_MAX, -FLT_MAX};

    bool IsValid() const {
        return Min.x <= Max.x && Min.y <= Max.y && Min.z <= Max.z;
    }

    void Merge(const Aabb& other) {
        if (!other.IsValid()) {
            return;
        }
        if (!IsValid()) {
            *this = other;
            return;
        }
        Min.x = (std::min)(Min.x, other.Min.x);
        Min.y = (std::min)(Min.y, other.Min.y);
        Min.z = (std::min)(Min.z, other.Min.z);
        Max.x = (std::max)(Max.x, other.Max.x);
        Max.y = (std::max)(Max.y, other.Max.y);
        Max.z = (std::max)(Max.z, other.Max.z);
    }

    void Expand(float pad) {
        Min.x -= pad;
        Min.y -= pad;
        Min.z -= pad;
        Max.x += pad;
        Max.y += pad;
        Max.z += pad;
    }
};

// View-frustum vs world AABB (Lab 4). Clip-space AABB test.
struct Frustum {
    DirectX::XMFLOAT4X4 viewProj{};

    void ExtractFromMatrix(DirectX::FXMMATRIX viewProjMatrix);
    bool IntersectsAabb(const DirectX::XMFLOAT3& minW, const DirectX::XMFLOAT3& maxW) const;
    bool IntersectsAabb(const Aabb& box) const {
        return IntersectsAabb(box.Min, box.Max);
    }
};

void TransformAabb(
    const DirectX::XMFLOAT3& localMin,
    const DirectX::XMFLOAT3& localMax,
    DirectX::FXMMATRIX world,
    DirectX::XMFLOAT3& outMin,
    DirectX::XMFLOAT3& outMax);

inline void TransformAabb(
    const DirectX::XMFLOAT3& localMin,
    const DirectX::XMFLOAT3& localMax,
    DirectX::FXMMATRIX world,
    Aabb& out) {
    TransformAabb(localMin, localMax, world, out.Min, out.Max);
}
