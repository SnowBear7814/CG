#include "Model.h"
#include "Common.h"

#include <DirectXTex.h>

#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace {
void CreateBuffer(
    ID3D12Device* device,
    UINT64 size,
    D3D12_HEAP_TYPE heapType,
    D3D12_RESOURCE_STATES initialState,
    ComPtr<ID3D12Resource>& outResource) {
    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = heapType;

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
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            initialState,
            nullptr,
            IID_PPV_ARGS(&outResource)),
        "CreateCommittedResource(buffer) failed");
}

void CreateSolidTexture(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* commandList,
    uint8_t r,
    uint8_t g,
    uint8_t b,
    uint8_t a,
    ComPtr<ID3D12Resource>& texture,
    ComPtr<ID3D12Resource>& upload,
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu) {
    const UINT width = 1;
    const UINT height = 1;
    const UINT rowPitch = AlignUp(width * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    std::vector<uint8_t> pixels(rowPitch, 0);
    pixels[0] = r;
    pixels[1] = g;
    pixels[2] = b;
    pixels[3] = a;

    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ThrowIfFailed(
        device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &texDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&texture)),
        "Create solid texture failed");

    CreateBuffer(device, rowPitch, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, upload);

    void* mapped = nullptr;
    ThrowIfFailed(upload->Map(0, nullptr, &mapped), "Map solid upload failed");
    std::memcpy(mapped, pixels.data(), pixels.size());
    upload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = 0;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = width;
    src.PlacedFootprint.Footprint.Height = height;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = rowPitch;

    commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &barrier);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(texture.Get(), &srvDesc, srvCpu);
}

bool UploadRgbaTexture(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* commandList,
    const TexMetadata& metadata,
    const Image& sourceImage,
    ComPtr<ID3D12Resource>& texture,
    ComPtr<ID3D12Resource>& upload,
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu) {
    TexMetadata meta = metadata;
    ScratchImage converted;
    const Image* source = &sourceImage;

    if (meta.format != DXGI_FORMAT_R8G8B8A8_UNORM) {
        const HRESULT hr =
            Convert(*source, DXGI_FORMAT_R8G8B8A8_UNORM, TEX_FILTER_DEFAULT, TEX_THRESHOLD_DEFAULT, converted);
        if (FAILED(hr)) {
            return false;
        }
        source = converted.GetImage(0, 0, 0);
        meta.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (!source) {
            return false;
        }
    }

    if (FAILED(CreateTexture(device, meta, texture.ReleaseAndGetAddressOf()))) {
        return false;
    }

    const UINT width = static_cast<UINT>(meta.width);
    const UINT height = static_cast<UINT>(meta.height);
    const UINT rowPitch = AlignUp(width * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const UINT64 uploadSize = static_cast<UINT64>(rowPitch) * height;

    CreateBuffer(device, uploadSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, upload);

    uint8_t* mapped = nullptr;
    ThrowIfFailed(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "Map texture upload failed");
    for (UINT y = 0; y < height; ++y) {
        const uint8_t* srcRow = source->pixels + static_cast<size_t>(y) * source->rowPitch;
        std::memcpy(mapped + static_cast<size_t>(y) * rowPitch, srcRow, static_cast<size_t>(width) * 4);
    }
    upload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = upload.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint.Offset = 0;
    srcLoc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srcLoc.PlacedFootprint.Footprint.Width = width;
    srcLoc.PlacedFootprint.Footprint.Height = height;
    srcLoc.PlacedFootprint.Footprint.Depth = 1;
    srcLoc.PlacedFootprint.Footprint.RowPitch = rowPitch;

    commandList->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &barrier);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = static_cast<UINT>(meta.mipLevels > 0 ? meta.mipLevels : 1);
    device->CreateShaderResourceView(texture.Get(), &srvDesc, srvCpu);
    return true;
}

bool LoadTextureFromFile(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* commandList,
    const std::wstring& path,
    ComPtr<ID3D12Resource>& texture,
    ComPtr<ID3D12Resource>& upload,
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu) {
    if (path.empty()) {
        return false;
    }

    TexMetadata metadata{};
    ScratchImage image;

    HRESULT hr = LoadFromTGAFile(path.c_str(), TGA_FLAGS_NONE, &metadata, image);
    if (FAILED(hr)) {
        hr = LoadFromWICFile(path.c_str(), WIC_FLAGS_NONE, &metadata, image);
    }
    if (FAILED(hr)) {
        return false;
    }

    const Image* source = image.GetImage(0, 0, 0);
    if (!source) {
        return false;
    }
    return UploadRgbaTexture(device, commandList, metadata, *source, texture, upload, srvCpu);
}

void LoadMaterialSlot(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* commandList,
    const std::wstring& path,
    uint8_t fallbackR,
    uint8_t fallbackG,
    uint8_t fallbackB,
    uint8_t fallbackA,
    ComPtr<ID3D12Resource>& texture,
    std::vector<ComPtr<ID3D12Resource>>& uploads,
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu,
    bool& outLoaded) {
    ComPtr<ID3D12Resource> upload;
    outLoaded = LoadTextureFromFile(device, commandList, path, texture, upload, srvCpu);
    if (!outLoaded) {
        CreateSolidTexture(
            device, commandList, fallbackR, fallbackG, fallbackB, fallbackA, texture, upload, srvCpu);
    }
    if (upload) {
        uploads.push_back(upload);
    }
}
} // namespace

