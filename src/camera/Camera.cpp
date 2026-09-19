#include "Camera.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

void Camera::SetPosition(XMFLOAT3 position) {
    m_position = position;
}

void Camera::SetYawPitch(float yawRadians, float pitchRadians) {
    m_yaw = yawRadians;
    m_pitch = pitchRadians;
}

void Camera::AddYawPitch(float deltaYaw, float deltaPitch) {
    m_yaw += deltaYaw;
    m_pitch += deltaPitch;

    constexpr float kPitchLimit = XM_PIDIV2 - 0.05f;
    m_pitch = std::clamp(m_pitch, -kPitchLimit, kPitchLimit);
}

XMVECTOR Camera::ForwardNormalized() const {
    // PCG ObjTexturesDemoApp::CameraForwardNormalized
    const float cp = std::cos(m_pitch);
    const float sp = std::sin(m_pitch);
    const float cy = std::cos(m_yaw);
    const float sy = std::sin(m_yaw);
    return XMVector3Normalize(XMVectorSet(sy * cp, sp, cy * cp, 0.0f));
}

XMVECTOR Camera::RightNormalized() const {
    static const XMVECTOR kWorldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    const XMVECTOR forward = ForwardNormalized();
    XMVECTOR right = XMVector3Cross(kWorldUp, forward);
    const float lenSq = XMVectorGetX(XMVector3LengthSq(right));
    if (lenSq < 1e-8f) {
        return XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
    }
    return XMVector3Normalize(right);
}

void Camera::MoveLocal(float forward, float right, float up) {
    static const XMVECTOR kWorldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    const XMVECTOR delta =
        ForwardNormalized() * forward +
        RightNormalized() * right +
        kWorldUp * up;

    const XMVECTOR position = XMLoadFloat3(&m_position) + delta;
    XMStoreFloat3(&m_position, position);
}

XMMATRIX Camera::GetViewMatrix() const {
    static const XMVECTOR kWorldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    const XMVECTOR eye = XMLoadFloat3(&m_position);
    const XMVECTOR target = eye + ForwardNormalized();
    return XMMatrixLookAtLH(eye, target, kWorldUp);
}

XMMATRIX Camera::GetProjectionMatrix(float aspectRatio) const {
    return XMMatrixPerspectiveFovLH(kFovYRadians, aspectRatio, kNearZ, kFarZ);
}
