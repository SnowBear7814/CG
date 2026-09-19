#include "PostProcess.h"
#include "Common.h"

#include <d3dcompiler.h>

#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace {
ComPtr<ID3DBlob> CompileShader(const std::wstring& path, const char* entry, const char* target) {
    UINT flags = 0;
#if defined(_DEBUG)
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> error;
    const HRESULT hr = D3DCompileFromFile(path.c_str(), nullptr, nullptr, entry, target, flags, 0, &blob, &error);
    if (FAILED(hr)) {
        const char* message = error ? static_cast<const char*>(error->GetBufferPointer()) : "Post shader compile failed";
        throw std::runtime_error(message);
    }
    return blob;
}
} // namespace

void PostProcess::Initialize(ID3D12Device* device, UINT width, UINT height) {
    m_srvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.NumDescriptors = kFrameBuffers;
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap)), "Post RTV heap failed");

    D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
    srvDesc.NumDescriptors = kFrameBuffers * 2; // scene+depth per frame
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&m_srvHeap)), "Post SRV heap failed");

    CreateConstantBuffer(device);
    CreatePipeline(device);
    CreateSceneTargets(device, width, height);
    CreateDepthSnapshots(device, width, height);
}

void PostProcess::Shutdown() {
    if (m_mappedCbBytes && m_cb) {
        m_cb->Unmap(0, nullptr);
        m_mappedCbBytes = nullptr;
    }
    m_pso.Reset();
    m_rootSignature.Reset();
    m_cb.Reset();
    for (UINT i = 0; i < kFrameBuffers; ++i) {
        m_sceneColor[i].Reset();
        m_depthSnapshot[i].Reset();
        m_sceneIsRtv[i] = true;
        m_depthSnapshotIsSrv[i] = false;
    }
    m_rtvHeap.Reset();
    m_srvHeap.Reset();
    m_width = 0;
    m_height = 0;
}

void PostProcess::Resize(ID3D12Device* device, UINT width, UINT height) {
    if (width == 0 || height == 0 || (width == m_width && height == m_height)) {
        return;
    }
    // Caller must WaitForGpu before Resize (Renderer does).
    for (UINT i = 0; i < kFrameBuffers; ++i) {
        m_sceneColor[i].Reset();
        m_depthSnapshot[i].Reset();
    }
    CreateSceneTargets(device, width, height);
    CreateDepthSnapshots(device, width, height);
}

void PostProcess::CreateConstantBuffer(ID3D12Device* device) {
    m_cbStride = AlignUp(static_cast<UINT>(sizeof(PostCB)), 256);

    D3D12_HEAP_PROPERTIES upload{};
    upload.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = static_cast<UINT64>(m_cbStride) * kFrameBuffers;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ThrowIfFailed(
        device->CreateCommittedResource(
            &upload,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&m_cb)),
        "Post CB create failed");
    ThrowIfFailed(m_cb->Map(0, nullptr, reinterpret_cast<void**>(&m_mappedCbBytes)), "Post CB map failed");
}

void PostProcess::CreateSceneSrv(ID3D12Device* device, UINT frame) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(frame * 2) * m_srvDescriptorSize;
    device->CreateShaderResourceView(m_sceneColor[frame].Get(), &srvDesc, cpu);
}

void PostProcess::CreateSceneTargets(ID3D12Device* device, UINT width, UINT height) {
    m_width = width;
    m_height = height;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    clear.Color[0] = 0.45f;
    clear.Color[1] = 0.55f;
    clear.Color[2] = 0.70f;
    clear.Color[3] = 1.0f;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    for (UINT i = 0; i < kFrameBuffers; ++i) {
        ThrowIfFailed(
            device->CreateCommittedResource(
                &defaultHeap,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                &clear,
                IID_PPV_ARGS(&m_sceneColor[i])),
            "Post scene RT create failed");

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(i) * m_rtvDescriptorSize;
        device->CreateRenderTargetView(m_sceneColor[i].Get(), nullptr, rtv);
        CreateSceneSrv(device, i);
        m_sceneIsRtv[i] = true;
    }
}

void PostProcess::CreatePipeline(ID3D12Device* device) {
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 2;
    srvRange.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[0].Descriptor.ShaderRegister = 0;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srvRange;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = 2;
    rootDesc.pParameters = params;
    rootDesc.NumStaticSamplers = 1;
    rootDesc.pStaticSamplers = &sampler;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    ThrowIfFailed(
        D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error),
        "Post root serialize failed");
    ThrowIfFailed(
        device->CreateRootSignature(
            0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)),
        "Post root create failed");

    const std::wstring shaderPath = std::wstring(CONTENT_DIR) + L"/shaders/post_process.hlsl";
    ComPtr<ID3DBlob> vs = CompileShader(shaderPath, "VS_Post", "vs_5_1");
    ComPtr<ID3DBlob> ps = CompileShader(shaderPath, "PS_Post", "ps_5_1");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_rootSignature.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.InputLayout = {nullptr, 0};
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.SampleDesc.Count = 1;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pso)), "Post PSO failed");
}

