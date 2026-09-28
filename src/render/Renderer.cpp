#include "Renderer.h"
#include "Common.h"

#include <stdexcept>

using Microsoft::WRL::ComPtr;

bool Renderer::Initialize(HWND hwnd, UINT width, UINT height) {
    m_width = width;
    m_height = height;

    UINT dxgiFactoryFlags = 0;
#if defined(_DEBUG)
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
#endif

    ComPtr<IDXGIFactory4> factory;
    ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2 failed");

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            continue;
        }
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr))) {
            break;
        }
        adapter.Reset();
    }

    if (!adapter) {
        ThrowIfFailed(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "EnumWarpAdapter failed");
    }

    ThrowIfFailed(
        D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)),
        "D3D12CreateDevice failed");

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)), "CreateCommandQueue failed");

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
    swapChainDesc.BufferCount = kFrameCount;
    swapChainDesc.Width = width;
    swapChainDesc.Height = height;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> swapChain;
    ThrowIfFailed(
        factory->CreateSwapChainForHwnd(m_commandQueue.Get(), hwnd, &swapChainDesc, nullptr, nullptr, &swapChain),
        "CreateSwapChainForHwnd failed");
    ThrowIfFailed(factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER), "MakeWindowAssociation failed");
    ThrowIfFailed(swapChain.As(&m_swapChain), "QueryInterface IDXGISwapChain3 failed");
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.NumDescriptors = kFrameCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)), "Create RTV heap failed");
    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    for (UINT i = 0; i < kFrameCount; ++i) {
        ThrowIfFailed(
            m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocators[i])),
            "CreateCommandAllocator failed");
    }

    ThrowIfFailed(
        m_device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            m_commandAllocators[m_frameIndex].Get(),
            nullptr,
            IID_PPV_ARGS(&m_commandList)),
        "CreateCommandList failed");
    ThrowIfFailed(m_commandList->Close(), "Close command list failed");

    ThrowIfFailed(
        m_device->CreateFence(m_fenceValues[m_frameIndex], D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)),
        "CreateFence failed");
    m_fenceValues[m_frameIndex]++;
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) {
        throw std::runtime_error("CreateEvent failed");
    }

    CreateRenderTargetViews();
    m_renderingSystem.Initialize(m_device.Get(), width, height);
    return true;
}

void Renderer::Shutdown() {
    if (m_commandQueue && m_fence) {
        WaitForGpu();
    }

    m_renderingSystem.Shutdown();

    if (m_fenceEvent) {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }

    for (UINT i = 0; i < kFrameCount; ++i) {
        m_renderTargets[i].Reset();
        m_commandAllocators[i].Reset();
        m_fenceValues[i] = 0;
    }

    m_commandList.Reset();
    m_rtvHeap.Reset();
    m_swapChain.Reset();
    m_commandQueue.Reset();
    m_fence.Reset();
    m_device.Reset();
}

void Renderer::Resize(UINT width, UINT height) {
    if (!m_swapChain || width == 0 || height == 0 || (width == m_width && height == m_height)) {
        return;
    }

    WaitForGpu();

    for (UINT i = 0; i < kFrameCount; ++i) {
        m_renderTargets[i].Reset();
        m_fenceValues[i] = m_fenceValues[m_frameIndex];
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    m_swapChain->GetDesc(&desc);
    ThrowIfFailed(
        m_swapChain->ResizeBuffers(kFrameCount, width, height, desc.BufferDesc.Format, desc.Flags),
        "ResizeBuffers failed");

    m_width = width;
    m_height = height;
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    CreateRenderTargetViews();
    m_renderingSystem.Resize(m_device.Get(), width, height);
}

void Renderer::LoadModel(const std::wstring& objPath) {
    WaitForGpu();

    ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset(), "CommandAllocator Reset failed");
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr), "CommandList Reset failed");

    m_renderingSystem.LoadModel(m_device.Get(), m_commandList.Get(), objPath);

    ThrowIfFailed(m_commandList->Close(), "Close upload command list failed");
    ID3D12CommandList* lists[] = {m_commandList.Get()};
    m_commandQueue->ExecuteCommandLists(1, lists);
    WaitForGpu();
}

