#include "RenderingSystem.h"
#include "Common.h"
#include "ObjLoader.h"
#include "TextureLoader.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace {
constexpr float kFlowerbedAnimAmp = 0.18f;
constexpr float kFlowerbedAnimSpeed = 2.2f;

ComPtr<ID3DBlob> CompileShader(const std::wstring& path, const char* entry, const char* target) {
    UINT flags = 0;
#if defined(_DEBUG)
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> error;
    const HRESULT hr = D3DCompileFromFile(path.c_str(), nullptr, nullptr, entry, target, flags, 0, &blob, &error);
    if (FAILED(hr)) {
        const char* message = error ? static_cast<const char*>(error->GetBufferPointer()) : "Shader compile failed";
        throw std::runtime_error(message);
    }
    return blob;
}

XMMATRIX ComposeWorldOnFloor(
    const XMFLOAT3& localMin,
    const XMFLOAT3& localMax,
    float uniformScale,
    float rotationY,
    const XMFLOAT3& anchorOnFloor) {
    const XMFLOAT3 pivot{
        (localMin.x + localMax.x) * 0.5f,
        localMin.y,
        (localMin.z + localMax.z) * 0.5f};

    const XMMATRIX scaleRot = XMMatrixTranslation(-pivot.x, -pivot.y, -pivot.z) *
                              XMMatrixScaling(uniformScale, uniformScale, uniformScale) *
                              XMMatrixRotationY(rotationY);

    // Transform local AABB corners roughly via center/extents after scale+rot (Y lift only needs Min.y).
    const XMVECTOR corners[2] = {
        XMVector3TransformCoord(XMLoadFloat3(&localMin), scaleRot),
        XMVector3TransformCoord(XMLoadFloat3(&localMax), scaleRot),
    };
    XMFLOAT3 c0{};
    XMFLOAT3 c1{};
    XMStoreFloat3(&c0, corners[0]);
    XMStoreFloat3(&c1, corners[1]);
    const float scaledMinY = (std::min)(c0.y, c1.y);
    // Also check other AABB corners for rotation.
    float minY = scaledMinY;
    const XMFLOAT3 pts[8] = {
        {localMin.x, localMin.y, localMin.z},
        {localMax.x, localMin.y, localMin.z},
        {localMin.x, localMax.y, localMin.z},
        {localMax.x, localMax.y, localMin.z},
        {localMin.x, localMin.y, localMax.z},
        {localMax.x, localMin.y, localMax.z},
        {localMin.x, localMax.y, localMax.z},
        {localMax.x, localMax.y, localMax.z},
    };
    for (const auto& p : pts) {
        XMFLOAT3 tw{};
        XMStoreFloat3(&tw, XMVector3TransformCoord(XMLoadFloat3(&p), scaleRot));
        minY = (std::min)(minY, tw.y);
    }

    const float liftY = anchorOnFloor.y - minY;
    return scaleRot * XMMatrixTranslation(anchorOnFloor.x, liftY, anchorOnFloor.z);
}

CpuModel MakeWaterPlane(int quads, float size) {
    CpuModel model;
    MaterialDesc mat{};
    mat.name = "water";
    mat.roughness = 0.12f;
    mat.metallic = 0.05f;
    mat.ao = 1.0f;
    model.materials.push_back(mat);

    const int vertsPerSide = quads + 1;
    model.vertices.reserve(static_cast<size_t>(vertsPerSide * vertsPerSide));
    const float half = size * 0.5f;
    for (int z = 0; z < vertsPerSide; ++z) {
        const float tz = static_cast<float>(z) / static_cast<float>(quads);
        const float pz = -half + tz * size;
        for (int x = 0; x < vertsPerSide; ++x) {
            const float tx = static_cast<float>(x) / static_cast<float>(quads);
            const float px = -half + tx * size;
            Vertex v{};
            v.position = {px, 0.0f, pz};
            v.normal = {0.0f, 1.0f, 0.0f};
            v.uv = {tx * 4.0f, tz * 4.0f};
            v.tangent = {1.0f, 0.0f, 0.0f, 1.0f};
            model.vertices.push_back(v);
        }
    }

    model.indices.reserve(static_cast<size_t>(quads * quads * 6));
    for (int z = 0; z < quads; ++z) {
        for (int x = 0; x < quads; ++x) {
            const uint32_t i0 = static_cast<uint32_t>(z * vertsPerSide + x);
            const uint32_t i1 = i0 + 1;
            const uint32_t i2 = i0 + static_cast<uint32_t>(vertsPerSide);
            const uint32_t i3 = i2 + 1;
            model.indices.push_back(i0);
            model.indices.push_back(i2);
            model.indices.push_back(i1);
            model.indices.push_back(i1);
            model.indices.push_back(i2);
            model.indices.push_back(i3);
        }
    }

    SubMeshDesc sm{};
    sm.indexStart = 0;
    sm.indexCount = static_cast<uint32_t>(model.indices.size());
    sm.materialIndex = 0;
    sm.objectName = "water_plane";
    model.submeshes.push_back(sm);

    model.boundsMin = {-half, 0.0f, -half};
    model.boundsMax = {half, 0.0f, half};
    ComputeTangents(model);
    return model;
}
} // namespace

void RenderingSystem::Initialize(ID3D12Device* device, UINT width, UINT height) {
    m_device = device;
    m_width = width;
    m_height = height;
    m_srvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_nextSrvIndex = 0;

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.NumDescriptors = kMaxMaterialSrvs;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(
        device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_materialSrvHeap)),
        "Material SRV heap failed");

    m_gbuffer.Create(device, width, height);
    m_shadowMaps.Create(device);
    m_post.Initialize(device, width, height);
    CreateLightingSrvHeap(device);
    CreateConstantBuffers(device);
    CreateGeometryPipeline(device);
    CreateRockTessPipeline(device);
    CreateWaterTessPipeline(device);
    CreateShadowPipeline(device);
    CreateLightingPipeline(device);
    UpdateLightingSrvHeap(device);
}

void RenderingSystem::Shutdown() {
    if (m_mappedGeometryCbBytes) {
        m_geometryCb->Unmap(0, nullptr);
        m_mappedGeometryCbBytes = nullptr;
    }
    if (m_mappedRockCbBytes) {
        m_rockCb->Unmap(0, nullptr);
        m_mappedRockCbBytes = nullptr;
    }
    if (m_mappedWaterCbBytes) {
        m_waterCb->Unmap(0, nullptr);
        m_mappedWaterCbBytes = nullptr;
    }
    if (m_mappedLightingCbBytes) {
        m_lightingCb->Unmap(0, nullptr);
        m_mappedLightingCbBytes = nullptr;
    }
    if (m_mappedShadowCbBytes) {
        m_shadowCb->Unmap(0, nullptr);
        m_mappedShadowCbBytes = nullptr;
    }

    m_rockInstances.clear();
    m_octreeItems.clear();
    m_octree.Clear();
    m_rockModel.Shutdown();
    m_waterModel.Shutdown();
    m_model.Shutdown();
    m_particles.Shutdown();
    m_post.Shutdown();
    m_shadowMaps.Shutdown();
    m_gbuffer.Shutdown();
    m_skybox.Reset();
    m_skyboxUpload.Reset();
    m_skyboxLoaded = false;
    m_lights.clear();
    m_flyingLights.clear();
    m_sceneLightCount = 0;
    m_shotColorIndex = 0;
    m_nextSrvIndex = 0;

    m_geometryCb.Reset();
    m_rockCb.Reset();
    m_waterCb.Reset();
    m_lightingCb.Reset();
    m_shadowCb.Reset();
    m_geometryPso.Reset();
    m_geometryRootSignature.Reset();
    m_rockTessPso.Reset();
    m_rockRootSignature.Reset();
    m_waterTessPso.Reset();
    m_waterRootSignature.Reset();
    m_shadowPso.Reset();
    m_shadowAlphaPso.Reset();
    m_shadowRootSignature.Reset();
    m_lightingPso.Reset();
    m_lightingRootSignature.Reset();
    m_materialSrvHeap.Reset();
    m_lightingSrvHeap.Reset();
    m_device.Reset();
}

