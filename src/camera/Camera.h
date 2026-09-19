#pragma once

#include <DirectXMath.h>

// Camera math/controls aligned with PCG-main ObjTexturesDemoApp (Labs 1–2).
class Camera {
public:
    // PCG: kCameraFovYRad = 0.25 * PI → 45°
    static constexpr float kFovYRadians = 0.25f * DirectX::XM_PI;
    static constexpr float kNearZ = 0.5f;
    static constexpr float kFarZ = 15000.0f;
    static constexpr float kDefaultSpeed = 280.0f;
    static constexpr float kBoostMultiplier = 2.5f;
    static constexpr float kMouseSensitivity = 0.0022f;

    void SetPosition(DirectX::XMFLOAT3 position);
    void SetYawPitch(float yawRadians, float pitchRadians);

    // PCG mouse: yaw += dx, pitch -= dy (dx/dy already include sensitivity)
    void AddYawPitch(float deltaYaw, float deltaPitch);

    void MoveLocal(float forward, float right, float up);

    DirectX::XMMATRIX GetViewMatrix() const;
    DirectX::XMMATRIX GetProjectionMatrix(float aspectRatio) const;

    DirectX::XMVECTOR ForwardNormalized() const;
    DirectX::XMVECTOR RightNormalized() const;

    DirectX::XMFLOAT3 GetPosition() const { return m_position; }
    float GetYaw() const { return m_yaw; }
    float GetPitch() const { return m_pitch; }

    float moveSpeed = kDefaultSpeed;
    float boostMultiplier = kBoostMultiplier;
    float mouseSensitivity = kMouseSensitivity;

private:
    DirectX::XMFLOAT3 m_position{0.0f, 1.5f, -4.0f};
    float m_yaw = 0.0f;   // 0 → look +Z (PCG)
    float m_pitch = 0.0f;
};
