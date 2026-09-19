#pragma once

#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

// Lab 6: opaque GPU particles — Consume/Append StructuredBuffers, CS update, GS billboards.
class ParticleSystem {
public:
    static constexpr UINT kMaxParticles = 1024;
    static constexpr UINT kThreadGroupSize = 64;
    static constexpr UINT kFrameBuffers = 2;
    static_assert(kMaxParticles % kThreadGroupSize == 0, "Dispatch must not pad Consume threads");

    struct Particle {
        DirectX::XMFLOAT3 position;
        float life = 0.0f;
        DirectX::XMFLOAT3 velocity;
        float size = 1.0f;
        DirectX::XMFLOAT4 color{1.0f, 1.0f, 1.0f, 1.0f};
    };

    void Initialize(ID3D12Device* device, ID3D12GraphicsCommandList* uploadCmd);
    void Shutdown();

    void SetEmitter(const DirectX::XMFLOAT3& position) { m_emitterPos = position; }
    void SetEnabled(bool enabled) { m_enabled = enabled; }
    void Toggle() { m_enabled = !m_enabled; }
    bool IsEnabled() const { return m_enabled; }
    UINT GetMaxParticles() const { return kMaxParticles; }

    // Call before recording particle work if a previous Signal is outstanding.
    void WaitForGpu();
    // Call after ExecuteCommandLists that included particle work.
    void Signal(ID3D12CommandQueue* queue);

    void UpdateAndRender(
        ID3D12GraphicsCommandList* cmd,
        UINT frameIndex,
        const DirectX::XMMATRIX& viewProj,
        DirectX::FXMVECTOR cameraRight,
        DirectX::FXMVECTOR cameraUp,
        float deltaSeconds,
        D3D12_CPU_DESCRIPTOR_HANDLE backbufferRtv,
        D3D12_CPU_DESCRIPTOR_HANDLE sceneDsv,
        const D3D12_VIEWPORT& viewport,
        const D3D12_RECT& scissor);

    bool IsValid() const { return m_buffers[0] != nullptr; }

private:
    struct SimCB {
        float deltaTime = 0.0f;
        float gravity = -9.8f;
        UINT maxParticles = kMaxParticles;
        UINT pad0 = 0;
        DirectX::XMFLOAT3 emitterPos{};
        float pad1 = 0.0f;
    };

    struct DrawCB {
        DirectX::XMFLOAT4X4 viewProj{};
        DirectX::XMFLOAT3 cameraRight{};
        float pad0 = 0.0f;
        DirectX::XMFLOAT3 cameraUp{};
        float pad1 = 0.0f;
    };

    void CreatePipelines(ID3D12Device* device);
    void CreateBuffers(ID3D12Device* device, ID3D12GraphicsCommandList* uploadCmd);
    void ResetAppendCounter(ID3D12GraphicsCommandList* cmd, UINT appendIndex);

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_csRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_csPso;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_drawRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_drawPso;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_buffers[2];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_counterBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_counterZeroUpload;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_counterInitUpload;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_particleUpload;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_simCb;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_drawCb;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_uavHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;

    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEvent = nullptr;
    UINT64 m_fenceValue = 0;
    bool m_workPending = false;

    UINT m_descriptorSize = 0;
    UINT m_consumeIndex = 0;
    UINT m_simCbStride = 256;
    UINT m_drawCbStride = 256;
    bool m_enabled = true;

    uint8_t* m_mappedSimCbBytes = nullptr;
    uint8_t* m_mappedDrawCbBytes = nullptr;
    DirectX::XMFLOAT3 m_emitterPos{0.0f, 50.0f, 0.0f};
};