void RenderingSystem::Resize(ID3D12Device* device, UINT width, UINT height) {
    if (width == 0 || height == 0) {
        return;
    }
    m_width = width;
    m_height = height;
    m_gbuffer.Resize(device, width, height);
    m_post.Resize(device, width, height);
    UpdateLightingSrvHeap(device);
}

void RenderingSystem::LoadModel(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* commandList,
    const std::wstring& objPath) {
    m_model.Shutdown();
    m_rockModel.Shutdown();
    m_rockInstances.clear();
    m_nextSrvIndex = 0;

    const CpuModel cpuModel = LoadObjModel(objPath);
    const UINT needed = static_cast<UINT>(cpuModel.materials.size()) * kTexturesPerMaterial;
    if (needed > kMaxMaterialSrvs) {
        throw std::runtime_error("Model has too many materials for SRV heap");
    }

    m_model.Create(
        device,
        commandList,
        cpuModel,
        m_materialSrvHeap.Get(),
        m_srvDescriptorSize,
        m_nextSrvIndex);
    m_nextSrvIndex += m_model.GetMaterialSrvCount();

    m_flowerbedPivotY = 0.0f;
    float flowerbedMinY = FLT_MAX;
    bool foundFlowerbed = false;
    for (const auto& submesh : cpuModel.submeshes) {
        if (submesh.objectName != "sponza_01") {
            continue;
        }
        foundFlowerbed = true;
        for (uint32_t i = 0; i < submesh.indexCount; ++i) {
            const uint32_t vi = cpuModel.indices[submesh.indexStart + i];
            flowerbedMinY = (std::min)(flowerbedMinY, cpuModel.vertices[vi].position.y);
        }
    }
    if (foundFlowerbed) {
        m_flowerbedPivotY = flowerbedMinY;
    }

    SetupLightsForScene(cpuModel.boundsMin, cpuModel.boundsMax);

    // Lab 6: particle fountain in the courtyard
    const XMFLOAT3 emitter{
        0.5f * (cpuModel.boundsMin.x + cpuModel.boundsMax.x),
        cpuModel.boundsMin.y + 525.0f,
        0.5f * (cpuModel.boundsMin.z + cpuModel.boundsMax.z)};
    if (m_particles.IsValid()) {
        m_particles.Shutdown();
    }
    m_particles.SetEmitter(emitter);
    m_particles.Initialize(device, commandList);
}

void RenderingSystem::LoadRocks(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* commandList,
    const std::wstring& objPath) {
    m_rockModel.Shutdown();
    m_rockInstances.clear();

    CpuModel cpuModel = LoadObjModel(objPath);

    // rock_04: diffuse + normal (visual). Displacement must match rock_07 UVs —
    // using rock_04 disp on rock_07 charts opens cracks along UV seams.
    const std::wstring rock04Dir = std::wstring(CONTENT_DIR) + L"/models/rock_04/";
    const std::wstring rock07Tex = std::wstring(CONTENT_DIR) + L"/models/rock_07/textures/";
    for (MaterialDesc& mat : cpuModel.materials) {
        mat.diffusePath = rock04Dir + L"rock_04_diff_1k.jpg";
        mat.normalPath = rock04Dir + L"rock_04_nor_dx_1k.jpg";
        mat.displacementPath = rock07Tex + L"rock_07_disp_1k.jpg";
        mat.roughness = 0.85f;
        mat.metallic = 0.0f;
        mat.ao = 1.0f;
    }

    const UINT needed = m_nextSrvIndex + static_cast<UINT>(cpuModel.materials.size()) * kTexturesPerMaterial;
    if (needed > kMaxMaterialSrvs) {
        throw std::runtime_error("Rock materials exceed SRV heap");
    }

    m_rockModel.Create(
        device,
        commandList,
        cpuModel,
        m_materialSrvHeap.Get(),
        m_srvDescriptorSize,
        m_nextSrvIndex);
    m_nextSrvIndex += m_rockModel.GetMaterialSrvCount();

    BuildRockInstances();
}

void RenderingSystem::LoadWater(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* commandList) {
    m_waterModel.Shutdown();

    CpuModel cpuModel = MakeWaterPlane(16, 160.0f);
    const UINT needed = m_nextSrvIndex + static_cast<UINT>(cpuModel.materials.size()) * kTexturesPerMaterial;
    if (needed > kMaxMaterialSrvs) {
        throw std::runtime_error("Water materials exceed SRV heap");
    }

    m_waterModel.Create(
        device,
        commandList,
        cpuModel,
        m_materialSrvHeap.Get(),
        m_srvDescriptorSize,
        m_nextSrvIndex);
    m_nextSrvIndex += m_waterModel.GetMaterialSrvCount();

    XMStoreFloat4x4(&m_waterWorld, XMMatrixTranslation(800.0f, 200.0f, 0.0f));
}

void RenderingSystem::LoadSkybox(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* commandList,
    const std::wstring& ddsPath) {
    m_skybox.Reset();
    m_skyboxUpload.Reset();
    m_skyboxLoaded = false;

    bool isCubemap = false;
    if (!LoadDdsTexture(device, commandList, ddsPath, m_skybox, m_skyboxUpload, &isCubemap) || !isCubemap) {
        m_skybox.Reset();
        m_skyboxUpload.Reset();
        throw std::runtime_error("Failed to load skybox cubemap DDS");
    }

    m_skyboxLoaded = true;
    UpdateLightingSrvHeap(device);
}

