#pragma once

#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>

// Lab 5: cascaded shadow maps (Texture2DArray) for one directional light.
class CascadedShadowMaps {
public:
    static constexpr UINT kCascadeCount = 4;
    static constexpr UINT kMapSize = 2048;

    void Create(ID3D12Device* device);
    void Shutdown();
    void Resize(ID3D12Device* device); // recreate if needed (same size for now)

    // Practical (log-uniform) split distances in view-space Z. outSplits[i] = far of cascade i.
    static void ComputePracticalSplits(
        float nearZ,
        float farZ,
        float lambda,
        std::array<float, kCascadeCount>& outSplits);

    // Fit orthographic light VP for each cascade slice of the camera frustum.
    void UpdateCascades(
        DirectX::FXMMATRIX cameraView,
        DirectX::FXMMATRIX cameraProj,
        DirectX::FXMVECTOR lightDirection,
        float nearZ,
        float cascadeFarZ,
        float lambda,
        const DirectX::XMFLOAT3& sceneMin,
        const DirectX::XMFLOAT3& sceneMax);

    void BeginCascade(ID3D12GraphicsCommandList* cmd, UINT cascadeIndex);
    void TransitionToShaderResource(ID3D12GraphicsCommandList* cmd);
    void TransitionToDepthWrite(ID3D12GraphicsCommandList* cmd);

    D3D12_GPU_DESCRIPTOR_HANDLE GetSrvGpu() const { return m_srvGpu; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetSrvCpu() const { return m_srvCpu; }
    ID3D12DescriptorHeap* GetSrvHeap() const { return m_srvHeap.Get(); }

    const DirectX::XMFLOAT4X4& GetLightViewProj(UINT cascade) const { return m_lightViewProj[cascade]; }
    const DirectX::XMFLOAT4& GetCascadeSplits() const { return m_cascadeSplits; }
    bool IsValid() const { return m_depthArray != nullptr; }

private:
    void CreateResources(ID3D12Device* device);

    Microsoft::WRL::ComPtr<ID3D12Resource> m_depthArray;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE m_dsvCpu[kCascadeCount]{};
    D3D12_CPU_DESCRIPTOR_HANDLE m_srvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE m_srvGpu{};
    UINT m_dsvDescriptorSize = 0;

    DirectX::XMFLOAT4X4 m_lightViewProj[kCascadeCount]{};
    DirectX::XMFLOAT4 m_cascadeSplits{};
    bool m_inShaderResourceState = false;
};
