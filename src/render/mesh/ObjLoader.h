#pragma once

#include <DirectXMath.h>

#include <cstdint>
#include <string>
#include <vector>

struct Vertex {
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 normal;
    DirectX::XMFLOAT2 uv;
    DirectX::XMFLOAT4 tangent{1.0f, 0.0f, 0.0f, 1.0f}; // xyz + bitangent sign
};

struct MaterialDesc {
    std::string name;
    std::wstring diffusePath;
    std::wstring normalPath;
    std::wstring displacementPath;
    bool alphaCutout = false;
    float roughness = 0.5f;
    float metallic = 0.0f;
    float ao = 1.0f;
};

struct SubMeshDesc {
    uint32_t indexStart = 0;
    uint32_t indexCount = 0;
    uint32_t materialIndex = 0;
    std::string objectName;
};

struct CpuModel {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<MaterialDesc> materials;
    std::vector<SubMeshDesc> submeshes;
    DirectX::XMFLOAT3 boundsMin{};
    DirectX::XMFLOAT3 boundsMax{};
};

CpuModel LoadObjModel(const std::wstring& objPath);
void ComputeTangents(CpuModel& model);