void RenderingSystem::BuildRockInstances() {
    m_rockInstances.clear();
    if (!m_model.IsValid() || !m_rockModel.IsValid()) {
        return;
    }

    const XMFLOAT3 sMin = m_model.GetBoundsMin();
    const XMFLOAT3 sMax = m_model.GetBoundsMax();
    const XMFLOAT3 rMin = m_rockModel.GetBoundsMin();
    const XMFLOAT3 rMax = m_rockModel.GetBoundsMax();

    const float rockExtent = (std::max)(
        (std::max)(rMax.x - rMin.x, rMax.y - rMin.y),
        rMax.z - rMin.z);
    const float sponzaExtent = (std::max)(
        (std::max)(sMax.x - sMin.x, sMax.y - sMin.y),
        sMax.z - sMin.z);

    constexpr float kRockClearanceAboveSponzaTop = 14.0f;
    // Scale rocks relative to Sponza extent so they stay readable in the courtyard.
    const float rockTarget = (std::max)(4.0f, sponzaExtent * 0.04f);
    const float rockScale = (rockExtent > 1e-5f) ? (rockTarget / rockExtent) : 1.0f;
    const float instanceSpacing = (std::max)(rockTarget * 1.08f, 10.0f);
    const float rockFloorY = sMax.y + kRockClearanceAboveSponzaTop;

    const float courtyardX = 0.5f * (sMin.x + sMax.x);
    const float courtyardZ = 0.5f * (sMin.z + sMax.z);

    // Displacement scale is in local space; the world matrix scales it.
    m_rockDispScale = 0.045f;
    m_rockMinTess = 1.0f;
    m_rockMaxTess = 5.0f;
    m_rockTessNear = (std::max)(rockTarget * 3.5f, sponzaExtent * 0.12f);
    m_rockTessFar = (std::max)(sponzaExtent * 0.58f, m_rockTessNear * 4.0f);

    m_rockInstances.reserve(static_cast<size_t>(kRockGridX * kRockGridZ));
    for (int iz = 0; iz < kRockGridZ; ++iz) {
        for (int ix = 0; ix < kRockGridX; ++ix) {
            const float ox = (static_cast<float>(ix) - (kRockGridX - 1) * 0.5f) * instanceSpacing;
            const float oz = (static_cast<float>(iz) - (kRockGridZ - 1) * 0.5f) * instanceSpacing;
            const float yaw = static_cast<float>((ix * 17 + iz * 31) % 360) * (XM_PI / 180.f);
            const XMFLOAT3 anchor{courtyardX + ox, rockFloorY, courtyardZ + oz};

            RockInstance inst{};
            const XMMATRIX world = ComposeWorldOnFloor(rMin, rMax, rockScale, yaw, anchor);
            XMStoreFloat4x4(&inst.world, world);
            TransformAabb(rMin, rMax, world, inst.worldBounds);
            // Pad for displacement / tess (local DispScale * world scale).
            inst.worldBounds.Expand(rockTarget * 0.08f);
            m_rockInstances.push_back(inst);
        }
    }

    BuildRockOctree();

    // Rebuild lights so the directional sits above the rock cluster.
    SetupLightsForScene(sMin, sMax, rockFloorY + rockTarget);
}

void RenderingSystem::BuildRockOctree() {
    m_octreeItems.clear();
    m_octreeItems.reserve(m_rockInstances.size());
    for (uint32_t i = 0; i < static_cast<uint32_t>(m_rockInstances.size()); ++i) {
        OctreeItem item{};
        item.Index = i;
        item.Bounds = m_rockInstances[i].worldBounds;
        m_octreeItems.push_back(item);
    }
    m_octree.Build(m_octreeItems);
}

void RenderingSystem::QueryFrustumVisibleRocks(std::vector<uint32_t>& outVisible) const {
    outVisible.clear();
    const uint32_t count = static_cast<uint32_t>(m_rockInstances.size());
    if (count == 0) {
        return;
    }

    if (m_octreeCullingEnabled) {
        m_octree.QueryFrustum(m_frustum, m_octreeItems, count, outVisible);
        return;
    }

    outVisible.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (m_frustum.IntersectsAabb(m_rockInstances[i].worldBounds)) {
            outVisible.push_back(i);
        }
    }
}

void RenderingSystem::ToggleFrustumLock() {
    if (m_frustumLockEnabled) {
        m_frustumLockEnabled = false;
        m_pendingFrustumLock = false;
        m_lockedVisibleRocks.clear();
        m_lockedRockMask.clear();
        return;
    }
    m_pendingFrustumLock = true;
}

void RenderingSystem::CollectVisibleRocks(std::vector<uint32_t>& outVisible) {
    outVisible.clear();
    const uint32_t count = static_cast<uint32_t>(m_rockInstances.size());
    if (count == 0) {
        return;
    }

    if (m_pendingFrustumLock) {
        QueryFrustumVisibleRocks(m_lockedVisibleRocks);
        m_lockedRockMask.assign(count, 0);
        for (uint32_t idx : m_lockedVisibleRocks) {
            if (idx < count) {
                m_lockedRockMask[idx] = 1;
            }
        }
        m_frustumLockEnabled = true;
        m_pendingFrustumLock = false;
    }

    if (m_frustumLockEnabled) {
        outVisible = m_lockedVisibleRocks;
        return;
    }

    if (!m_frustumCullingEnabled) {
        outVisible.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            outVisible.push_back(i);
        }
        return;
    }

    QueryFrustumVisibleRocks(outVisible);
}

void RenderingSystem::SetupLightsForScene(
    const XMFLOAT3& boundsMin,
    const XMFLOAT3& boundsMax,
    float rockRoofY) {
    m_lights.clear();

    const float cx = 0.5f * (boundsMin.x + boundsMax.x);
    const float cz = 0.5f * (boundsMin.z + boundsMax.z);
    const float roofY = (rockRoofY > 0.0f) ? rockRoofY : (boundsMax.y + 14.0f);

    // Directional from above the rocks (rays travel downward / slightly angled).
    {
        GpuLight sun{};
        sun.type = static_cast<uint32_t>(LightType::Directional);
        sun.position = {cx, roofY + 80.0f, cz};
        sun.direction = {0.25f, -1.0f, 0.15f}; // toward the roof cluster
        sun.color = {1.0f, 0.96f, 0.88f};
        sun.intensity = 4.5f;
        sun.range = 0.0f;
        m_lights.push_back(sun);
    }

    // Fill point lights along the courtyard (Lab 2).
    const float xs[] = {-500.0f, 0.0f, 500.0f};
    const XMFLOAT3 colors[] = {
        {1.0f, 0.45f, 0.25f},
        {0.35f, 0.65f, 1.0f},
        {0.45f, 1.0f, 0.50f},
    };

    for (int i = 0; i < 3; ++i) {
        GpuLight light{};
        light.type = static_cast<uint32_t>(LightType::Point);
        light.position = {xs[i], 500.0f, 0.0f};
        light.color = colors[i];
        light.intensity = 28.0f;
        light.range = 1200.0f;
        m_lights.push_back(light);
    }

    m_sceneLightCount = static_cast<UINT>(m_lights.size());
    ComposeLights();
}

void RenderingSystem::ComposeLights() {
    if (m_lights.size() > m_sceneLightCount) {
        m_lights.resize(m_sceneLightCount);
    }
    for (const FlyingLight& shot : m_flyingLights) {
        if (m_lights.size() >= kMaxLights) {
            break;
        }
        GpuLight light{};
        light.type = static_cast<uint32_t>(LightType::Point);
        light.position = shot.position;
        light.color = shot.color;
        light.intensity = shot.intensity;
        light.range = shot.range;
        m_lights.push_back(light);
    }
}

void RenderingSystem::ShootLight(XMFLOAT3 origin, XMFLOAT3 direction) {
    const XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&direction));
    if (XMVectorGetX(XMVector3LengthSq(dir)) < 1e-8f) {
        return;
    }

    const UINT shotSlots = kMaxLights - m_sceneLightCount;
    if (shotSlots == 0) {
        return;
    }
    if (m_flyingLights.size() >= shotSlots) {
        m_flyingLights.erase(m_flyingLights.begin());
    }

    static const XMFLOAT3 kShotColors[] = {
        {1.00f, 0.35f, 0.20f},
        {0.25f, 0.75f, 1.00f},
        {1.00f, 0.90f, 0.25f},
        {0.85f, 0.35f, 1.00f},
        {0.30f, 1.00f, 0.45f},
        {1.00f, 1.00f, 1.00f},
    };

    constexpr float kSpawnOffset = 18.0f;
    constexpr float kSpeed = 520.0f;

    const XMVECTOR originV = XMLoadFloat3(&origin) + dir * kSpawnOffset;
    FlyingLight shot{};
    XMStoreFloat3(&shot.position, originV);
    XMStoreFloat3(&shot.velocity, dir * kSpeed);
    shot.color = kShotColors[m_shotColorIndex % 6];
    ++m_shotColorIndex;
    shot.intensity = 42.0f;
    shot.range = 520.0f;
    shot.age = 0.0f;
    m_flyingLights.push_back(shot);
    ComposeLights();
}

