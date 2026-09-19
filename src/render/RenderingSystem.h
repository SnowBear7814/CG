#pragma once

#include "Camera.h"
#include "CascadedShadowMaps.h"
#include "Frustum.h"
#include "GBuffer.h"
#include "Light.h"
#include "Model.h"
#include "Octree.h"
#include "ParticleSystem.h"
#include "PostProcess.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <string>
#include <vector>

// Lab 2–8: deferred + rocks + cull + CSM + particles + post + PBR
class RenderingSystem {
public:
    static constexpr UINT kMaxLights = 16;
    static constexpr UINT kMaxMaterialSrvs = 256;
    static constexpr UINT kFrameBuffers = 2;
    static constexpr int kRockGridX = 6;
    static constexpr int kRockGridZ = 6;

    void Initialize(ID3D12Device* device, UINT width, UINT height);
    void Shutdown();
    void Resize(ID3D12Device* device, UINT width, UINT height);

    void LoadModel(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* commandList,
        const std::wstring& objPath);

    void LoadRocks(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* commandList,
        const std::wstring& objPath);

    void LoadSkybox(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* commandList,
        const std::wstring& ddsPath);

    void ClearSkyboxUpload() { m_skyboxUpload.Reset(); }

    void SetupLightsForScene(
        const DirectX::XMFLOAT3& boundsMin,
        const DirectX::XMFLOAT3& boundsMax,
        float rockRoofY = -1.0f);

    void Render(
        ID3D12GraphicsCommandList* commandList,
        const Camera& camera,
        float timeSeconds,
        float deltaSeconds,
        D3D12_CPU_DESCRIPTOR_HANDLE backbufferRtv,
        UINT width,
        UINT height,
        UINT frameIndex);

    void SetFrustumCullingEnabled(bool enabled) { m_frustumCullingEnabled = enabled; }
    void ToggleFrustumCulling() { m_frustumCullingEnabled = !m_frustumCullingEnabled; }
    bool IsFrustumCullingEnabled() const { return m_frustumCullingEnabled; }

    void SetOctreeCullingEnabled(bool enabled) { m_octreeCullingEnabled = enabled; }
    void ToggleOctreeCulling() { m_octreeCullingEnabled = !m_octreeCullingEnabled; }
    bool IsOctreeCullingEnabled() const { return m_octreeCullingEnabled; }
    UINT GetOctreeNodeCount() const { return m_octree.GetNodeCount(); }

    void ToggleShadows() { m_shadowsEnabled = !m_shadowsEnabled; }
    bool AreShadowsEnabled() const { return m_shadowsEnabled; }

    void ToggleParticles() { m_particles.Toggle(); }
    bool AreParticlesEnabled() const { return m_particles.IsEnabled(); }
    UINT GetParticleCount() const { return m_particles.GetMaxParticles(); }
    void SignalParticles(ID3D12CommandQueue* queue) { m_particles.Signal(queue); }

    void ToggleDof() { m_post.ToggleDof(); }
    void ToggleChromaticAberration() { m_post.ToggleChromatic(); }
    bool IsDofEnabled() const { return m_post.IsDofEnabled(); }
    bool IsChromaticAberrationEnabled() const { return m_post.IsChromaticEnabled(); }

    void TogglePbr() { m_pbrEnabled = !m_pbrEnabled; }
    bool IsPbrEnabled() const { return m_pbrEnabled; }

    bool HasModel() const { return m_model.IsValid(); }
    DirectX::XMFLOAT3 GetModelBoundsMin() const { return m_model.GetBoundsMin(); }
    DirectX::XMFLOAT3 GetModelBoundsMax() const { return m_model.GetBoundsMax(); }
    const std::vector<GpuLight>& GetLights() const { return m_lights; }
    UINT GetGBufferWidth() const { return m_gbuffer.GetWidth(); }
    UINT GetGBufferHeight() const { return m_gbuffer.GetHeight(); }
    UINT GetPostWidth() const { return m_post.GetWidth(); }
    UINT GetPostHeight() const { return m_post.GetHeight(); }
    UINT GetRockInstanceCount() const { return static_cast<UINT>(m_rockInstances.size()); }
    UINT GetRocksDrawnLastFrame() const { return m_rocksDrawnLastFrame; }

private:
    struct GeometryCB {
        DirectX::XMFLOAT4X4 worldViewProj;
    };

    struct ShadowCB {
        DirectX::XMFLOAT4X4 worldLightViewProj;
        float alphaTestEnable = 0.0f;
        float alphaTestCutoff = 0.2f;
        float pad0 = 0.0f;
        float pad1 = 0.0f;
    };

