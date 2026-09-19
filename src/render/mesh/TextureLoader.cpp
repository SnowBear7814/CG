#include "TextureLoader.h"
#include "Common.h"

#include <DirectXTex.h>

#include <algorithm>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace {
void UploadSubresources(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* destination,
    ID3D12Resource* upload,
    UINT firstSubresource,
    UINT numSubresources,
    const D3D12_SUBRESOURCE_DATA* srcData) {
    const D3D12_RESOURCE_DESC desc = destination->GetDesc();
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(numSubresources);
    std::vector<UINT> numRows(numSubresources);
    std::vector<UINT64> rowSizesInBytes(numSubresources);
    UINT64 requiredSize = 0;
    device->GetCopyableFootprints(
        &desc,
        firstSubresource,
        numSubresources,
        0,
        layouts.data(),
        numRows.data(),
        rowSizesInBytes.data(),
        &requiredSize);

    uint8_t* mapped = nullptr;
    ThrowIfFailed(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "Map DDS upload failed");

    for (UINT i = 0; i < numSubresources; ++i) {
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& layout = layouts[i];
        const D3D12_SUBRESOURCE_DATA& src = srcData[i];
        BYTE* pDestSlice = mapped + layout.Offset;

        for (UINT z = 0; z < layout.Footprint.Depth; ++z) {
            BYTE* pDestData = pDestSlice + static_cast<SIZE_T>(layout.Footprint.RowPitch) * numRows[i] * z;
            const BYTE* pSrcData =
                static_cast<const BYTE*>(src.pData) + static_cast<SIZE_T>(src.SlicePitch) * z;
            for (UINT y = 0; y < numRows[i]; ++y) {
                std::memcpy(
                    pDestData + static_cast<SIZE_T>(layout.Footprint.RowPitch) * y,
                    pSrcData + static_cast<SIZE_T>(src.RowPitch) * y,
                    static_cast<size_t>(rowSizesInBytes[i]));
            }
        }
    }
    upload->Unmap(0, nullptr);

    for (UINT i = 0; i < numSubresources; ++i) {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = destination;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = firstSubresource + i;

        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = upload;
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint = layouts[i];

        commandList->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
    }
}

void Transition(
    ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &barrier);
}
} // namespace

bool LoadDdsTexture(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* commandList,
    const std::wstring& path,
    ComPtr<ID3D12Resource>& outTexture,
    ComPtr<ID3D12Resource>& outUpload,
    bool* outIsCubemap) {
    if (!device || !commandList || path.empty()) {
        return false;
    }

    ScratchImage image;
    TexMetadata metadata{};
    if (FAILED(LoadFromDDSFile(path.c_str(), DDS_FLAGS_NONE, &metadata, image))) {
        return false;
    }

    const bool isCubemap = metadata.IsCubemap() != 0;
    if (outIsCubemap) {
        *outIsCubemap = isCubemap;
    }

    // Match PCG importer: create in COMMON, then barrier to COPY_DEST before upload.
    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = (metadata.dimension == TEX_DIMENSION_TEXTURE3D)
        ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
        : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = metadata.width;
    texDesc.Height = static_cast<UINT>(metadata.height);
    texDesc.DepthOrArraySize = static_cast<UINT16>(
        metadata.dimension == TEX_DIMENSION_TEXTURE3D ? metadata.depth : metadata.arraySize);
    texDesc.MipLevels = static_cast<UINT16>(metadata.mipLevels);
    texDesc.Format = metadata.format;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    outTexture.Reset();
    outUpload.Reset();

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &texDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&outTexture)))) {
        return false;
    }

    std::vector<D3D12_SUBRESOURCE_DATA> subresources;
    if (FAILED(PrepareUpload(
            device, image.GetImages(), image.GetImageCount(), metadata, subresources)) ||
        subresources.empty()) {
        outTexture.Reset();
        return false;
    }

    const UINT numSubresources = static_cast<UINT>(subresources.size());
    const UINT expected = static_cast<UINT>(metadata.mipLevels) *
        static_cast<UINT>(
            metadata.dimension == TEX_DIMENSION_TEXTURE3D ? metadata.depth : metadata.arraySize);
    if (numSubresources != expected) {
        outTexture.Reset();
        return false;
    }

    UINT64 uploadSize = 0;
    device->GetCopyableFootprints(&texDesc, 0, numSubresources, 0, nullptr, nullptr, nullptr, &uploadSize);
    if (uploadSize == 0) {
        outTexture.Reset();
        return false;
    }

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bufferDesc{};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = uploadSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&outUpload)))) {
        outTexture.Reset();
        return false;
    }

    Transition(commandList, outTexture.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    UploadSubresources(
        device, commandList, outTexture.Get(), outUpload.Get(), 0, numSubresources, subresources.data());
    Transition(
        commandList,
        outTexture.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
}