void RenderingSystem::UpdateShotLights(float deltaSeconds) {
    if (m_flyingLights.empty() || deltaSeconds <= 0.0f) {
        ComposeLights();
        return;
    }

    constexpr float kMaxAge = 25.0f;
    size_t write = 0;
    for (size_t i = 0; i < m_flyingLights.size(); ++i) {
        FlyingLight shot = m_flyingLights[i];
        shot.age += deltaSeconds;
        shot.position.x += shot.velocity.x * deltaSeconds;
        shot.position.y += shot.velocity.y * deltaSeconds;
        shot.position.z += shot.velocity.z * deltaSeconds;
        if (shot.age < kMaxAge) {
            m_flyingLights[write++] = shot;
        }
    }
    m_flyingLights.resize(write);
    ComposeLights();
}

void RenderingSystem::ClearShotLights() {
    m_flyingLights.clear();
    ComposeLights();
}

void RenderingSystem::Render(
    ID3D12GraphicsCommandList* commandList,
    const Camera& camera,
    float timeSeconds,
    float deltaSeconds,
    D3D12_CPU_DESCRIPTOR_HANDLE backbufferRtv,
    UINT width,
    UINT height,
    UINT frameIndex) {
    if (!m_model.IsValid() || width == 0 || height == 0 || !m_gbuffer.IsValid()) {
        return;
    }

    m_frameIndex = frameIndex % kFrameBuffers;
    UpdateShotLights(deltaSeconds);

    const UINT gbWidth = m_gbuffer.GetWidth();
    const UINT gbHeight = m_gbuffer.GetHeight();
    if (gbWidth == 0 || gbHeight == 0) {
        return;
    }

    const float aspect = static_cast<float>(gbWidth) / static_cast<float>(gbHeight);
    const XMMATRIX view = camera.GetViewMatrix();
    const XMMATRIX proj = camera.GetProjectionMatrix(aspect);
    const XMMATRIX viewProj = view * proj;
    GeometryCB* geometryCb = GeometryCbForFrame();
    XMStoreFloat4x4(&geometryCb->worldViewProj, XMMatrixTranspose(viewProj));
    geometryCb->timeSeconds = timeSeconds;

    // Lab 5: cascaded shadow maps (before G-buffer so depth array is ready for lighting)
    if (m_shadowsEnabled && m_shadowMaps.IsValid()) {
        RenderShadowMaps(commandList, camera, aspect, timeSeconds);
    }

    D3D12_VIEWPORT screenViewport{};
    screenViewport.TopLeftX = 0.0f;
    screenViewport.TopLeftY = 0.0f;
    screenViewport.Width = static_cast<float>(gbWidth);
    screenViewport.Height = static_cast<float>(gbHeight);
    screenViewport.MinDepth = 0.0f;
    screenViewport.MaxDepth = 1.0f;
    const D3D12_RECT screenScissor{0, 0, static_cast<LONG>(gbWidth), static_cast<LONG>(gbHeight)};

    m_gbuffer.TransitionToWrite(commandList);
    m_gbuffer.BindAsRenderTargets(commandList);
    m_gbuffer.Clear(commandList);

    commandList->RSSetViewports(1, &screenViewport);
    commandList->RSSetScissorRects(1, &screenScissor);

    // --- Sponza geometry → GBuffer ---
    commandList->SetPipelineState(m_geometryPso.Get());
    commandList->SetGraphicsRootSignature(m_geometryRootSignature.Get());
    ID3D12DescriptorHeap* materialHeaps[] = {m_materialSrvHeap.Get()};
    commandList->SetDescriptorHeaps(1, materialHeaps);
    commandList->SetGraphicsRootConstantBufferView(0, GeometryCbGpu());

    const D3D12_VERTEX_BUFFER_VIEW vbv = m_model.GetVertexBufferView();
    const D3D12_INDEX_BUFFER_VIEW ibv = m_model.GetIndexBufferView();
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->IASetVertexBuffers(0, 1, &vbv);
    commandList->IASetIndexBuffer(&ibv);

    constexpr float kAnimTiling = 3.0f;
    constexpr float kScrollU = 0.18f;
    constexpr float kScrollV = 0.07f;

    const auto& materials = m_model.GetMaterials();
    for (const auto& submesh : m_model.GetSubmeshes()) {
        const GpuMaterial& mat = materials[submesh.materialIndex];
        float matParams[12] = {
            1.0f,
            1.0f,
            0.0f,
            0.0f,
            mat.roughness,
            mat.metallic,
            mat.ao,
            0.0f,
            0.0f,
            0.0f,
            1.0f,
            0.0f};
        if (submesh.objectName == "sponza_320") {
            matParams[0] = kAnimTiling;
            matParams[1] = kAnimTiling;
            matParams[2] = timeSeconds * kScrollU;
            matParams[3] = timeSeconds * kScrollV;
        }
        if (submesh.objectName == "sponza_01") {
            matParams[7] = 1.0f;
            matParams[8] = m_flowerbedPivotY;
            matParams[9] = kFlowerbedAnimAmp;
            matParams[10] = kFlowerbedAnimSpeed;
        }

        commandList->SetGraphicsRoot32BitConstants(1, 12, matParams, 0);
        commandList->SetGraphicsRootDescriptorTable(2, mat.srvGpu);
        commandList->DrawIndexedInstanced(submesh.indexCount, 1, submesh.indexStart, 0, 0);
    }

    // --- Lab 3–4: tessellated rocks + frustum / octree culling ---
    m_rocksDrawnLastFrame = 0;
    if (m_rockModel.IsValid() && !m_rockInstances.empty()) {
        m_frustum.ExtractFromMatrix(viewProj);

        std::vector<uint32_t> visible;
        CollectVisibleRocks(visible);

        commandList->SetPipelineState(m_rockTessPso.Get());
        commandList->SetGraphicsRootSignature(m_rockRootSignature.Get());
        commandList->SetDescriptorHeaps(1, materialHeaps);

        const D3D12_VERTEX_BUFFER_VIEW rockVbv = m_rockModel.GetVertexBufferView();
        const D3D12_INDEX_BUFFER_VIEW rockIbv = m_rockModel.GetIndexBufferView();
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
        commandList->IASetVertexBuffers(0, 1, &rockVbv);
        commandList->IASetIndexBuffer(&rockIbv);

        const auto& rockMaterials = m_rockModel.GetMaterials();
        const XMFLOAT3 eye = camera.GetPosition();
        UINT rockCbSlot = 0;
        uint8_t* rockCbBase = RockCbBytesBaseForFrame();

        for (uint32_t instIdx : visible) {
            const RockInstance& inst = m_rockInstances[instIdx];
            const XMMATRIX world = XMLoadFloat4x4(&inst.world);
            const XMMATRIX worldInvTranspose = XMMatrixTranspose(XMMatrixInverse(nullptr, world));
            const XMMATRIX wvp = world * viewProj;

            for (const auto& submesh : m_rockModel.GetSubmeshes()) {
                if (rockCbSlot >= m_rockSlotsPerFrame) {
                    break;
                }

                const GpuMaterial& mat = rockMaterials[submesh.materialIndex];
                RockObjectCB cb{};
                XMStoreFloat4x4(&cb.world, XMMatrixTranspose(world));
                XMStoreFloat4x4(&cb.worldInvTranspose, XMMatrixTranspose(worldInvTranspose));
                XMStoreFloat4x4(&cb.worldViewProj, XMMatrixTranspose(wvp));
                cb.eyePosW = eye;
                cb.dispScale = m_rockDispScale;
                cb.minTess = m_rockMinTess;
                cb.maxTess = m_rockMaxTess;
                cb.tessNear = m_rockTessNear;
                cb.tessFar = m_rockTessFar;
                cb.hasNormalTexture = mat.hasNormal ? 1.0f : 0.0f;
                cb.hasDispTexture = mat.hasDisplacement ? 1.0f : 0.0f;
                cb.normalFlipY = mat.normalFlipY ? 1.0f : 0.0f;
                cb.roughness = mat.roughness;
                cb.metallic = mat.metallic;
                cb.ao = mat.ao;

                *reinterpret_cast<RockObjectCB*>(rockCbBase + rockCbSlot * m_rockCbStride) = cb;
                commandList->SetGraphicsRootConstantBufferView(0, RockCbGpu(rockCbSlot));
                commandList->SetGraphicsRootDescriptorTable(1, mat.srvGpu);
                commandList->DrawIndexedInstanced(submesh.indexCount, 1, submesh.indexStart, 0, 0);
                ++rockCbSlot;
            }

            ++m_rocksDrawnLastFrame;
        }
    }

    m_gbuffer.TransitionToRead(commandList);
    if (m_shadowsEnabled && m_shadowMaps.IsValid()) {
        m_shadowMaps.TransitionToShaderResource(commandList);
    }

    // Lab 7: lighting + particles → scene RT, then DoF/CA → backbuffer
    m_post.BeginScene(commandList, m_frameIndex);
    const D3D12_CPU_DESCRIPTOR_HANDLE sceneRtv = m_post.GetSceneRtv(m_frameIndex);

    commandList->OMSetRenderTargets(1, &sceneRtv, FALSE, nullptr);
    commandList->RSSetViewports(1, &screenViewport);
    commandList->RSSetScissorRects(1, &screenScissor);

    LightingCB* lightingCb = LightingCbForFrame();
    lightingCb->cameraPos = camera.GetPosition();
    lightingCb->ambient = {0.07f, 0.07f, 0.09f};
    lightingCb->shadowBias = 0.0015f;
    lightingCb->shadowEnabled = (m_shadowsEnabled && m_shadowMaps.IsValid()) ? 1.0f : 0.0f;
    lightingCb->pbrEnabled = m_pbrEnabled ? 1.0f : 0.0f;
    lightingCb->skyboxEnabled = m_skyboxLoaded ? 1.0f : 0.0f;
    lightingCb->lightCount = static_cast<uint32_t>((std::min)(m_lights.size(), static_cast<size_t>(kMaxLights)));
    for (uint32_t i = 0; i < lightingCb->lightCount; ++i) {
        lightingCb->lights[i] = m_lights[i];
    }
    XMStoreFloat4x4(&lightingCb->view, XMMatrixTranspose(view));
    XMStoreFloat4x4(&lightingCb->invViewProj, XMMatrixTranspose(XMMatrixInverse(nullptr, viewProj)));
    lightingCb->cascadeSplits = m_shadowMaps.GetCascadeSplits();
    for (UINT c = 0; c < CascadedShadowMaps::kCascadeCount; ++c) {
        XMStoreFloat4x4(
            &lightingCb->lightViewProj[c],
            XMMatrixTranspose(XMLoadFloat4x4(&m_shadowMaps.GetLightViewProj(c))));
    }

    commandList->SetPipelineState(m_lightingPso.Get());
    commandList->SetGraphicsRootSignature(m_lightingRootSignature.Get());
    ID3D12DescriptorHeap* lightingHeaps[] = {m_lightingSrvHeap.Get()};
    commandList->SetDescriptorHeaps(1, lightingHeaps);
    commandList->SetGraphicsRootConstantBufferView(0, LightingCbGpu());
    commandList->SetGraphicsRootDescriptorTable(1, m_lightingSrvHeap->GetGPUDescriptorHandleForHeapStart());

    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(3, 1, 0, 0);

    // Lab 3 bonus: transparent water after lighting (alpha blend, depth test, no depth write)
    if (m_waterModel.IsValid() && m_waterTessPso && m_mappedWaterCbBytes) {
        const D3D12_CPU_DESCRIPTOR_HANDLE waterDsv = m_gbuffer.GetDsvCpu();
        commandList->OMSetRenderTargets(1, &sceneRtv, FALSE, &waterDsv);
        commandList->RSSetViewports(1, &screenViewport);
        commandList->RSSetScissorRects(1, &screenScissor);

        commandList->SetPipelineState(m_waterTessPso.Get());
        commandList->SetGraphicsRootSignature(m_waterRootSignature.Get());
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);

        const D3D12_VERTEX_BUFFER_VIEW waterVbv = m_waterModel.GetVertexBufferView();
        const D3D12_INDEX_BUFFER_VIEW waterIbv = m_waterModel.GetIndexBufferView();
        commandList->IASetVertexBuffers(0, 1, &waterVbv);
        commandList->IASetIndexBuffer(&waterIbv);

        const XMMATRIX world = XMLoadFloat4x4(&m_waterWorld);
        const XMMATRIX worldInvTranspose = XMMatrixTranspose(XMMatrixInverse(nullptr, world));
        const XMMATRIX wvp = world * viewProj;

        WaterCB* waterCb = WaterCbForFrame();
        XMStoreFloat4x4(&waterCb->world, XMMatrixTranspose(world));
        XMStoreFloat4x4(&waterCb->worldInvTranspose, XMMatrixTranspose(worldInvTranspose));
        XMStoreFloat4x4(&waterCb->worldViewProj, XMMatrixTranspose(wvp));
        waterCb->eyePosW = camera.GetPosition();
        waterCb->time = timeSeconds;
        waterCb->minTess = 6.0f;
        waterCb->maxTess = 24.0f;
        waterCb->tessNear = 80.0f;
        waterCb->tessFar = 900.0f;
        waterCb->waveAmp = 2.4f;
        waterCb->waveFreq = 0.085f;
        waterCb->waveSpeed = 1.6f;
        waterCb->pad = 0.0f;
        commandList->SetGraphicsRootConstantBufferView(0, WaterCbGpu());

        for (const auto& submesh : m_waterModel.GetSubmeshes()) {
            commandList->DrawIndexedInstanced(submesh.indexCount, 1, submesh.indexStart, 0, 0);
        }
    }

    // Lab 6: opaque GPU particles (CS Append/Consume + GS billboards)
    if (m_particles.IsValid() && m_particles.IsEnabled()) {
        const XMVECTOR camRight = camera.RightNormalized();
        const XMVECTOR camForward = camera.ForwardNormalized();
        const XMVECTOR camUp = XMVector3Normalize(XMVector3Cross(camForward, camRight));
        m_particles.UpdateAndRender(
            commandList,
            m_frameIndex,
            viewProj,
            camRight,
            camUp,
            deltaSeconds,
            sceneRtv,
            m_gbuffer.GetDsvCpu(),
            screenViewport,
            screenScissor);
    }

    m_post.Apply(
        commandList,
        m_frameIndex,
        backbufferRtv,
        m_gbuffer.GetDepthResource(),
        Camera::kNearZ,
        Camera::kFarZ,
        // Final blit must cover the swapchain, not a stale/smaller RT viewport.
        D3D12_VIEWPORT{
            0.0f,
            0.0f,
            static_cast<float>(width),
            static_cast<float>(height),
            0.0f,
            1.0f},
        D3D12_RECT{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)});
}