void Renderer::LoadRocks(const std::wstring& objPath) {
    WaitForGpu();

    ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset(), "CommandAllocator Reset failed");
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr), "CommandList Reset failed");

    m_renderingSystem.LoadRocks(m_device.Get(), m_commandList.Get(), objPath);

    ThrowIfFailed(m_commandList->Close(), "Close rock upload command list failed");
    ID3D12CommandList* lists[] = {m_commandList.Get()};
    m_commandQueue->ExecuteCommandLists(1, lists);
    WaitForGpu();
}

void Renderer::LoadWater() {
    WaitForGpu();

    ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset(), "CommandAllocator Reset failed");
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr), "CommandList Reset failed");

    m_renderingSystem.LoadWater(m_device.Get(), m_commandList.Get());

    ThrowIfFailed(m_commandList->Close(), "Close water upload command list failed");
    ID3D12CommandList* lists[] = {m_commandList.Get()};
    m_commandQueue->ExecuteCommandLists(1, lists);
    WaitForGpu();
}

void Renderer::LoadSkybox(const std::wstring& ddsPath) {
    WaitForGpu();

    ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset(), "CommandAllocator Reset failed");
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr), "CommandList Reset failed");

    m_renderingSystem.LoadSkybox(m_device.Get(), m_commandList.Get(), ddsPath);

    ThrowIfFailed(m_commandList->Close(), "Close skybox upload command list failed");
    ID3D12CommandList* lists[] = {m_commandList.Get()};
    m_commandQueue->ExecuteCommandLists(1, lists);
    WaitForGpu();
    m_renderingSystem.ClearSkyboxUpload();
}

void Renderer::DrawFrame(const Camera& camera, float timeSeconds, float deltaSeconds) {
    if (m_width == 0 || m_height == 0) {
        return;
    }

    // Keep GBuffer + post scene RT locked to swapchain (prevents top-left quarter view).
    if (m_renderingSystem.GetGBufferWidth() != m_width ||
        m_renderingSystem.GetGBufferHeight() != m_height ||
        m_renderingSystem.GetPostWidth() != m_width ||
        m_renderingSystem.GetPostHeight() != m_height) {
        WaitForGpu();
        m_renderingSystem.Resize(m_device.Get(), m_width, m_height);
    }

    ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset(), "CommandAllocator Reset failed");
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr), "CommandList Reset failed");

    D3D12_RESOURCE_BARRIER toRtv{};
    toRtv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRtv.Transition.pResource = m_renderTargets[m_frameIndex].Get();
    toRtv.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toRtv.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toRtv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &toRtv);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += static_cast<SIZE_T>(m_frameIndex) * m_rtvDescriptorSize;

    const float clearColor[] = {0.45f, 0.55f, 0.70f, 1.0f};
    m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    m_renderingSystem.Render(
        m_commandList.Get(),
        camera,
        timeSeconds,
        deltaSeconds,
        rtvHandle,
        m_width,
        m_height,
        m_frameIndex);

    D3D12_RESOURCE_BARRIER toPresent{};
    toPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toPresent.Transition.pResource = m_renderTargets[m_frameIndex].Get();
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    toPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &toPresent);

    ThrowIfFailed(m_commandList->Close(), "CommandList Close failed");
    ID3D12CommandList* lists[] = {m_commandList.Get()};
    m_commandQueue->ExecuteCommandLists(1, lists);
    m_renderingSystem.SignalParticles(m_commandQueue.Get());
    ThrowIfFailed(m_swapChain->Present(1, 0), "Present failed");
    MoveToNextFrame();
}

void Renderer::WaitForGpu() {
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), m_fenceValues[m_frameIndex]), "Signal failed");
    ThrowIfFailed(
        m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent),
        "SetEventOnCompletion failed");
    WaitForSingleObject(m_fenceEvent, INFINITE);
    m_fenceValues[m_frameIndex]++;
}

void Renderer::MoveToNextFrame() {
    const UINT64 currentFenceValue = m_fenceValues[m_frameIndex];
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), currentFenceValue), "Signal failed");

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex]) {
        ThrowIfFailed(
            m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent),
            "SetEventOnCompletion failed");
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }

    m_fenceValues[m_frameIndex] = currentFenceValue + 1;
}

void Renderer::CreateRenderTargetViews() {
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; ++i) {
        ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i])), "GetBuffer failed");
        m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += m_rtvDescriptorSize;
    }
}
