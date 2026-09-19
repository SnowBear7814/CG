#pragma once

#include "ObjLoader.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <vector>

inline constexpr UINT kTexturesPerMaterial = 3; // diffuse, normal, displacement

struct GpuMaterial {
    Microsoft::WRL::ComPtr<ID3D12Resource> diffuse;
    Microsoft::WRL::ComPtr<ID3D12Resource> normal;
    Microsoft::WRL::ComPtr<ID3D12Resource> displacement;
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu{}; // base of 3 consecutive SRVs
    bool hasDiffuse = false;
    bool hasNormal = false;
    bool hasDisplacement = false;
    bool normalFlipY = false; // OpenGL-style normal maps
    bool alphaCutout = false;
    float roughness = 0.5f;
    float metallic = 0.0f;
    float ao = 1.0f;
};

class Model {
public:
    void Create(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* commandList,
        const CpuModel& cpuModel,
        ID3D12DescriptorHeap* srvHeap,
        UINT srvDescriptorSize,
        UINT srvStartIndex);

    void Shutdown();

    D3D12_VERTEX_BUFFER_VIEW GetVertexBufferView() const { return m_vbv; }
    D3D12_INDEX_BUFFER_VIEW GetIndexBufferView() const { return m_ibv; }
    const std::vector<SubMeshDesc>& GetSubmeshes() const { return m_submeshes; }
    const std::vector<GpuMaterial>& GetMaterials() const { return m_materials; }
    DirectX::XMFLOAT3 GetBoundsMin() const { return m_boundsMin; }
    DirectX::XMFLOAT3 GetBoundsMax() const { return m_boundsMax; }
    bool IsValid() const { return m_vertexBuffer != nullptr; }
    UINT GetMaterialSrvCount() const {
        return static_cast<UINT>(m_materials.size()) * kTexturesPerMaterial;
    }

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> m_vertexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_indexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_uploadBuffer;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_textureUploads;

    D3D12_VERTEX_BUFFER_VIEW m_vbv{};
    D3D12_INDEX_BUFFER_VIEW m_ibv{};
    std::vector<SubMeshDesc> m_submeshes;
    std::vector<GpuMaterial> m_materials;
    DirectX::XMFLOAT3 m_boundsMin{};
    DirectX::XMFLOAT3 m_boundsMax{};
};