void RenderingSystem::CreateConstantBuffers(ID3D12Device* device) {
    auto createUpload = [&](UINT64 size, ComPtr<ID3D12Resource>& resource, void** mapped) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = AlignUp(static_cast<UINT>(size), 256);
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(
            device->CreateCommittedResource(
                &heap,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&resource)),
            "CB create failed");
        ThrowIfFailed(resource->Map(0, nullptr, mapped), "CB map failed");
    };

    m_geometryCbStride = AlignUp(static_cast<UINT>(sizeof(GeometryCB)), 256);
    m_lightingCbStride = AlignUp(static_cast<UINT>(sizeof(LightingCB)), 256);
    m_rockCbStride = AlignUp(static_cast<UINT>(sizeof(RockObjectCB)), 256);
    m_waterCbStride = AlignUp(static_cast<UINT>(sizeof(WaterCB)), 256);
    m_shadowCbStride = AlignUp(static_cast<UINT>(sizeof(ShadowCB)), 256);

    m_rockSlotsPerFrame = static_cast<UINT>(kRockGridX * kRockGridZ) * 4u;
    // Per-submesh Sponza + rocks × cascades (alpha cutouts need own slots)
    m_shadowSlotsPerFrame = CascadedShadowMaps::kCascadeCount * 256u;

    createUpload(
        static_cast<UINT64>(m_geometryCbStride) * kFrameBuffers,
        m_geometryCb,
        reinterpret_cast<void**>(&m_mappedGeometryCbBytes));
    createUpload(
        static_cast<UINT64>(m_lightingCbStride) * kFrameBuffers,
        m_lightingCb,
        reinterpret_cast<void**>(&m_mappedLightingCbBytes));
    createUpload(
        static_cast<UINT64>(m_rockCbStride) * m_rockSlotsPerFrame * kFrameBuffers,
        m_rockCb,
        reinterpret_cast<void**>(&m_mappedRockCbBytes));
    createUpload(
        static_cast<UINT64>(m_waterCbStride) * kFrameBuffers,
        m_waterCb,
        reinterpret_cast<void**>(&m_mappedWaterCbBytes));
    createUpload(
        static_cast<UINT64>(m_shadowCbStride) * m_shadowSlotsPerFrame * kFrameBuffers,
        m_shadowCb,
        reinterpret_cast<void**>(&m_mappedShadowCbBytes));
}

