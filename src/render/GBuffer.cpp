#include "GBuffer.h"
#include "Common.h"

#include <algorithm>

using Microsoft::WRL::ComPtr;

DXGI_FORMAT GBuffer::RtFormat(UINT index) {
    static constexpr DXGI_FORMAT kFormats[kRtCount] = {
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R8G8B8A8_UNORM, // ORM: R=AO, G=roughness, B=metallic
    };
    return kFormats[index < kRtCount ? index : 0];
}

void GBuffer::Create(ID3D12Device* device, UINT width, UINT height) {
    m_rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_srvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.NumDescriptors = kRtCount;
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap)), "GBuffer RTV heap failed");

    D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{};
    dsvDesc.NumDescriptors = 1;
    dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    ThrowIfFailed(device->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&m_dsvHeap)), "GBuffer DSV heap failed");

    D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
    srvDesc.NumDescriptors = kSrvCount;
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&m_srvHeap)), "GBuffer SRV heap failed");

    CreateTargets(device, width, height);
}

void GBuffer::Resize(ID3D12Device* device, UINT width, UINT height) {
    if (width == 0 || height == 0 || (width == m_width && height == m_height)) {
        return;
    }
    for (UINT i = 0; i < kRtCount; ++i) {
        m_color[i].Reset();
    }
    m_depth.Reset();
    CreateTargets(device, width, height);
}

void GBuffer::Shutdown() {
    for (UINT i = 0; i < kRtCount; ++i) {
        m_color[i].Reset();
    }
    m_depth.Reset();
    m_rtvHeap.Reset();
    m_dsvHeap.Reset();
    m_srvHeap.Reset();
    m_width = 0;
    m_height = 0;
    m_inShaderResourceState = false;
}

void GBuffer::CreateTargets(ID3D12Device* device, UINT width, UINT height) {
    m_width = width;
    m_height = height;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    for (UINT i = 0; i < kRtCount; ++i) {
        const DXGI_FORMAT format = RtFormat(i);

        D3D12_CLEAR_VALUE clear{};
        clear.Format = format;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        ThrowIfFailed(
            device->CreateCommittedResource(
                &defaultHeap,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                &clear,
                IID_PPV_ARGS(&m_color[i])),
            "GBuffer color create failed");
    }

    {
        D3D12_CLEAR_VALUE depthClear{};
        depthClear.Format = DXGI_FORMAT_D32_FLOAT;
        depthClear.DepthStencil.Depth = 1.0f;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R32_TYPELESS;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        ThrowIfFailed(
            device->CreateCommittedResource(
                &defaultHeap,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                D3D12_RESOURCE_STATE_DEPTH_WRITE,
                &depthClear,
                IID_PPV_ARGS(&m_depth)),
            "GBuffer depth create failed");
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kRtCount; ++i) {
        device->CreateRenderTargetView(m_color[i].Get(), nullptr, rtv);
        rtv.ptr += m_rtvDescriptorSize;
    }

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(m_depth.Get(), &dsvDesc, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_CPU_DESCRIPTOR_HANDLE srv = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kRtCount; ++i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = RtFormat(i);
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(m_color[i].Get(), &srvDesc, srv);
        srv.ptr += m_srvDescriptorSize;
    }

    m_inShaderResourceState = false;
}

void GBuffer::TransitionToWrite(ID3D12GraphicsCommandList* cmd) {
    if (!m_inShaderResourceState) {
        return;
    }

    D3D12_RESOURCE_BARRIER barriers[kRtCount]{};
    for (UINT i = 0; i < kRtCount; ++i) {
        barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[i].Transition.pResource = m_color[i].Get();
        barriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barriers[i].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    cmd->ResourceBarrier(kRtCount, barriers);
    m_inShaderResourceState = false;
}

void GBuffer::TransitionToRead(ID3D12GraphicsCommandList* cmd) {
    D3D12_RESOURCE_BARRIER barriers[kRtCount]{};
    for (UINT i = 0; i < kRtCount; ++i) {
        barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[i].Transition.pResource = m_color[i].Get();
        barriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[i].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    cmd->ResourceBarrier(kRtCount, barriers);
    m_inShaderResourceState = true;
}

void GBuffer::BindAsRenderTargets(ID3D12GraphicsCommandList* cmd) {
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[kRtCount]{};
    rtvs[0] = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 1; i < kRtCount; ++i) {
        rtvs[i] = rtvs[0];
        rtvs[i].ptr += static_cast<SIZE_T>(i) * m_rtvDescriptorSize;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    cmd->OMSetRenderTargets(kRtCount, rtvs, FALSE, &dsv);
}

void GBuffer::Clear(ID3D12GraphicsCommandList* cmd) {
    const float black[] = {0.0f, 0.0f, 0.0f, 0.0f};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kRtCount; ++i) {
        cmd->ClearRenderTargetView(rtv, black, 0, nullptr);
        rtv.ptr += m_rtvDescriptorSize;
    }
    cmd->ClearDepthStencilView(
        m_dsvHeap->GetCPUDescriptorHandleForHeapStart(),
        D3D12_CLEAR_FLAG_DEPTH,
        1.0f,
        0,
        0,
        nullptr);
}

void GBuffer::BindAsShaderResources(ID3D12GraphicsCommandList* cmd, UINT rootParameterIndex) {
    cmd->SetGraphicsRootDescriptorTable(rootParameterIndex, m_srvHeap->GetGPUDescriptorHandleForHeapStart());
}

D3D12_CPU_DESCRIPTOR_HANDLE GBuffer::GetSrvCpu(UINT index) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>((std::min)(index, kSrvCount - 1u)) * m_srvDescriptorSize;
    return h;
}

ID3D12Resource* GBuffer::GetColorResource(UINT index) const {
    return m_color[(std::min)(index, kRtCount - 1u)].Get();
}
