#include "ParticleSystem.h"
#include "Common.h"

#include <d3dcompiler.h>

#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

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
        const char* message = error ? static_cast<const char*>(error->GetBufferPointer()) : "Particle shader compile failed";
        throw std::runtime_error(message);
    }
    return blob;
}

constexpr UINT kCounterAlign = D3D12_UAV_COUNTER_PLACEMENT_ALIGNMENT;
} // namespace

void ParticleSystem::Initialize(ID3D12Device* device, ID3D12GraphicsCommandList* uploadCmd) {
    m_descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CreateBuffers(device, uploadCmd);
    CreatePipelines(device);

    ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)), "Particle fence failed");
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) {
        throw std::runtime_error("Particle fence event failed");
    }
    m_fenceValue = 0;
    m_workPending = false;
}

void ParticleSystem::Shutdown() {
    WaitForGpu();

    if (m_fenceEvent) {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
    m_fence.Reset();

    if (m_mappedSimCbBytes && m_simCb) {
        m_simCb->Unmap(0, nullptr);
        m_mappedSimCbBytes = nullptr;
    }
    if (m_mappedDrawCbBytes && m_drawCb) {
        m_drawCb->Unmap(0, nullptr);
        m_mappedDrawCbBytes = nullptr;
    }

    m_csPso.Reset();
    m_csRootSignature.Reset();
    m_drawPso.Reset();
    m_drawRootSignature.Reset();
    m_buffers[0].Reset();
    m_buffers[1].Reset();
    m_counterBuffer.Reset();
    m_counterZeroUpload.Reset();
    m_counterInitUpload.Reset();
    m_particleUpload.Reset();
    m_simCb.Reset();
    m_drawCb.Reset();
    m_uavHeap.Reset();
    m_srvHeap.Reset();
    m_consumeIndex = 0;
    m_workPending = false;
}

void ParticleSystem::WaitForGpu() {
    if (!m_fence || m_fenceValue == 0) {
        return;
    }
    if (m_fence->GetCompletedValue() < m_fenceValue) {
        ThrowIfFailed(
            m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent),
            "Particle fence wait failed");
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
    m_workPending = false;
}

void ParticleSystem::Signal(ID3D12CommandQueue* queue) {
    if (!m_fence || !queue || !m_workPending) {
        return;
    }
    ++m_fenceValue;
    ThrowIfFailed(queue->Signal(m_fence.Get(), m_fenceValue), "Particle fence signal failed");
    m_workPending = false;
}

void ParticleSystem::CreateBuffers(ID3D12Device* device, ID3D12GraphicsCommandList* uploadCmd) {
    const UINT64 particleBytes = static_cast<UINT64>(sizeof(Particle)) * kMaxParticles;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    auto createDefaultBuffer = [&](UINT64 size, D3D12_RESOURCE_FLAGS flags, ComPtr<ID3D12Resource>& out) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = flags;
        ThrowIfFailed(
            device->CreateCommittedResource(
                &defaultHeap,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                D3D12_RESOURCE_STATE_COMMON,
                nullptr,
                IID_PPV_ARGS(&out)),
            "Particle buffer create failed");
    };

    auto createUploadBuffer = [&](UINT64 size, ComPtr<ID3D12Resource>& out) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(
            device->CreateCommittedResource(
                &uploadHeap,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&out)),
            "Particle upload create failed");
    };

    createDefaultBuffer(particleBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_buffers[0]);
    createDefaultBuffer(particleBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_buffers[1]);
    createDefaultBuffer(kCounterAlign * 2u, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_counterBuffer);

    std::vector<Particle> seeds(kMaxParticles);
    for (UINT i = 0; i < kMaxParticles; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kMaxParticles);
        const float a = t * 6.2831853f * 7.0f;
        const float r = 2.0f + std::fmod(t * 37.0f, 1.0f) * 8.0f;
        seeds[i].position = {
            m_emitterPos.x + std::cos(a) * r,
            m_emitterPos.y + std::fmod(t * 53.0f, 1.0f) * 12.0f,
            m_emitterPos.z + std::sin(a) * r};
        seeds[i].velocity = {
            (std::fmod(t * 17.0f, 1.0f) - 0.5f) * 6.0f,
            8.0f + std::fmod(t * 29.0f, 1.0f) * 14.0f,
            (std::fmod(t * 19.0f, 1.0f) - 0.5f) * 6.0f};
        seeds[i].life = 1.5f + std::fmod(t * 41.0f, 1.0f) * 2.5f;
        seeds[i].size = 1.2f + std::fmod(t * 23.0f, 1.0f) * 1.6f;
        seeds[i].color = {
            0.85f + std::fmod(t * 11.0f, 1.0f) * 0.15f,
            0.55f + std::fmod(t * 13.0f, 1.0f) * 0.35f,
            0.15f + std::fmod(t * 7.0f, 1.0f) * 0.25f,
            1.0f};
    }

    createUploadBuffer(particleBytes, m_particleUpload);
    void* mappedParticles = nullptr;
    ThrowIfFailed(m_particleUpload->Map(0, nullptr, &mappedParticles), "Particle seed map failed");
    std::memcpy(mappedParticles, seeds.data(), static_cast<size_t>(particleBytes));
    m_particleUpload->Unmap(0, nullptr);

    createUploadBuffer(sizeof(UINT) * 2, m_counterInitUpload);
    createUploadBuffer(sizeof(UINT), m_counterZeroUpload);
    {
        UINT* counts = nullptr;
        ThrowIfFailed(m_counterInitUpload->Map(0, nullptr, reinterpret_cast<void**>(&counts)), "Counter init map failed");
        counts[0] = kMaxParticles;
        counts[1] = 0;
        m_counterInitUpload->Unmap(0, nullptr);

        UINT* zero = nullptr;
        ThrowIfFailed(m_counterZeroUpload->Map(0, nullptr, reinterpret_cast<void**>(&zero)), "Counter zero map failed");
        *zero = 0;
        m_counterZeroUpload->Unmap(0, nullptr);
    }

    {
        D3D12_RESOURCE_BARRIER toCopy[3]{};
        for (UINT i = 0; i < 2; ++i) {
            toCopy[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toCopy[i].Transition.pResource = m_buffers[i].Get();
            toCopy[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            toCopy[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            toCopy[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        toCopy[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopy[2].Transition.pResource = m_counterBuffer.Get();
        toCopy[2].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        toCopy[2].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        toCopy[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        uploadCmd->ResourceBarrier(3, toCopy);
    }

    uploadCmd->CopyBufferRegion(m_buffers[0].Get(), 0, m_particleUpload.Get(), 0, particleBytes);
    uploadCmd->CopyBufferRegion(m_counterBuffer.Get(), 0, m_counterInitUpload.Get(), 0, sizeof(UINT));
    uploadCmd->CopyBufferRegion(m_counterBuffer.Get(), kCounterAlign, m_counterInitUpload.Get(), sizeof(UINT), sizeof(UINT));

    {
        D3D12_RESOURCE_BARRIER toUav[3]{};
        for (UINT i = 0; i < 2; ++i) {
            toUav[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toUav[i].Transition.pResource = m_buffers[i].Get();
            toUav[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            toUav[i].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            toUav[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        toUav[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toUav[2].Transition.pResource = m_counterBuffer.Get();
        toUav[2].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        toUav[2].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toUav[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        uploadCmd->ResourceBarrier(3, toUav);
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.NumDescriptors = 2;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_uavHeap)), "Particle UAV heap failed");
    ThrowIfFailed(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_srvHeap)), "Particle SRV heap failed");

    for (UINT i = 0; i < 2; ++i) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.NumElements = kMaxParticles;
        uavDesc.Buffer.StructureByteStride = sizeof(Particle);
        uavDesc.Buffer.CounterOffsetInBytes = i * kCounterAlign;
        uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

        D3D12_CPU_DESCRIPTOR_HANDLE uavCpu = m_uavHeap->GetCPUDescriptorHandleForHeapStart();
        uavCpu.ptr += static_cast<SIZE_T>(i) * m_descriptorSize;
        device->CreateUnorderedAccessView(m_buffers[i].Get(), m_counterBuffer.Get(), &uavDesc, uavCpu);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Buffer.FirstElement = 0;
        srvDesc.Buffer.NumElements = kMaxParticles;
        srvDesc.Buffer.StructureByteStride = sizeof(Particle);
        srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

        D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        srvCpu.ptr += static_cast<SIZE_T>(i) * m_descriptorSize;
        device->CreateShaderResourceView(m_buffers[i].Get(), &srvDesc, srvCpu);
    }

    m_simCbStride = AlignUp(static_cast<UINT>(sizeof(SimCB)), 256);
    m_drawCbStride = AlignUp(static_cast<UINT>(sizeof(DrawCB)), 256);

    auto createCbRing = [&](UINT stride, ComPtr<ID3D12Resource>& res, uint8_t** mapped) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = static_cast<UINT64>(stride) * kFrameBuffers;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(
            device->CreateCommittedResource(
                &uploadHeap,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&res)),
            "Particle CB create failed");
        ThrowIfFailed(res->Map(0, nullptr, reinterpret_cast<void**>(mapped)), "Particle CB map failed");
    };
    createCbRing(m_simCbStride, m_simCb, &m_mappedSimCbBytes);
    createCbRing(m_drawCbStride, m_drawCb, &m_mappedDrawCbBytes);

    m_consumeIndex = 0;
}

void ParticleSystem::CreatePipelines(ID3D12Device* device) {
    const std::wstring shaderPath = std::wstring(CONTENT_DIR) + L"/shaders/particles.hlsl";

    {
        D3D12_DESCRIPTOR_RANGE uavConsume{};
        uavConsume.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavConsume.NumDescriptors = 1;
        uavConsume.BaseShaderRegister = 0;

        D3D12_DESCRIPTOR_RANGE uavAppend{};
        uavAppend.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavAppend.NumDescriptors = 1;
        uavAppend.BaseShaderRegister = 1;

        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[0].Descriptor.ShaderRegister = 0;

        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &uavConsume;

        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].DescriptorTable.NumDescriptorRanges = 1;
        params[2].DescriptorTable.pDescriptorRanges = &uavAppend;

        D3D12_ROOT_SIGNATURE_DESC rootDesc{};
        rootDesc.NumParameters = 3;
        rootDesc.pParameters = params;

        ComPtr<ID3DBlob> signature;
        ComPtr<ID3DBlob> error;
        ThrowIfFailed(
            D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error),
            "Particle CS root serialize failed");
        ThrowIfFailed(
            device->CreateRootSignature(
                0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_csRootSignature)),
            "Particle CS root create failed");

        ComPtr<ID3DBlob> cs = CompileShader(shaderPath, "CS_Update", "cs_5_1");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_csRootSignature.Get();
        pso.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
        ThrowIfFailed(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_csPso)), "Particle CS PSO failed");
    }

    {
        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = 1;
        srvRange.BaseShaderRegister = 0;

        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[0].Descriptor.ShaderRegister = 0;

        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &srvRange;

        D3D12_ROOT_SIGNATURE_DESC rootDesc{};
        rootDesc.NumParameters = 2;
        rootDesc.pParameters = params;

        ComPtr<ID3DBlob> signature;
        ComPtr<ID3DBlob> error;
        ThrowIfFailed(
            D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error),
            "Particle draw root serialize failed");
        ThrowIfFailed(
            device->CreateRootSignature(
                0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_drawRootSignature)),
            "Particle draw root create failed");

        ComPtr<ID3DBlob> vs = CompileShader(shaderPath, "VS_Particle", "vs_5_1");
        ComPtr<ID3DBlob> gs = CompileShader(shaderPath, "GS_Billboard", "gs_5_1");
        ComPtr<ID3DBlob> ps = CompileShader(shaderPath, "PS_Particle", "ps_5_1");

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_drawRootSignature.Get();
        pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pso.GS = {gs->GetBufferPointer(), gs->GetBufferSize()};
        pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.SampleMask = UINT_MAX;
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.DepthStencilState.DepthEnable = TRUE;
        pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        pso.InputLayout = {nullptr, 0};
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pso.SampleDesc.Count = 1;
        ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_drawPso)), "Particle draw PSO failed");
    }
}