void Model::Create(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* commandList,
    const CpuModel& cpuModel,
    ID3D12DescriptorHeap* srvHeap,
    UINT srvDescriptorSize,
    UINT srvStartIndex) {
    m_submeshes = cpuModel.submeshes;
    m_boundsMin = cpuModel.boundsMin;
    m_boundsMax = cpuModel.boundsMax;

    const UINT64 vbSize = sizeof(Vertex) * cpuModel.vertices.size();
    const UINT64 ibSize = sizeof(uint32_t) * cpuModel.indices.size();
    const UINT64 uploadSize = vbSize + ibSize;

    CreateBuffer(device, vbSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, m_vertexBuffer);
    CreateBuffer(device, ibSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, m_indexBuffer);
    CreateBuffer(device, uploadSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, m_uploadBuffer);

    uint8_t* mapped = nullptr;
    ThrowIfFailed(m_uploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "Map mesh upload failed");
    std::memcpy(mapped, cpuModel.vertices.data(), static_cast<size_t>(vbSize));
    std::memcpy(mapped + vbSize, cpuModel.indices.data(), static_cast<size_t>(ibSize));
    m_uploadBuffer->Unmap(0, nullptr);

    commandList->CopyBufferRegion(m_vertexBuffer.Get(), 0, m_uploadBuffer.Get(), 0, vbSize);
    commandList->CopyBufferRegion(m_indexBuffer.Get(), 0, m_uploadBuffer.Get(), vbSize, ibSize);

    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource = m_vertexBuffer.Get();
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource = m_indexBuffer.Get();
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_INDEX_BUFFER;
    barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(2, barriers);

    m_vbv.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
    m_vbv.SizeInBytes = static_cast<UINT>(vbSize);
    m_vbv.StrideInBytes = sizeof(Vertex);

    m_ibv.BufferLocation = m_indexBuffer->GetGPUVirtualAddress();
    m_ibv.SizeInBytes = static_cast<UINT>(ibSize);
    m_ibv.Format = DXGI_FORMAT_R32_UINT;

    m_materials.resize(cpuModel.materials.size());
    for (size_t i = 0; i < cpuModel.materials.size(); ++i) {
        GpuMaterial& gpuMat = m_materials[i];
        gpuMat.alphaCutout = cpuModel.materials[i].alphaCutout;
        gpuMat.roughness = cpuModel.materials[i].roughness;
        gpuMat.metallic = cpuModel.materials[i].metallic;
        gpuMat.ao = cpuModel.materials[i].ao;

        const UINT baseIndex = srvStartIndex + static_cast<UINT>(i) * kTexturesPerMaterial;
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = srvHeap->GetCPUDescriptorHandleForHeapStart();
        cpuHandle.ptr += static_cast<SIZE_T>(baseIndex) * srvDescriptorSize;
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = srvHeap->GetGPUDescriptorHandleForHeapStart();
        gpuHandle.ptr += static_cast<SIZE_T>(baseIndex) * srvDescriptorSize;
        gpuMat.srvCpu = cpuHandle;
        gpuMat.srvGpu = gpuHandle;

        D3D12_CPU_DESCRIPTOR_HANDLE diffuseCpu = cpuHandle;
        D3D12_CPU_DESCRIPTOR_HANDLE normalCpu = cpuHandle;
        normalCpu.ptr += srvDescriptorSize;
        D3D12_CPU_DESCRIPTOR_HANDLE dispCpu = cpuHandle;
        dispCpu.ptr += static_cast<SIZE_T>(2) * srvDescriptorSize;

        // Diffuse → white fallback
        LoadMaterialSlot(
            device,
            commandList,
            cpuModel.materials[i].diffusePath,
            255,
            255,
            255,
            255,
            gpuMat.diffuse,
            m_textureUploads,
            diffuseCpu,
            gpuMat.hasDiffuse);

        // Normal → flat (0.5, 0.5, 1)
        LoadMaterialSlot(
            device,
            commandList,
            cpuModel.materials[i].normalPath,
            128,
            128,
            255,
            255,
            gpuMat.normal,
            m_textureUploads,
            normalCpu,
            gpuMat.hasNormal);
        if (cpuModel.materials[i].normalPath.empty()) {
            gpuMat.hasNormal = false;
        } else {
            // *_gl / ddn → flip Y; *_dx → DirectX normals (no flip).
            std::wstring normLower = cpuModel.materials[i].normalPath;
            for (wchar_t& c : normLower) {
                if (c >= L'A' && c <= L'Z') {
                    c = static_cast<wchar_t>(c - L'A' + L'a');
                }
            }
            gpuMat.normalFlipY =
                normLower.find(L"_gl") != std::wstring::npos ||
                normLower.find(L"ddn") != std::wstring::npos;
        }

        // Displacement → mid-gray
        LoadMaterialSlot(
            device,
            commandList,
            cpuModel.materials[i].displacementPath,
            128,
            128,
            128,
            255,
            gpuMat.displacement,
            m_textureUploads,
            dispCpu,
            gpuMat.hasDisplacement);
        if (cpuModel.materials[i].displacementPath.empty()) {
            gpuMat.hasDisplacement = false;
        }
    }
}

void Model::Shutdown() {
    m_materials.clear();
    m_submeshes.clear();
    m_textureUploads.clear();
    m_uploadBuffer.Reset();
    m_indexBuffer.Reset();
    m_vertexBuffer.Reset();
    m_vbv = {};
    m_ibv = {};
    m_boundsMin = {};
    m_boundsMax = {};
}