void PostProcess::CreateDepthSnapshotSrv(ID3D12Device* device, UINT frame) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(frame * 2 + 1) * m_srvDescriptorSize;
    device->CreateShaderResourceView(m_depthSnapshot[frame].Get(), &srvDesc, cpu);
}

void PostProcess::CreateDepthSnapshots(ID3D12Device* device, UINT width, UINT height) {
    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    for (UINT i = 0; i < kFrameBuffers; ++i) {
        ThrowIfFailed(
            device->CreateCommittedResource(
                &defaultHeap,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(&m_depthSnapshot[i])),
            "Post depth snapshot create failed");
        CreateDepthSnapshotSrv(device, i);
        m_depthSnapshotIsSrv[i] = false;
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE PostProcess::GetSceneRtv(UINT frameIndex) const {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(frameIndex % kFrameBuffers) * m_rtvDescriptorSize;
    return rtv;
}

ID3D12Resource* PostProcess::GetSceneColor(UINT frameIndex) const {
    return m_sceneColor[frameIndex % kFrameBuffers].Get();
}

void PostProcess::BeginScene(ID3D12GraphicsCommandList* cmd, UINT frameIndex) {
    const UINT frame = frameIndex % kFrameBuffers;
    if (!m_sceneIsRtv[frame]) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = m_sceneColor[frame].Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &barrier);
        m_sceneIsRtv[frame] = true;
    }

    const float clearColor[] = {0.45f, 0.55f, 0.70f, 1.0f};
    cmd->ClearRenderTargetView(GetSceneRtv(frame), clearColor, 0, nullptr);
}

void PostProcess::Apply(
    ID3D12GraphicsCommandList* cmd,
    UINT frameIndex,
    D3D12_CPU_DESCRIPTOR_HANDLE backbufferRtv,
    ID3D12Resource* sceneDepthResource,
    float nearZ,
    float farZ,
    const D3D12_VIEWPORT& viewport,
    const D3D12_RECT& scissor) {
    if (!IsValid() || !sceneDepthResource || !m_depthSnapshot[0]) {
        return;
    }

    const UINT frame = frameIndex % kFrameBuffers;

    if (m_sceneIsRtv[frame]) {
        D3D12_RESOURCE_BARRIER toSrv{};
        toSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toSrv.Transition.pResource = m_sceneColor[frame].Get();
        toSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        toSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        toSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &toSrv);
        m_sceneIsRtv[frame] = false;
    }

    // Copy scene depth into a per-frame snapshot so the shared DSV is free for the next frame.
    {
        D3D12_RESOURCE_BARRIER before[2]{};
        before[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        before[0].Transition.pResource = sceneDepthResource;
        before[0].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        before[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        before[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        before[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        before[1].Transition.pResource = m_depthSnapshot[frame].Get();
        before[1].Transition.StateBefore =
            m_depthSnapshotIsSrv[frame] ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                        : D3D12_RESOURCE_STATE_COPY_DEST;
        before[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        before[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        // If already COPY_DEST, skip second barrier's noop by only issuing when needed
        if (m_depthSnapshotIsSrv[frame]) {
            cmd->ResourceBarrier(2, before);
        } else {
            cmd->ResourceBarrier(1, &before[0]);
        }

        cmd->CopyResource(m_depthSnapshot[frame].Get(), sceneDepthResource);

        D3D12_RESOURCE_BARRIER after[2]{};
        after[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        after[0].Transition.pResource = sceneDepthResource;
        after[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        after[0].Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        after[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        after[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        after[1].Transition.pResource = m_depthSnapshot[frame].Get();
        after[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        after[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        after[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(2, after);
        m_depthSnapshotIsSrv[frame] = true;
    }

    auto* cb = reinterpret_cast<PostCB*>(m_mappedCbBytes + frame * m_cbStride);
    cb->nearZ = nearZ;
    cb->farZ = farZ;
    cb->focusDistance = m_focusDistance;
    cb->focusRange = m_focusRange;
    cb->maxBlurPixels = m_maxBlurPixels;
    cb->chromaticStrength = m_chromaticStrength;
    cb->chromaticRadial = m_chromaticRadial;
    cb->dofEnabled = m_dofEnabled ? 1.0f : 0.0f;
    cb->caEnabled = m_caEnabled ? 1.0f : 0.0f;

    cmd->OMSetRenderTargets(1, &backbufferRtv, FALSE, nullptr);
    cmd->RSSetViewports(1, &viewport);
    cmd->RSSetScissorRects(1, &scissor);

    cmd->SetPipelineState(m_pso.Get());
    cmd->SetGraphicsRootSignature(m_rootSignature.Get());
    ID3D12DescriptorHeap* heaps[] = {m_srvHeap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootConstantBufferView(
        0, m_cb->GetGPUVirtualAddress() + static_cast<UINT64>(frame) * m_cbStride);

    D3D12_GPU_DESCRIPTOR_HANDLE table = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    table.ptr += static_cast<SIZE_T>(frame * 2) * m_srvDescriptorSize;
    cmd->SetGraphicsRootDescriptorTable(1, table);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);
}
