#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

// Lab 2/8 G-buffer:
// RT0 albedo, RT1 world normal, RT2 world position, RT3 ORM (ao/rough/metal) + depth DSV
class GBuffer {
public:
    static constexpr UINT kRtCount = 4;
    static constexpr UINT kAlbedoIndex = 0;
    static constexpr UINT kNormalIndex = 1;
    static constexpr UINT kPositionIndex = 2;
    static constexpr UINT kMaterialIndex = 3;
    static constexpr UINT kSrvCount = kRtCount;

    void Create(ID3D12Device* device, UINT width, UINT height);
    void Resize(ID3D12Device* device, UINT width, UINT height);
    void Shutdown();

    void TransitionToWrite(ID3D12GraphicsCommandList* cmd);
    void TransitionToRead(ID3D12GraphicsCommandList* cmd);

    void BindAsRenderTargets(ID3D12GraphicsCommandList* cmd);
    void Clear(ID3D12GraphicsCommandList* cmd);
    void BindAsShaderResources(ID3D12GraphicsCommandList* cmd, UINT rootParameterIndex);

    ID3D12DescriptorHeap* GetSrvHeap() const { return m_srvHeap.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE GetSrvCpu(UINT index) const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetDsvCpu() const {
        return m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    }
    ID3D12Resource* GetColorResource(UINT index) const;
    ID3D12Resource* GetDepthResource() const { return m_depth.Get(); }
    UINT GetWidth() const { return m_width; }
    UINT GetHeight() const { return m_height; }
    bool IsValid() const { return m_color[0] != nullptr; }

    static DXGI_FORMAT RtFormat(UINT index);

private:
    void CreateTargets(ID3D12Device* device, UINT width, UINT height);

    Microsoft::WRL::ComPtr<ID3D12Resource> m_color[kRtCount];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_depth;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;

    UINT m_rtvDescriptorSize = 0;
    UINT m_srvDescriptorSize = 0;
    UINT m_width = 0;
    UINT m_height = 0;
    bool m_inShaderResourceState = false;
};
