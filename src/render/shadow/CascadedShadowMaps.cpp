#include "CascadedShadowMaps.h"
#include "Common.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

void CascadedShadowMaps::ComputePracticalSplits(
    float nearZ,
    float farZ,
    float lambda,
    std::array<float, kCascadeCount>& outSplits) {
    const float n = (std::max)(nearZ, 0.01f);
    const float f = (std::max)(farZ, n + 1.0f);
    const float ratio = f / n;
    const float lam = std::clamp(lambda, 0.0f, 1.0f);

    for (UINT i = 0; i < kCascadeCount; ++i) {
        const float p = static_cast<float>(i + 1) / static_cast<float>(kCascadeCount);
        const float logSplit = n * std::pow(ratio, p);
        const float uniSplit = n + (f - n) * p;
        outSplits[i] = lam * logSplit + (1.0f - lam) * uniSplit;
    }
}

void CascadedShadowMaps::Create(ID3D12Device* device) {
    Shutdown();
    m_dsvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    CreateResources(device);
}

void CascadedShadowMaps::Resize(ID3D12Device* device) {
    if (!device) {
        return;
    }
    Create(device);
}

void CascadedShadowMaps::Shutdown() {
    m_depthArray.Reset();
    m_dsvHeap.Reset();
    m_srvHeap.Reset();
    m_inShaderResourceState = false;
    for (UINT i = 0; i < kCascadeCount; ++i) {
        m_dsvCpu[i] = {};
        m_lightViewProj[i] = {};
    }
    m_srvCpu = {};
    m_srvGpu = {};
    m_cascadeSplits = {};
}

void CascadedShadowMaps::CreateResources(ID3D12Device* device) {
    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = kMapSize;
    texDesc.Height = kMapSize;
    texDesc.DepthOrArraySize = static_cast<UINT16>(kCascadeCount);
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    texDesc.SampleDesc.Count = 1;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1.0f;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ThrowIfFailed(
        device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &texDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &clear,
            IID_PPV_ARGS(&m_depthArray)),
        "Shadow depth array create failed");
    m_inShaderResourceState = false;

    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
    dsvHeapDesc.NumDescriptors = kCascadeCount;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    ThrowIfFailed(device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_dsvHeap)), "Shadow DSV heap failed");

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
    srvHeapDesc.NumDescriptors = 1;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_srvHeap)), "Shadow SRV heap failed");

    D3D12_CPU_DESCRIPTOR_HANDLE dsvStart = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kCascadeCount; ++i) {
        m_dsvCpu[i] = dsvStart;
        m_dsvCpu[i].ptr += static_cast<SIZE_T>(i) * m_dsvDescriptorSize;

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Texture2DArray.ArraySize = 1;
        dsvDesc.Texture2DArray.FirstArraySlice = i;
        dsvDesc.Texture2DArray.MipSlice = 0;
        device->CreateDepthStencilView(m_depthArray.Get(), &dsvDesc, m_dsvCpu[i]);
    }

    m_srvCpu = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    m_srvGpu = m_srvHeap->GetGPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2DArray.MipLevels = 1;
    srvDesc.Texture2DArray.ArraySize = kCascadeCount;
    srvDesc.Texture2DArray.FirstArraySlice = 0;
    device->CreateShaderResourceView(m_depthArray.Get(), &srvDesc, m_srvCpu);
}

