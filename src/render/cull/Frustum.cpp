#include "Frustum.h"

#include <algorithm>
#include <cfloat>

using namespace DirectX;

void Frustum::ExtractFromMatrix(FXMMATRIX viewProjMatrix) {
    XMStoreFloat4x4(&viewProj, viewProjMatrix);
}

bool Frustum::IntersectsAabb(const XMFLOAT3& minW, const XMFLOAT3& maxW) const {
    if (minW.x > maxW.x || minW.y > maxW.y || minW.z > maxW.z) {
        return false;
    }

    const XMMATRIX clip = XMLoadFloat4x4(&viewProj);
    const XMFLOAT3 corners[8] = {
        {minW.x, minW.y, minW.z},
        {maxW.x, minW.y, minW.z},
        {minW.x, maxW.y, minW.z},
        {maxW.x, maxW.y, minW.z},
        {minW.x, minW.y, maxW.z},
        {maxW.x, minW.y, maxW.z},
        {minW.x, maxW.y, maxW.z},
        {maxW.x, maxW.y, maxW.z},
    };

    XMVECTOR clipCorners[8];
    for (int i = 0; i < 8; ++i) {
        clipCorners[i] = XMVector4Transform(
            XMVectorSet(corners[i].x, corners[i].y, corners[i].z, 1.0f),
            clip);
    }

    auto allOutside = [&](auto&& outside) {
        for (int i = 0; i < 8; ++i) {
            if (!outside(clipCorners[i])) {
                return false;
            }
        }
        return true;
    };

    // D3D LH clip: -w <= x <= w, -w <= y <= w, 0 <= z <= w
    if (allOutside([](XMVECTOR c) { return XMVectorGetX(c) + XMVectorGetW(c) < 0.0f; })) {
        return false;
    }
    if (allOutside([](XMVECTOR c) { return XMVectorGetX(c) - XMVectorGetW(c) > 0.0f; })) {
        return false;
    }
    if (allOutside([](XMVECTOR c) { return XMVectorGetY(c) + XMVectorGetW(c) < 0.0f; })) {
        return false;
    }
    if (allOutside([](XMVECTOR c) { return XMVectorGetY(c) - XMVectorGetW(c) > 0.0f; })) {
        return false;
    }
    if (allOutside([](XMVECTOR c) { return XMVectorGetZ(c) < 0.0f; })) {
        return false;
    }
    if (allOutside([](XMVECTOR c) { return XMVectorGetZ(c) - XMVectorGetW(c) > 0.0f; })) {
        return false;
    }

    return true;
}

void TransformAabb(
    const XMFLOAT3& localMin,
    const XMFLOAT3& localMax,
    FXMMATRIX world,
    XMFLOAT3& outMin,
    XMFLOAT3& outMax) {
    const XMFLOAT3 corners[8] = {
        {localMin.x, localMin.y, localMin.z},
        {localMax.x, localMin.y, localMin.z},
        {localMin.x, localMax.y, localMin.z},
        {localMax.x, localMax.y, localMin.z},
        {localMin.x, localMin.y, localMax.z},
        {localMax.x, localMin.y, localMax.z},
        {localMin.x, localMax.y, localMax.z},
        {localMax.x, localMax.y, localMax.z},
    };

    outMin = {FLT_MAX, FLT_MAX, FLT_MAX};
    outMax = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (const auto& c : corners) {
        XMFLOAT3 w{};
        XMStoreFloat3(&w, XMVector3TransformCoord(XMLoadFloat3(&c), world));
        outMin.x = (std::min)(outMin.x, w.x);
        outMin.y = (std::min)(outMin.y, w.y);
        outMin.z = (std::min)(outMin.z, w.z);
        outMax.x = (std::max)(outMax.x, w.x);
        outMax.y = (std::max)(outMax.y, w.y);
        outMax.z = (std::max)(outMax.z, w.z);
    }
}