RenderingSystem::GeometryCB* RenderingSystem::GeometryCbForFrame() {
    return reinterpret_cast<GeometryCB*>(m_mappedGeometryCbBytes + m_frameIndex * m_geometryCbStride);
}

RenderingSystem::LightingCB* RenderingSystem::LightingCbForFrame() {
    return reinterpret_cast<LightingCB*>(m_mappedLightingCbBytes + m_frameIndex * m_lightingCbStride);
}

D3D12_GPU_VIRTUAL_ADDRESS RenderingSystem::GeometryCbGpu() {
    return m_geometryCb->GetGPUVirtualAddress() +
        static_cast<UINT64>(m_frameIndex) * m_geometryCbStride;
}

D3D12_GPU_VIRTUAL_ADDRESS RenderingSystem::LightingCbGpu() {
    return m_lightingCb->GetGPUVirtualAddress() +
        static_cast<UINT64>(m_frameIndex) * m_lightingCbStride;
}

uint8_t* RenderingSystem::RockCbBytesBaseForFrame() {
    return m_mappedRockCbBytes +
        static_cast<size_t>(m_frameIndex) * m_rockSlotsPerFrame * m_rockCbStride;
}

uint8_t* RenderingSystem::ShadowCbBytesBaseForFrame() {
    return m_mappedShadowCbBytes +
        static_cast<size_t>(m_frameIndex) * m_shadowSlotsPerFrame * m_shadowCbStride;
}

D3D12_GPU_VIRTUAL_ADDRESS RenderingSystem::RockCbGpu(UINT slotInFrame) {
    return m_rockCb->GetGPUVirtualAddress() +
        (static_cast<UINT64>(m_frameIndex) * m_rockSlotsPerFrame + slotInFrame) * m_rockCbStride;
}

RenderingSystem::WaterCB* RenderingSystem::WaterCbForFrame() {
    return reinterpret_cast<WaterCB*>(m_mappedWaterCbBytes + m_frameIndex * m_waterCbStride);
}

D3D12_GPU_VIRTUAL_ADDRESS RenderingSystem::WaterCbGpu() {
    return m_waterCb->GetGPUVirtualAddress() +
        static_cast<UINT64>(m_frameIndex) * m_waterCbStride;
}

D3D12_GPU_VIRTUAL_ADDRESS RenderingSystem::ShadowCbGpu(UINT slotInFrame) {
    return m_shadowCb->GetGPUVirtualAddress() +
        (static_cast<UINT64>(m_frameIndex) * m_shadowSlotsPerFrame + slotInFrame) * m_shadowCbStride;
}

void RenderingSystem::CreateGeometryPipeline(ID3D12Device* device) {
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[0].Descriptor.ShaderRegister = 0;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].Constants.ShaderRegister = 1;
    params[1].Constants.Num32BitValues = 12;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &srvRange;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = 3;
    rootDesc.pParameters = params;
    rootDesc.NumStaticSamplers = 1;
    rootDesc.pStaticSamplers = &sampler;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    ThrowIfFailed(
        D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error),
        "Geometry root sig serialize failed");
    ThrowIfFailed(
        device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_geometryRootSignature)),
        "Geometry root sig create failed");

    const std::wstring shaderPath = std::wstring(CONTENT_DIR) + L"/shaders/gbuffer.hlsl";
    ComPtr<ID3DBlob> vs = CompileShader(shaderPath, "VSMain", "vs_5_1");
    ComPtr<ID3DBlob> ps = CompileShader(shaderPath, "PSMain", "ps_5_1");

    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_geometryRootSignature.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.BlendState.RenderTarget[2].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.BlendState.RenderTarget[3].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pso.InputLayout = {inputLayout, _countof(inputLayout)};
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 4;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pso.RTVFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pso.RTVFormats[3] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;

    ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_geometryPso)), "Geometry PSO failed");
}