    struct RockObjectCB {
        DirectX::XMFLOAT4X4 world;
        DirectX::XMFLOAT4X4 worldInvTranspose;
        DirectX::XMFLOAT4X4 worldViewProj;
        DirectX::XMFLOAT3 eyePosW;
        float pad0 = 0.0f;
        float dispScale = 0.045f;
        float minTess = 1.0f;
        float maxTess = 5.0f;
        float tessNear = 40.0f;
        float tessFar = 220.0f;
        float hasNormalTexture = 1.0f;
        float hasDispTexture = 1.0f;
        float normalFlipY = 0.0f;
        float roughness = 0.85f;
        float metallic = 0.0f;
        float ao = 1.0f;
        float padPbr = 0.0f;
    };

    struct LightingCB {
        DirectX::XMFLOAT3 cameraPos;
        uint32_t lightCount = 0;
        DirectX::XMFLOAT3 ambient{0.08f, 0.08f, 0.10f};
        float shadowBias = 0.0015f;
        GpuLight lights[kMaxLights]{};
        DirectX::XMFLOAT4X4 lightViewProj[CascadedShadowMaps::kCascadeCount]{};
        DirectX::XMFLOAT4 cascadeSplits{};
        DirectX::XMFLOAT4X4 view{};
        float shadowEnabled = 1.0f;
        float pbrEnabled = 1.0f;
        float skyboxEnabled = 0.0f;
        float padShadow = 0.0f;
        DirectX::XMFLOAT4X4 invViewProj{};
    };

    struct RockInstance {
        DirectX::XMFLOAT4X4 world{};
        Aabb worldBounds{};
    };

    void CreateGeometryPipeline(ID3D12Device* device);
    void CreateRockTessPipeline(ID3D12Device* device);
    void CreateShadowPipeline(ID3D12Device* device);
    void CreateLightingPipeline(ID3D12Device* device);
    void CreateConstantBuffers(ID3D12Device* device);
    void CreateLightingSrvHeap(ID3D12Device* device);
    void UpdateLightingSrvHeap(ID3D12Device* device);
    void BuildRockInstances();
    void BuildRockOctree();
    void CollectVisibleRocks(std::vector<uint32_t>& outVisible);
    void RenderShadowMaps(ID3D12GraphicsCommandList* commandList, const Camera& camera, float aspect);
    DirectX::XMFLOAT3 FindDirectionalLightDirection() const;

    GeometryCB* GeometryCbForFrame();
    LightingCB* LightingCbForFrame();
    D3D12_GPU_VIRTUAL_ADDRESS GeometryCbGpu();
    D3D12_GPU_VIRTUAL_ADDRESS LightingCbGpu();
    uint8_t* RockCbBytesBaseForFrame();
    uint8_t* ShadowCbBytesBaseForFrame();
    D3D12_GPU_VIRTUAL_ADDRESS RockCbGpu(UINT slotInFrame);
    D3D12_GPU_VIRTUAL_ADDRESS ShadowCbGpu(UINT slotInFrame);

    GBuffer m_gbuffer;
    CascadedShadowMaps m_shadowMaps;
    ParticleSystem m_particles;
    PostProcess m_post;
    Model m_model;
    Model m_rockModel;
    std::vector<GpuLight> m_lights;
    std::vector<RockInstance> m_rockInstances;
    std::vector<OctreeItem> m_octreeItems;
    Octree m_octree;
    Frustum m_frustum{};

    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_materialSrvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_lightingSrvHeap;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_geometryRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_geometryPso;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rockRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_rockTessPso;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_shadowRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_shadowPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_shadowAlphaPso;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_lightingRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_lightingPso;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_geometryCb;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_rockCb;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_lightingCb;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_shadowCb;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_skybox;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_skyboxUpload;

    uint8_t* m_mappedGeometryCbBytes = nullptr;
    uint8_t* m_mappedRockCbBytes = nullptr;
    uint8_t* m_mappedLightingCbBytes = nullptr;
    uint8_t* m_mappedShadowCbBytes = nullptr;

    UINT m_geometryCbStride = 256;
    UINT m_lightingCbStride = 256;
    UINT m_rockCbStride = 256;
    UINT m_shadowCbStride = 256;
    UINT m_rockSlotsPerFrame = 0;
    UINT m_shadowSlotsPerFrame = 0;
    UINT m_frameIndex = 0;

    UINT m_srvDescriptorSize = 0;
    UINT m_nextSrvIndex = 0;
    UINT m_width = 0;
    UINT m_height = 0;
    UINT m_rocksDrawnLastFrame = 0;
    bool m_frustumCullingEnabled = true;
    bool m_octreeCullingEnabled = false;
    bool m_shadowsEnabled = true;
    bool m_pbrEnabled = true;
    bool m_skyboxLoaded = false;

    float m_rockDispScale = 0.045f;
    float m_rockMinTess = 1.0f;
    float m_rockMaxTess = 5.0f;
    float m_rockTessNear = 40.0f;
    float m_rockTessFar = 220.0f;
    float m_cascadeLambda = 0.75f;
};
