#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

// Lab 7: fullscreen post — Depth of Field + Chromatic Aberration.
// Scene color is double-buffered to match swapchain frames-in-flight.
class PostProcess {
public:
    static constexpr UINT kFrameBuffers = 2;

    void Initialize(ID3D12Device* device, UINT width, UINT height);
    void Shutdown();
    void Resize(ID3D12Device* device, UINT width, UINT height);

    void BeginScene(ID3D12GraphicsCommandList* cmd, UINT frameIndex);
    D3D12_CPU_DESCRIPTOR_HANDLE GetSceneRtv(UINT frameIndex) const;
    ID3D12Resource* GetSceneColor(UINT frameIndex) const;

    void Apply(
        ID3D12GraphicsCommandList* cmd,
        UINT frameIndex,
        D3D12_CPU_DESCRIPTOR_HANDLE backbufferRtv,
        ID3D12Resource* sceneDepthResource,
        float nearZ,
        float farZ,
        const D3D12_VIEWPORT& viewport,
        const D3D12_RECT& scissor);

    void ToggleDof() { m_dofEnabled = !m_dofEnabled; }
    void ToggleChromatic() { m_caEnabled = !m_caEnabled; }
    bool IsDofEnabled() const { return m_dofEnabled; }
    bool IsChromaticEnabled() const { return m_caEnabled; }

    bool IsValid() const { return m_sceneColor[0] != nullptr; }
    UINT GetWidth() const { return m_width; }
    UINT GetHeight() const { return m_height; }

private:
    struct PostCB {
        float nearZ = 0.5f;
        float farZ = 5000.0f;
        float focusDistance = 420.0f;
        float focusRange = 220.0f;
        float maxBlurPixels = 6.0f;
        float chromaticStrength = 0.0035f;
        float chromaticRadial = 1.35f;
        float dofEnabled = 1.0f;
        float caEnabled = 1.0f;
        float pad[3]{};
    };

    void CreateSceneTargets(ID3D12Device* device, UINT width, UINT height);
    void CreateDepthSnapshots(ID3D12Device* device, UINT width, UINT height);
    void CreatePipeline(ID3D12Device* device);
    void CreateConstantBuffer(ID3D12Device* device);
    void CreateSceneSrv(ID3D12Device* device, UINT frame);
    void CreateDepthSnapshotSrv(ID3D12Device* device, UINT frame);

    Microsoft::WRL::ComPtr<ID3D12Resource> m_sceneColor[kFrameBuffers];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_depthSnapshot[kFrameBuffers];
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap; // 2 RTVs
    // SRV layout: [scene0, depth0, scene1, depth1]
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_cb;

    uint8_t* m_mappedCbBytes = nullptr;
    UINT m_cbStride = 256;
    UINT m_srvDescriptorSize = 0;
    UINT m_rtvDescriptorSize = 0;
    UINT m_width = 0;
    UINT m_height = 0;
    bool m_sceneIsRtv[kFrameBuffers]{true, true};
    bool m_depthSnapshotIsSrv[kFrameBuffers]{false, false};

    bool m_dofEnabled = true;
    bool m_caEnabled = true;
    float m_focusDistance = 420.0f;
    float m_focusRange = 220.0f;
    float m_maxBlurPixels = 6.0f;
    float m_chromaticStrength = 0.0035f;
    float m_chromaticRadial = 1.35f;
};