void RenderingSystem::CreateRockTessPipeline(ID3D12Device* device) {
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 3;
    srvRange.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[0].Descriptor.ShaderRegister = 0;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srvRange;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = 2;
    rootDesc.pParameters = params;
    rootDesc.NumStaticSamplers = 1;
    rootDesc.pStaticSamplers = &sampler;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    ThrowIfFailed(
        D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error),
        "Rock root sig serialize failed");
    ThrowIfFailed(
        device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_rockRootSignature)),
        "Rock root sig create failed");

    const std::wstring shaderPath = std::wstring(CONTENT_DIR) + L"/shaders/rock_tessellation.hlsl";
    ComPtr<ID3DBlob> vs = CompileShader(shaderPath, "VSMain", "vs_5_1");
    ComPtr<ID3DBlob> hs = CompileShader(shaderPath, "HSMain", "hs_5_1");
    ComPtr<ID3DBlob> ds = CompileShader(shaderPath, "DSMain", "ds_5_1");
    ComPtr<ID3DBlob> ps = CompileShader(shaderPath, "PSMain", "ps_5_1");

    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_rockRootSignature.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.HS = {hs->GetBufferPointer(), hs->GetBufferSize()};
    pso.DS = {ds->GetBufferPointer(), ds->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.BlendState.RenderTarget[2].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.BlendState.RenderTarget[3].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // Double-sided rocks: fewer holes after displacement
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pso.InputLayout = {inputLayout, _countof(inputLayout)};
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    pso.NumRenderTargets = 4;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pso.RTVFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pso.RTVFormats[3] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;

    ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_rockTessPso)), "Rock tess PSO failed");
}

void RenderingSystem::CreateWaterTessPipeline(ID3D12Device* device) {
    D3D12_ROOT_PARAMETER params[1]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[0].Descriptor.ShaderRegister = 0;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = 1;
    rootDesc.pParameters = params;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    ThrowIfFailed(
        D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error),
        "Water root sig serialize failed");
    ThrowIfFailed(
        device->CreateRootSignature(
            0,
            signature->GetBufferPointer(),
            signature->GetBufferSize(),
            IID_PPV_ARGS(&m_waterRootSignature)),
        "Water root sig create failed");

    const std::wstring shaderPath = std::wstring(CONTENT_DIR) + L"/shaders/water_tessellation.hlsl";
    ComPtr<ID3DBlob> vs = CompileShader(shaderPath, "VSMain", "vs_5_1");
    ComPtr<ID3DBlob> hs = CompileShader(shaderPath, "HSMain", "hs_5_1");
    ComPtr<ID3DBlob> ds = CompileShader(shaderPath, "DSMain", "ds_5_1");
    ComPtr<ID3DBlob> ps = CompileShader(shaderPath, "PSMain", "ps_5_1");

    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_waterRootSignature.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.HS = {hs->GetBufferPointer(), hs->GetBufferSize()};
    pso.DS = {ds->GetBufferPointer(), ds->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
    pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pso.InputLayout = {inputLayout, _countof(inputLayout)};
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;

    ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_waterTessPso)), "Water tess PSO failed");
}

void RenderingSystem::CreateShadowPipeline(ID3D12Device* device) {
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[0].Descriptor.ShaderRegister = 0;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srvRange;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = 2;
    rootDesc.pParameters = params;
    rootDesc.NumStaticSamplers = 1;
    rootDesc.pStaticSamplers = &sampler;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    ThrowIfFailed(
        D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error),
        "Shadow root sig serialize failed");
    ThrowIfFailed(
        device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_shadowRootSignature)),
        "Shadow root sig create failed");

    const std::wstring shaderPath = std::wstring(CONTENT_DIR) + L"/shaders/shadow_depth.hlsl";
    ComPtr<ID3DBlob> vs = CompileShader(shaderPath, "VSMain", "vs_5_1");
    ComPtr<ID3DBlob> ps = CompileShader(shaderPath, "PSMain", "ps_5_1");

    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    auto makeShadowPso = [&](bool alphaTest, ComPtr<ID3D12PipelineState>& outPso) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_shadowRootSignature.Get();
        pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        if (alphaTest) {
            pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        } else {
            pso.PS = {nullptr, 0};
        }
        pso.SampleMask = UINT_MAX;
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;
        pso.RasterizerState.DepthBias = 1000;
        pso.RasterizerState.SlopeScaledDepthBias = 1.5f;
        pso.RasterizerState.DepthBiasClamp = 0.0f;
        pso.DepthStencilState.DepthEnable = TRUE;
        pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        pso.InputLayout = {inputLayout, _countof(inputLayout)};
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 0;
        pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pso.SampleDesc.Count = 1;
        ThrowIfFailed(
            device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&outPso)),
            alphaTest ? "Shadow alpha PSO failed" : "Shadow PSO failed");
    };

    makeShadowPso(false, m_shadowPso);
    makeShadowPso(true, m_shadowAlphaPso);
}

void RenderingSystem::CreateLightingPipeline(ID3D12Device* device) {
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = GBuffer::kSrvCount + 2; // albedo, normal, pos, ORM, shadow, skybox
    srvRange.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[0].Descriptor.ShaderRegister = 0;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srvRange;

    D3D12_STATIC_SAMPLER_DESC samplers[3]{};
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[0].ShaderRegister = 0;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    samplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[1].ShaderRegister = 1;
    samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    samplers[2].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[2].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[2].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[2].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[2].ShaderRegister = 2;
    samplers[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = 2;
    rootDesc.pParameters = params;
    rootDesc.NumStaticSamplers = 3;
    rootDesc.pStaticSamplers = samplers;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    ThrowIfFailed(
        D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error),
        "Lighting root sig serialize failed");
    ThrowIfFailed(
        device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_lightingRootSignature)),
        "Lighting root sig create failed");

    const std::wstring shaderPath = std::wstring(CONTENT_DIR) + L"/shaders/deferred_lighting_pass.hlsl";
    ComPtr<ID3DBlob> vs = CompileShader(shaderPath, "VSMain", "vs_5_1");
    ComPtr<ID3DBlob> ps = CompileShader(shaderPath, "PSMain", "ps_5_1");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_lightingRootSignature.Get();
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

    ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_lightingPso)), "Lighting PSO failed");
}

void RenderingSystem::CreateLightingSrvHeap(ID3D12Device* device) {
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.NumDescriptors = GBuffer::kSrvCount + 2; // GBuffer + shadow + skybox
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(
        device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_lightingSrvHeap)),
        "Lighting SRV heap failed");
}

