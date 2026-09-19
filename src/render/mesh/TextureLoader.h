#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <string>

// Load a DDS (2D or cubemap) into a default-heap texture + upload heap.
// Caller must keep upload alive until the GPU finishes the copy.
bool LoadDdsTexture(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* commandList,
    const std::wstring& path,
    Microsoft::WRL::ComPtr<ID3D12Resource>& outTexture,
    Microsoft::WRL::ComPtr<ID3D12Resource>& outUpload,
    bool* outIsCubemap = nullptr);