void CascadedShadowMaps::UpdateCascades(
    FXMMATRIX cameraView,
    FXMMATRIX cameraProj,
    FXMVECTOR lightDirection,
    float nearZ,
    float cascadeFarZ,
    float lambda,
    const XMFLOAT3& sceneMin,
    const XMFLOAT3& sceneMax) {
    std::array<float, kCascadeCount> splits{};
    ComputePracticalSplits(nearZ, cascadeFarZ, lambda, splits);
    m_cascadeSplits = {splits[0], splits[1], splits[2], splits[3]};

    XMVECTOR lightDir = XMVector3Normalize(lightDirection);
    // Avoid parallel-to-up look-at singularity.
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    if (std::fabs(XMVectorGetX(XMVector3Dot(lightDir, up))) > 0.99f) {
        up = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
    }

    const XMFLOAT3 sceneCenter{
        0.5f * (sceneMin.x + sceneMax.x),
        0.5f * (sceneMin.y + sceneMax.y),
        0.5f * (sceneMin.z + sceneMax.z)};
    const float sceneRadius = 0.5f * std::sqrt(
        (sceneMax.x - sceneMin.x) * (sceneMax.x - sceneMin.x) +
        (sceneMax.y - sceneMin.y) * (sceneMax.y - sceneMin.y) +
        (sceneMax.z - sceneMin.z) * (sceneMax.z - sceneMin.z));

    const XMMATRIX invViewProj = XMMatrixInverse(nullptr, cameraView * cameraProj);

    // NDC corners of unit cube → world (full camera frustum), then rebuild per-slice.
    auto ndcToWorld = [&](float x, float y, float z) -> XMVECTOR {
        XMVECTOR c = XMVectorSet(x, y, z, 1.0f);
        c = XMVector4Transform(c, invViewProj);
        c = XMVectorScale(c, 1.0f / XMVectorGetW(c));
        return c;
    };

    float sliceNear = nearZ;
    for (UINT cascade = 0; cascade < kCascadeCount; ++cascade) {
        const float sliceFar = splits[cascade];

        // Frustum slice corners in view space, then to world.
        // Build by interpolating along camera frustum rays between near and far planes.
        const XMVECTOR nearCorners[4] = {
            ndcToWorld(-1, -1, 0),
            ndcToWorld(+1, -1, 0),
            ndcToWorld(-1, +1, 0),
            ndcToWorld(+1, +1, 0),
        };
        const XMVECTOR farCorners[4] = {
            ndcToWorld(-1, -1, 1),
            ndcToWorld(+1, -1, 1),
            ndcToWorld(-1, +1, 1),
            ndcToWorld(+1, +1, 1),
        };

        // Convert NDC near/far corners to view Z and lerp to sliceNear/sliceFar.
        auto viewZ = [&](XMVECTOR world) {
            return XMVectorGetZ(XMVector3TransformCoord(world, cameraView));
        };

        XMVECTOR slice[8];
        for (int i = 0; i < 4; ++i) {
            const float zn = viewZ(nearCorners[i]);
            const float zf = viewZ(farCorners[i]);
            const float denom = (std::max)(zf - zn, 1e-4f);
            const float t0 = (sliceNear - zn) / denom;
            const float t1 = (sliceFar - zn) / denom;
            slice[i] = XMVectorLerp(nearCorners[i], farCorners[i], t0);
            slice[i + 4] = XMVectorLerp(nearCorners[i], farCorners[i], t1);
        }

        // Light looks toward scene center along lightDir (rays travel +lightDir from eye).
        const XMVECTOR center = XMLoadFloat3(&sceneCenter);
        const XMVECTOR eye = center - lightDir * (sceneRadius * 2.0f + 50.0f);
        const XMMATRIX lightView = XMMatrixLookAtLH(eye, center, up);

        float minX = FLT_MAX, minY = FLT_MAX, minZ = FLT_MAX;
        float maxX = -FLT_MAX, maxY = -FLT_MAX, maxZ = -FLT_MAX;
        for (int i = 0; i < 8; ++i) {
            XMFLOAT3 ls{};
            XMStoreFloat3(&ls, XMVector3TransformCoord(slice[i], lightView));
            minX = (std::min)(minX, ls.x);
            minY = (std::min)(minY, ls.y);
            minZ = (std::min)(minZ, ls.z);
            maxX = (std::max)(maxX, ls.x);
            maxY = (std::max)(maxY, ls.y);
            maxZ = (std::max)(maxZ, ls.z);
        }

        // Expand Z with scene AABB in light space so casters behind the slice still contribute.
        const XMFLOAT3 sceneCorners[8] = {
            {sceneMin.x, sceneMin.y, sceneMin.z},
            {sceneMax.x, sceneMin.y, sceneMin.z},
            {sceneMin.x, sceneMax.y, sceneMin.z},
            {sceneMax.x, sceneMax.y, sceneMin.z},
            {sceneMin.x, sceneMin.y, sceneMax.z},
            {sceneMax.x, sceneMin.y, sceneMax.z},
            {sceneMin.x, sceneMax.y, sceneMax.z},
            {sceneMax.x, sceneMax.y, sceneMax.z},
        };
        for (const auto& sc : sceneCorners) {
            XMFLOAT3 ls{};
            XMStoreFloat3(&ls, XMVector3TransformCoord(XMLoadFloat3(&sc), lightView));
            minZ = (std::min)(minZ, ls.z);
            maxZ = (std::max)(maxZ, ls.z);
        }

        // Pad XY so slice edges / PCF taps stay inside the map.
        const float padX = (maxX - minX) * 0.08f + 2.0f;
        const float padY = (maxY - minY) * 0.08f + 2.0f;
        minX -= padX;
        maxX += padX;
        minY -= padY;
        maxY += padY;

        // Texel snap (stabilize shimmer when camera moves).
        const float worldUnitsPerTexelX = (maxX - minX) / static_cast<float>(kMapSize);
        const float worldUnitsPerTexelY = (maxY - minY) / static_cast<float>(kMapSize);
        if (worldUnitsPerTexelX > 1e-5f) {
            minX = std::floor(minX / worldUnitsPerTexelX) * worldUnitsPerTexelX;
            maxX = std::ceil(maxX / worldUnitsPerTexelX) * worldUnitsPerTexelX;
        }
        if (worldUnitsPerTexelY > 1e-5f) {
            minY = std::floor(minY / worldUnitsPerTexelY) * worldUnitsPerTexelY;
            maxY = std::ceil(maxY / worldUnitsPerTexelY) * worldUnitsPerTexelY;
        }

        constexpr float kZPad = 50.0f;
        const XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(
            minX, maxX, minY, maxY, minZ - kZPad, maxZ + kZPad);
        XMStoreFloat4x4(&m_lightViewProj[cascade], lightView * lightProj);

        sliceNear = sliceFar;
    }
}

void CascadedShadowMaps::BeginCascade(ID3D12GraphicsCommandList* cmd, UINT cascadeIndex) {
    if (cascadeIndex >= kCascadeCount || !m_depthArray) {
        return;
    }

    if (m_inShaderResourceState) {
        TransitionToDepthWrite(cmd);
    }

    D3D12_VIEWPORT vp{};
    vp.Width = static_cast<float>(kMapSize);
    vp.Height = static_cast<float>(kMapSize);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(kMapSize), static_cast<LONG>(kMapSize)};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &scissor);
    cmd->OMSetRenderTargets(0, nullptr, FALSE, &m_dsvCpu[cascadeIndex]);
    cmd->ClearDepthStencilView(m_dsvCpu[cascadeIndex], D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
}

void CascadedShadowMaps::TransitionToShaderResource(ID3D12GraphicsCommandList* cmd) {
    if (!m_depthArray || m_inShaderResourceState) {
        return;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_depthArray.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &barrier);
    m_inShaderResourceState = true;
}

void CascadedShadowMaps::TransitionToDepthWrite(ID3D12GraphicsCommandList* cmd) {
    if (!m_depthArray || !m_inShaderResourceState) {
        return;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_depthArray.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &barrier);
    m_inShaderResourceState = false;
}