void RenderingSystem::UpdateLightingSrvHeap(ID3D12Device* device) {
    if (!device || !m_lightingSrvHeap || !m_gbuffer.IsValid()) {
        return;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE dst = m_lightingSrvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < GBuffer::kSrvCount; ++i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = GBuffer::RtFormat(i);
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(m_gbuffer.GetColorResource(i), &srvDesc, dst);
        dst.ptr += m_srvDescriptorSize;
    }

    if (m_shadowMaps.IsValid()) {
        device->CopyDescriptorsSimple(1, dst, m_shadowMaps.GetSrvCpu(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    dst.ptr += m_srvDescriptorSize;

    if (m_skybox) {
        const D3D12_RESOURCE_DESC skyDesc = m_skybox->GetDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = skyDesc.Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.TextureCube.MipLevels = skyDesc.MipLevels;
        device->CreateShaderResourceView(m_skybox.Get(), &srvDesc, dst);
    }
}

XMFLOAT3 RenderingSystem::FindDirectionalLightDirection() const {
    for (const auto& light : m_lights) {
        if (light.type == static_cast<uint32_t>(LightType::Directional)) {
            return light.direction;
        }
    }
    return {0.25f, -1.0f, 0.15f};
}

void RenderingSystem::RenderShadowMaps(
    ID3D12GraphicsCommandList* commandList,
    const Camera& camera,
    float aspect,
    float timeSeconds) {
    if (!m_shadowMaps.IsValid() || !m_mappedShadowCbBytes) {
        return;
    }

    const XMMATRIX view = camera.GetViewMatrix();
    const XMMATRIX proj = camera.GetProjectionMatrix(aspect);

    XMFLOAT3 sceneMin = m_model.GetBoundsMin();
    XMFLOAT3 sceneMax = m_model.GetBoundsMax();
    for (const auto& inst : m_rockInstances) {
        sceneMin.x = (std::min)(sceneMin.x, inst.worldBounds.Min.x);
        sceneMin.y = (std::min)(sceneMin.y, inst.worldBounds.Min.y);
        sceneMin.z = (std::min)(sceneMin.z, inst.worldBounds.Min.z);
        sceneMax.x = (std::max)(sceneMax.x, inst.worldBounds.Max.x);
        sceneMax.y = (std::max)(sceneMax.y, inst.worldBounds.Max.y);
        sceneMax.z = (std::max)(sceneMax.z, inst.worldBounds.Max.z);
    }

    const XMFLOAT3 lightDir = FindDirectionalLightDirection();
    // Shadow coverage: near camera plane → practical far (scene-sized, not infinite camera far).
    const float extent = (std::max)(
        (std::max)(sceneMax.x - sceneMin.x, sceneMax.y - sceneMin.y),
        sceneMax.z - sceneMin.z);
    // Cover most of the walkable scene so mid/far cascades actually receive shadows.
    const float cascadeFar = (std::min)(Camera::kFarZ, (std::max)(extent * 2.0f, 2000.0f) * 3.0f);

    m_shadowMaps.UpdateCascades(
        view,
        proj,
        XMLoadFloat3(&lightDir),
        Camera::kNearZ,
        cascadeFar,
        m_cascadeLambda,
        sceneMin,
        sceneMax);

    m_shadowMaps.TransitionToDepthWrite(commandList);
    commandList->SetGraphicsRootSignature(m_shadowRootSignature.Get());
    ID3D12DescriptorHeap* materialHeaps[] = {m_materialSrvHeap.Get()};
    commandList->SetDescriptorHeaps(1, materialHeaps);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    UINT shadowSlot = 0;
    uint8_t* shadowCbBase = ShadowCbBytesBaseForFrame();
    bool usingAlphaPso = false;
    commandList->SetPipelineState(m_shadowPso.Get());

    auto pushShadowDraw = [&](const XMMATRIX& world,
                              const D3D12_VERTEX_BUFFER_VIEW& vbv,
                              const D3D12_INDEX_BUFFER_VIEW& ibv,
                              UINT indexCount,
                              UINT startIndex,
                              UINT cascade,
                              bool alphaCutout,
                              D3D12_GPU_DESCRIPTOR_HANDLE diffuseSrv,
                              float vertexAnimEnable = 0.0f,
                              float vertexAnimPivotY = 0.0f) {
        if (shadowSlot >= m_shadowSlotsPerFrame || indexCount == 0) {
            return;
        }

        if (alphaCutout != usingAlphaPso) {
            commandList->SetPipelineState(alphaCutout ? m_shadowAlphaPso.Get() : m_shadowPso.Get());
            usingAlphaPso = alphaCutout;
        }

        const XMMATRIX lightVp = XMLoadFloat4x4(&m_shadowMaps.GetLightViewProj(cascade));
        ShadowCB cb{};
        XMStoreFloat4x4(&cb.worldLightViewProj, XMMatrixTranspose(world * lightVp));
        cb.alphaTestEnable = alphaCutout ? 1.0f : 0.0f;
        cb.alphaTestCutoff = 0.2f;
        cb.vertexAnimEnable = vertexAnimEnable;
        cb.vertexAnimPivotY = vertexAnimPivotY;
        cb.vertexAnimTime = timeSeconds;
        cb.vertexAnimAmp = kFlowerbedAnimAmp;
        cb.vertexAnimSpeed = kFlowerbedAnimSpeed;
        *reinterpret_cast<ShadowCB*>(shadowCbBase + shadowSlot * m_shadowCbStride) = cb;
        commandList->SetGraphicsRootConstantBufferView(0, ShadowCbGpu(shadowSlot));
        if (alphaCutout) {
            commandList->SetGraphicsRootDescriptorTable(1, diffuseSrv);
        }
        commandList->IASetVertexBuffers(0, 1, &vbv);
        commandList->IASetIndexBuffer(&ibv);
        commandList->DrawIndexedInstanced(indexCount, 1, startIndex, 0, 0);
        ++shadowSlot;
    };

    const D3D12_VERTEX_BUFFER_VIEW sponzaVbv = m_model.GetVertexBufferView();
    const D3D12_INDEX_BUFFER_VIEW sponzaIbv = m_model.GetIndexBufferView();
    const auto& sponzaMaterials = m_model.GetMaterials();
    const auto& sponzaSubmeshes = m_model.GetSubmeshes();

    const D3D12_VERTEX_BUFFER_VIEW rockVbv =
        m_rockModel.IsValid() ? m_rockModel.GetVertexBufferView() : D3D12_VERTEX_BUFFER_VIEW{};
    const D3D12_INDEX_BUFFER_VIEW rockIbv =
        m_rockModel.IsValid() ? m_rockModel.GetIndexBufferView() : D3D12_INDEX_BUFFER_VIEW{};
    const UINT rockIndexCount = rockIbv.SizeInBytes / sizeof(uint32_t);

    Frustum lightFrustum{};

    for (UINT cascade = 0; cascade < CascadedShadowMaps::kCascadeCount; ++cascade) {
        m_shadowMaps.BeginCascade(commandList, cascade);
        lightFrustum.ExtractFromMatrix(XMLoadFloat4x4(&m_shadowMaps.GetLightViewProj(cascade)));

        for (const auto& submesh : sponzaSubmeshes) {
            const GpuMaterial& mat = sponzaMaterials[submesh.materialIndex];
            const bool isFlowerbed = submesh.objectName == "sponza_01";
            pushShadowDraw(
                XMMatrixIdentity(),
                sponzaVbv,
                sponzaIbv,
                submesh.indexCount,
                submesh.indexStart,
                cascade,
                mat.alphaCutout,
                mat.srvGpu,
                isFlowerbed ? 1.0f : 0.0f,
                isFlowerbed ? m_flowerbedPivotY : 0.0f);
        }

        if (m_rockModel.IsValid() && rockIndexCount > 0) {
            for (uint32_t instIdx = 0; instIdx < static_cast<uint32_t>(m_rockInstances.size()); ++instIdx) {
                if (m_frustumLockEnabled &&
                    (instIdx >= m_lockedRockMask.size() || !m_lockedRockMask[instIdx])) {
                    continue;
                }
                const RockInstance& inst = m_rockInstances[instIdx];
                if (!lightFrustum.IntersectsAabb(inst.worldBounds)) {
                    continue;
                }
                pushShadowDraw(
                    XMLoadFloat4x4(&inst.world),
                    rockVbv,
                    rockIbv,
                    rockIndexCount,
                    0,
                    cascade,
                    false,
                    {});
            }
        }
    }
}