void ParticleSystem::ResetAppendCounter(ID3D12GraphicsCommandList* cmd, UINT appendIndex) {
    D3D12_RESOURCE_BARRIER toCopy{};
    toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy.Transition.pResource = m_counterBuffer.Get();
    toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &toCopy);

    cmd->CopyBufferRegion(
        m_counterBuffer.Get(),
        static_cast<UINT64>(appendIndex) * kCounterAlign,
        m_counterZeroUpload.Get(),
        0,
        sizeof(UINT));

    D3D12_RESOURCE_BARRIER toUav = toCopy;
    toUav.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    toUav.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    cmd->ResourceBarrier(1, &toUav);
}

void ParticleSystem::UpdateAndRender(
    ID3D12GraphicsCommandList* cmd,
    UINT frameIndex,
    const XMMATRIX& viewProj,
    FXMVECTOR cameraRight,
    FXMVECTOR cameraUp,
    float deltaSeconds,
    D3D12_CPU_DESCRIPTOR_HANDLE backbufferRtv,
    D3D12_CPU_DESCRIPTOR_HANDLE sceneDsv,
    const D3D12_VIEWPORT& viewport,
    const D3D12_RECT& scissor) {
    if (!m_enabled || !IsValid()) {
        return;
    }

    // Ping-pong + counters are shared across frames — finish previous GPU use first.
    WaitForGpu();

    const UINT frame = frameIndex % kFrameBuffers;
    const float dt = (std::min)((std::max)(deltaSeconds, 0.0f), 0.05f);
    const UINT appendIndex = 1u - m_consumeIndex;

    auto* simCb = reinterpret_cast<SimCB*>(m_mappedSimCbBytes + frame * m_simCbStride);
    simCb->deltaTime = dt;
    simCb->gravity = -18.0f;
    simCb->maxParticles = kMaxParticles;
    simCb->emitterPos = m_emitterPos;

    ResetAppendCounter(cmd, appendIndex);

    cmd->SetPipelineState(m_csPso.Get());
    cmd->SetComputeRootSignature(m_csRootSignature.Get());
    ID3D12DescriptorHeap* uavHeaps[] = {m_uavHeap.Get()};
    cmd->SetDescriptorHeaps(1, uavHeaps);
    cmd->SetComputeRootConstantBufferView(
        0, m_simCb->GetGPUVirtualAddress() + static_cast<UINT64>(frame) * m_simCbStride);

    D3D12_GPU_DESCRIPTOR_HANDLE consumeGpu = m_uavHeap->GetGPUDescriptorHandleForHeapStart();
    consumeGpu.ptr += static_cast<SIZE_T>(m_consumeIndex) * m_descriptorSize;
    D3D12_GPU_DESCRIPTOR_HANDLE appendGpu = m_uavHeap->GetGPUDescriptorHandleForHeapStart();
    appendGpu.ptr += static_cast<SIZE_T>(appendIndex) * m_descriptorSize;
    cmd->SetComputeRootDescriptorTable(1, consumeGpu);
    cmd->SetComputeRootDescriptorTable(2, appendGpu);

    cmd->Dispatch(kMaxParticles / kThreadGroupSize, 1, 1);

    D3D12_RESOURCE_BARRIER uavBarriers[3]{};
    uavBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarriers[0].UAV.pResource = m_buffers[m_consumeIndex].Get();
    uavBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarriers[1].UAV.pResource = m_buffers[appendIndex].Get();
    uavBarriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarriers[2].UAV.pResource = m_counterBuffer.Get();
    cmd->ResourceBarrier(3, uavBarriers);

    D3D12_RESOURCE_BARRIER toSrv{};
    toSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toSrv.Transition.pResource = m_buffers[appendIndex].Get();
    toSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    toSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    toSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &toSrv);

    auto* drawCb = reinterpret_cast<DrawCB*>(m_mappedDrawCbBytes + frame * m_drawCbStride);
    XMStoreFloat4x4(&drawCb->viewProj, XMMatrixTranspose(viewProj));
    XMStoreFloat3(&drawCb->cameraRight, cameraRight);
    XMStoreFloat3(&drawCb->cameraUp, cameraUp);

    cmd->OMSetRenderTargets(1, &backbufferRtv, FALSE, &sceneDsv);
    cmd->RSSetViewports(1, &viewport);
    cmd->RSSetScissorRects(1, &scissor);

    cmd->SetPipelineState(m_drawPso.Get());
    cmd->SetGraphicsRootSignature(m_drawRootSignature.Get());
    ID3D12DescriptorHeap* srvHeaps[] = {m_srvHeap.Get()};
    cmd->SetDescriptorHeaps(1, srvHeaps);
    cmd->SetGraphicsRootConstantBufferView(
        0, m_drawCb->GetGPUVirtualAddress() + static_cast<UINT64>(frame) * m_drawCbStride);

    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    srvGpu.ptr += static_cast<SIZE_T>(appendIndex) * m_descriptorSize;
    cmd->SetGraphicsRootDescriptorTable(1, srvGpu);

    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    cmd->DrawInstanced(kMaxParticles, 1, 0, 0);

    D3D12_RESOURCE_BARRIER toUav{};
    toUav.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toUav.Transition.pResource = m_buffers[appendIndex].Get();
    toUav.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    toUav.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    toUav.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &toUav);

    m_consumeIndex = appendIndex;
    m_workPending = true;
}
