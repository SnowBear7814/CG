#pragma once

#include "Camera.h"
#include "RenderingSystem.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

// Owns DX12 device / swapchain / frame sync; delegates deferred work to RenderingSystem.
class Renderer {
public:
    bool Initialize(HWND hwnd, UINT width, UINT height);
    void Shutdown();

    void Resize(UINT width, UINT height);
    void LoadModel(const std::wstring& objPath);
    void LoadRocks(const std::wstring& objPath);
    void LoadSkybox(const std::wstring& ddsPath);
    void DrawFrame(const Camera& camera, float timeSeconds, float deltaSeconds);

    bool IsInitialized() const { return m_device != nullptr; }
    bool HasModel() const { return m_renderingSystem.HasModel(); }
    DirectX::XMFLOAT3 GetModelBoundsMin() const { return m_renderingSystem.GetModelBoundsMin(); }
    DirectX::XMFLOAT3 GetModelBoundsMax() const { return m_renderingSystem.GetModelBoundsMax(); }
    size_t GetLightCount() const { return m_renderingSystem.GetLights().size(); }
    UINT GetRockInstanceCount() const { return m_renderingSystem.GetRockInstanceCount(); }
    UINT GetRocksDrawnLastFrame() const { return m_renderingSystem.GetRocksDrawnLastFrame(); }
    bool IsFrustumCullingEnabled() const { return m_renderingSystem.IsFrustumCullingEnabled(); }
    void ToggleFrustumCulling() { m_renderingSystem.ToggleFrustumCulling(); }
    bool IsOctreeCullingEnabled() const { return m_renderingSystem.IsOctreeCullingEnabled(); }
    void ToggleOctreeCulling() { m_renderingSystem.ToggleOctreeCulling(); }
    UINT GetOctreeNodeCount() const { return m_renderingSystem.GetOctreeNodeCount(); }
    void ToggleShadows() { m_renderingSystem.ToggleShadows(); }
    bool AreShadowsEnabled() const { return m_renderingSystem.AreShadowsEnabled(); }
    void ToggleParticles() { m_renderingSystem.ToggleParticles(); }
    bool AreParticlesEnabled() const { return m_renderingSystem.AreParticlesEnabled(); }
    UINT GetParticleCount() const { return m_renderingSystem.GetParticleCount(); }
    void ToggleDof() { m_renderingSystem.ToggleDof(); }
    void ToggleChromaticAberration() { m_renderingSystem.ToggleChromaticAberration(); }
    bool IsDofEnabled() const { return m_renderingSystem.IsDofEnabled(); }
    bool IsChromaticAberrationEnabled() const { return m_renderingSystem.IsChromaticAberrationEnabled(); }
    void TogglePbr() { m_renderingSystem.TogglePbr(); }
    bool IsPbrEnabled() const { return m_renderingSystem.IsPbrEnabled(); }

    ID3D12Device* GetDevice() const { return m_device.Get(); }

private:
    static constexpr UINT kFrameCount = 2;

    void WaitForGpu();
    void MoveToNextFrame();
    void CreateRenderTargetViews();

    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_commandQueue;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_renderTargets[kFrameCount];
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_commandAllocators[kFrameCount];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;

    RenderingSystem m_renderingSystem;

    UINT m_rtvDescriptorSize = 0;
    UINT m_frameIndex = 0;
    UINT m_width = 0;
    UINT m_height = 0;
    UINT64 m_fenceValues[kFrameCount]{};
    HANDLE m_fenceEvent = nullptr;
};
