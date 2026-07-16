// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

#include <algorithm>
#include <chrono>
#include <shellscalingapi.h>

#include "MemoryManagerGPU-DirectX12.h"
#include "RenderPage2D-DirectX12.h"
#include "UserInterface-DirectX12.h"
#include "विश्वकर्मा.h"

#include "colors.h"
#include "RenderCompositor.h"
#include "RenderCompositor-DirectX12.h"

extern शंकर gpu;
extern std::vector<std::thread> renderThreads;
extern std::atomic<uint64_t> atlasFence;
extern std::shared_mutex monitorMutex;
extern HINSTANCE hInst;
extern int primaryMonitorIndex;

// Reverse deps: defined in Main.cpp, called by the moved window-lifecycle code.
float GetTopRibbonHeightPxForWindow(const SingleUIWindow* window);
void PushSystemTodoToTab(DATASETTAB* tab, ACTION_TYPE actionType, int x = 0,
    int y = 0, int delta = 0, uint64_t objectId = 0, uint64_t auxValue = 0);

// DirectX related codes:

void PrintHResult(int i) {
    HRESULT reason = gpu.device->GetDeviceRemovedReason();
    std::cerr << "HResult Check " << i << ": ";
    std::cerr << "Device Removed Reason: ";
    switch (reason) {
    case S_OK:
        std::cerr << "S_OK (Device NOT removed?)";
        break;
    case DXGI_ERROR_DEVICE_REMOVED:
        std::cerr << "DXGI_ERROR_DEVICE_REMOVED";
        break;
    case DXGI_ERROR_DEVICE_HUNG:
        std::cerr << "DXGI_ERROR_DEVICE_HUNG (Bad GPU commands)";
        break;
    case DXGI_ERROR_DEVICE_RESET:
        std::cerr << "DXGI_ERROR_DEVICE_RESET";
        break;
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
        std::cerr << "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
        break;
    case DXGI_ERROR_INVALID_CALL:
        std::cerr << "DXGI_ERROR_INVALID_CALL (API misuse)";
        break;
    default:
        std::cerr << "Unknown HRESULT: 0x"
            << std::hex << reason << std::dec;
        break;
    }
    std::cerr << std::endl;
}

void GpuRenderThread(int monitorId, int refreshRate) {
    // Our architecture is 1 GPU Render thread per Monitor. 
    std::wcout << "Render Thread (Monitor " << monitorId << ", " << refreshRate << "Hz) started." << std::endl;
    auto lastFpsTime = std::chrono::high_resolution_clock::now();
    int frameCount = 0;
    UINT64 allocatorFenceValues[FRAMES_PER_RENDERTARGETS] = { 0 };

    // Ge the persistent Command Queue for this monitor. Do NOT create a new queue.
    ID3D12CommandQueue* pCommandQueue = gpu.screens[monitorId].commandQueue.Get();

    // Initialize Thread-Local Resources (CommandQueue/Allocator/CommandList)
    // Command queue is per monitor to enable different refresh rate monitors to operate independently.
    // Otherwise present on slower monitor would block present on faster monitor.
    DX12ResourcesPerRenderThread threadRes;

    // We just store the pointer for convenience, we don't own it (ComPtr assignment adds ref)
    threadRes.commandQueue = pCommandQueue;

    // Create one allocator per frame-in-flight (Double Buffering)
    for (UINT i = 0; i < FRAMES_PER_RENDERTARGETS; i++) {
        ThrowIfFailed(gpu.device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&threadRes.commandAllocators[i])));
    }
    // Create the Command List (Only 1 needed, we reset it repeatedly)
    ThrowIfFailed(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        threadRes.commandAllocators[0].Get(), nullptr, // Pipeline state set later
        IID_PPV_ARGS(&threadRes.commandList)));

    // Command lists are created in the recording state, but our loop expects them closed initially.
    threadRes.commandList->Close();

    uint64_t lastRenderedFrame = 0;
    const auto frameDuration = std::chrono::milliseconds(1000 / refreshRate);

    // Create synchronization objects
    ThrowIfFailed(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&threadRes.fence)));
    UINT64 currentFenceValue = 0; // TODO: Is it OK to start separate render threads with same 0 value ?
    threadRes.fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    while (!shutdownSignal && !pauseRenderThreads) {
        auto frameStart = std::chrono::high_resolution_clock::now();

        // Reset Thread Allocator & List.We must do this outside the window loop,
        // since we send the command list of all windows in one go to the GPU, to render all windows on this monitor.
        auto& allocator = threadRes.commandAllocators[threadRes.allocatorIndex];
        allocator->Reset();
        threadRes.commandList->Reset(allocator.Get(), nullptr); // Pass 'nullptr' for the PSO here so we don't enforce a state yet.

        bool didRender = false;
        // Picks recorded this frame; promoted to in-flight (with the frame's fence) after Signal.
        std::vector<PickPassContext*> pendingPickCtxs;

        // Do not move outside the while loop since, main thread could have created new windows or moved some.
        uint16_t* windowList = publishedWindowIndexes.load(std::memory_order_acquire);
        uint16_t windowCount = publishedWindowCount.load(std::memory_order_acquire);

        for (uint16_t wi = 0; wi < windowCount; ++wi) {
            SingleUIWindow& window = allWindows[windowList[wi]];

            // Check if user have migrated this windows to a different monitor. This is marked by UI thread.
            uint32_t state = window.migrationState.load(std::memory_order_acquire);
            if (state == 1 && window.currentMonitorIndex == monitorId) {
                std::wcout << "Source thread releasing window\n";
                // Wait GPU idle
                UINT64 waitValue = gpu.screens[window.currentMonitorIndex].renderFenceValue;
                auto& screen = gpu.screens[window.currentMonitorIndex];
                if (screen.renderFence->GetCompletedValue() < waitValue) {
                    screen.renderFence->SetEventOnCompletion(waitValue, screen.renderFenceEvent);
                    WaitForSingleObject(screen.renderFenceEvent, INFINITE);
                }
                window.isMigrating = true;
                gpu.CleanupWindowResources(window.dx);
                window.migrationState.store(2, std::memory_order_release);
                continue;
            }

            // Check if any other render thread released this thread for migration and we need to acquired it.
            else if (state == 2 && window.requestedMonitorIndex == monitorId) {
                uint32_t expected = 2;
                if (!window.migrationState.compare_exchange_strong(expected, 3, std::memory_order_acq_rel))
                    continue;

                int newMonitor = window.requestedMonitorIndex;
                std::wcout << "Destination thread acquiring window\n";

                gpu.InitD3DPerWindow(window.dx, window.hWnd, gpu.screens[newMonitor].commandQueue.Get());
                window.currentMonitorIndex = newMonitor;
                window.isMigrating = false;
                window.migrationState.store(0, std::memory_order_release);
                std::wcout << "Migration complete\n";
                continue;
            }

            if (window.currentMonitorIndex != monitorId) continue; // Skip the windows not on this monitor.
            // The Safety Switch: If migrating, pretend this window doesn't exist for now.
            if (window.isMigrating) continue;

            // Resize handling. Render thread might have signalled windows resize. Trigger it if required.
            if (window.resizeState.load(std::memory_order_acquire) == 1) {
                uint32_t reqW = window.nextRequestedWidth.load(std::memory_order_relaxed);
                uint32_t reqH = window.nextRequestedHeight.load(std::memory_order_relaxed);
                if (reqW > 0 && reqH > 0 && (reqW != window.currentWidth || reqH != window.currentHeight)) {
                    window.isResizing.store(true, std::memory_order_release);
                    // This is the ONLY place ResizeD3DWindow is now called
                    gpu.ResizeD3DWindow(window.dx, reqW, reqH);
                    window.currentWidth = reqW;
                    window.currentHeight = reqH;
                    window.resizeState.store(0, std::memory_order_release);
                    window.isResizing.store(false, std::memory_order_release);
                }
                else { window.resizeState.store(0, std::memory_order_release); }
            }

            if (window.isResizing) continue;
            if (!window.dx.swapChain) continue;

            // TODO: Ideally, it should be handled in WM_MOVE or nearby. Following is simply safeguard for bugs elsewhere.
            // Check if the window is physically on this monitor, but chemically bound to another queue
            if (window.dx.swapChain && window.dx.creatorQueue != threadRes.commandQueue.Get()) {
                std::wcout << "Monitor Mismatch detected! Recreating SwapChain for new Queue." << std::endl;
                // Ensure the GPU is done with the OLD queue resources before destroying them
                // (In a production engine, you would use a fence wait here on the OLD queue)
                gpu.WaitForPreviousFrame(threadRes);
                gpu.CleanupWindowResources(window.dx);// Clean up resources tied to the old queue
                // Re-initialize resources on the CURRENT thread's queue
                // Note: We assume 'window.hwnd' is accessible here. If not, add it to SingleUIWindow struct.
                gpu.InitD3DPerWindow(window.dx, window.hWnd, threadRes.commandQueue.Get());
            }

            DX12ResourcesPerWindow& winRes = window.dx;// Get Window Resources (Swap chain, RTV)

            // CONTEXT SWITCHING. Set the Viewport/Scissor for THIS window (Critical!)
            threadRes.commandList->RSSetViewports(1, &winRes.viewport);
            threadRes.commandList->RSSetScissorRects(1, &winRes.scissorRect);

            auto barrierStart = CD3DX12_RESOURCE_BARRIER::Transition(winRes.renderTextures[winRes.frameIndex].Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            threadRes.commandList->ResourceBarrier(1, &barrierStart);

            CD3DX12_CPU_DESCRIPTOR_HANDLE rttHandle(winRes.rttRtvHeap->GetCPUDescriptorHandleForHeapStart(),
                winRes.frameIndex, gpu.rtvDescriptorSize);
            CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(winRes.rtvHeap->GetCPUDescriptorHandleForHeapStart(),
                winRes.frameIndex, gpu.rtvDescriptorSize);
            CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(winRes.dsvHeap->GetCPUDescriptorHandleForHeapStart());

            threadRes.commandList->OMSetRenderTargets(1, &rttHandle, FALSE, &dsvHandle);

            const float clearColor[] = { kSceneSkyTopR, kSceneSkyTopG, kSceneSkyTopB, 1.0f };// Clear
            threadRes.commandList->ClearRenderTargetView(rttHandle, clearColor, 0, nullptr);
            threadRes.commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

            // GET TAB DATA & RECORD GEOMETRY
            // Also Sets the Unique Root Signature for THIS window. Root Signature and Pipeline State are per-window.
            // In fact the Populate Command List can change it multiple times per window if needed.
            int tabIndex = window.activeTabIndex;

            // TAKE SNAPSHOT HERE (eliminates all race conditions)
            UIInput inputSnapshot = window.uiInput;   // cheap copy (struct is ~100 bytes)
            // Reset the "this-frame" flags for next frame (main thread will set them again)
            window.uiInput.leftButtonPressedThisFrame = false;
            window.uiInput.leftButtonReleasedThisFrame = false;
            window.uiInput.leftButtonDoubleClickedThisFrame = false;
            window.uiInput.rightButtonPressedThisFrame = false;
            window.uiInput.middleButtonPressedThisFrame = false;
            window.uiInput.mouseWheelDelta = 0;
            window.uiInput.textInputCount = 0;

            if (tabIndex >= 0) {
                DATASETTAB& tab = allTabs[tabIndex];
                DX12ResourcesPerTab& tabRes = tab.dx;
                // Scene-3D selection context (populated in the 3D branch below, used after the UI).
                bool scene3DActive = false;
                DirectX::XMMATRIX sceneViewProj = DirectX::XMMatrixIdentity();
                int sceneTopUI = 0, sceneVpW = 0, sceneVpH = 0;
                const bool contentOnlyWindow = window.windowKind == WINDOW_KIND_VIEW;
                // Resolve which sub-tab/view this window displays (platform-agnostic compositor
                // logic, RenderCompositor.cpp). Camera is written back every frame as before.
                const WindowViewTarget viewTarget = ResolveWindowViewTarget(window, tab);
                const uint64_t activeInternalSubTabMemoryId = viewTarget.containerMemoryId;
                const VishwakarmaStorage::ObjectType activeInternalSubTabType =
                    viewTarget.containerType;
                const int renderSlot = viewTarget.renderSlot;
                tabRes.camera = viewTarget.camera;
                const bool isInputViewWindow = viewTarget.isInputViewWindow;
                uint64_t fenceToWaitFor = gpu.copyFenceValue.load(std::memory_order_acquire);// Cross-Queue Sync.
                //if (fenceToWaitFor > 0) { threadRes.commandQueue->Wait(gpu.copyFence.Get(), fenceToWaitFor); }
                //Above is commented out because render thread now no longer need to wait for copyFence,
                //because, now render thread operate over READ ONLY page list.
                if (activeInternalSubTabType == VishwakarmaStorage::ObjectType::Page2D && tab.cad2d) {
                    RenderPage2D(threadRes.commandList.Get(), winRes, *tab.cad2d,
                        gpu.uiResources, monitorId, activeInternalSubTabMemoryId, renderSlot);
                }
                else {
                    ClearSceneSkyGradient(threadRes.commandList.Get(), winRes, rttHandle, monitorId);
                    gpu.RenderScene3D(threadRes.commandList.Get(), winRes, tabRes, tab.geometry,
                        monitorId, activeInternalSubTabMemoryId);// Renders geometry.

                    // Selection highlight + rotation-cube overlay (still inside the scene RTV/DSV +
                    // scene viewport bound by RenderScene3D, so this draws before the UI).
                    scene3DActive = true;
                    sceneTopUI = SceneTopUIHeightPx(monitorId, winRes);
                    sceneVpW = winRes.WindowWidth;
                    sceneVpH = winRes.WindowHeight - sceneTopUI;
                    if (sceneVpH > 0) {
                        DirectX::XMVECTOR eyeP = DirectX::XMLoadFloat3(&tabRes.camera.position);
                        DirectX::XMVECTOR focusP = DirectX::XMLoadFloat3(&tabRes.camera.target);
                        DirectX::XMVECTOR upP = DirectX::XMLoadFloat3(&tabRes.camera.up);
                        DirectX::XMMATRIX viewM = DirectX::XMMatrixLookAtLH(eyeP, focusP, upP);
                        const float aspect = static_cast<float>(winRes.WindowWidth) /
                            static_cast<float>(sceneVpH);
                        DirectX::XMMATRIX projM = DirectX::XMMatrixPerspectiveFovLH(
                            tabRes.camera.fov, aspect, tabRes.camera.nearZ, tabRes.camera.farZ);
                        sceneViewProj = viewM * projM; // Matches RenderScene3D's view-proj.
                        if (isInputViewWindow) {
                            RecordSelectionOverlays(threadRes.commandList.Get(), winRes, tabRes,
                                tab.geometry, tab.selection, sceneViewProj, sceneTopUI, sceneVpW,
                                sceneVpH, activeInternalSubTabMemoryId);
                        }
                    }
                }
                // Render User Interface using safe snapshot copy of current Input.

                if (contentOnlyWindow ||
                    atlasFence.load(std::memory_order_acquire) == 0 ||
                    gpu.copyFence->GetCompletedValue() < atlasFence.load()) {
                    ; // atlas not ready (or extracted view window: no ribbon/bands) → skip UI
                }
                else {
                    // Restore full window viewport & scissor before rendering UI overlay so UI is not clipped
                    threadRes.commandList->RSSetViewports(1, &winRes.viewport);
                    threadRes.commandList->RSSetScissorRects(1, &winRes.scissorRect);
                    RenderUIOverlay(window, threadRes.commandList.Get(), gpu.uiResources,
                        gpu.screens[monitorId].topRibbonLayout,
                        static_cast<float>(gpu.screens[monitorId].physicalDpiX),
                        static_cast<float>(gpu.screens[monitorId].physicalDpiY),
                        inputSnapshot, activeInternalSubTabMemoryId);
                }

                // Service any pending click/scroll pick request. Runs after the UI (it renders into
                // its own targets) and records a readback tagged with this frame's fence below.
                // Only the input view's window services picks: its camera/viewport match the click.
                if (scene3DActive && sceneVpH > 0 && isInputViewWindow) {
                    ServicePick(threadRes.commandList.Get(), winRes, tabRes, tab.geometry,
                        tab.selection, tabRes.pickCtx, monitorId, sceneViewProj, sceneTopUI,
                        sceneVpW, sceneVpH, activeInternalSubTabMemoryId);
                    if (tabRes.pickCtx.state == PickPassContext::State::Recorded) {
                        pendingPickCtxs.push_back(&tabRes.pickCtx);
                    }
                }
            }

            // Transition RTT: PIXEL_SHADER_RESOURCE → COPY_SOURCE
            auto rttToCopySource = CD3DX12_RESOURCE_BARRIER::Transition(winRes.renderTextures[winRes.frameIndex].Get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            threadRes.commandList->ResourceBarrier(1, &rttToCopySource);

            // Transition BackBuffer: PRESENT → COPY_DEST
            auto bbToCopyDest = CD3DX12_RESOURCE_BARRIER::Transition(winRes.renderTargets[winRes.frameIndex].Get(),
                D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
            threadRes.commandList->ResourceBarrier(1, &bbToCopyDest);

            // Copy RTT → BackBuffer
            threadRes.commandList->CopyResource(winRes.renderTargets[winRes.frameIndex].Get(), // DEST
                winRes.renderTextures[winRes.frameIndex].Get()); // SRC

            // Transition BackBuffer: COPY_DEST → PRESENT
            auto bbToPresent = CD3DX12_RESOURCE_BARRIER::Transition(winRes.renderTargets[winRes.frameIndex].Get(),
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
            threadRes.commandList->ResourceBarrier(1, &bbToPresent);

            // (Optional) Transition RTT back to SRV for next frame
            auto rttBackToSRV = CD3DX12_RESOURCE_BARRIER::Transition(winRes.renderTextures[winRes.frameIndex].Get(),
                D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            threadRes.commandList->ResourceBarrier(1, &rttBackToSRV);

            didRender = true;
        } // End of loop over all windows on this monitor.

        // Close & Execute ONCE after recording all windows.
        // TODO: Future Optimization: Spawn separate thread for each recording command list of each window. Synchronization will be complex though.
        threadRes.commandList->Close();
        if (didRender) {
            ID3D12CommandList* ppCommandLists[] = { threadRes.commandList.Get() };
            threadRes.commandQueue->ExecuteCommandLists(1, ppCommandLists);

            // Present ALL windows and wait
            /*  The first parameter 1 enables VSync!This, tells the GPU to wait for the monitor's vertical blank interval before presenting the frame
            Synchronize frame presentation with the display's refresh rate. It Throttle application to match the monitor's Hz
            This is more energy efficient way. We are engineering application, not some 1st person shooter video game maximizing fps !
            Without VSync, it was going 650fps(FullHD) on Laptop GPU with 25 Pyramid only geometry.
            */

            for (uint16_t wi = 0; wi < windowCount; ++wi) {
                SingleUIWindow& window = allWindows[windowList[wi]];

                if (window.currentMonitorIndex != monitorId) continue;
                if (window.isResizing) continue;
                if (window.isMigrating) continue;
                HRESULT hr = window.dx.swapChain->Present(1, 0);
                if (FAILED(hr)) { std::cerr << "Present failed: " << hr << std::endl; }
                // Do NOT Handle Fences here. It will be handled after all windows are presented.

                // Update THIS window's specific buffer index. Update the frame index immediately so the NEXT loop uses the correct buffer.
                window.dx.frameIndex = window.dx.swapChain->GetCurrentBackBufferIndex();
            }

            // SIGNAL the fence for the current frame
            currentFenceValue = gpu.renderFenceValue.fetch_add(1);
            threadRes.commandQueue->Signal(threadRes.fence.Get(), currentFenceValue);
            //tabRes.lastRenderFenceValue.store(currentFenceValue, std::memory_order_release);
            // Mirror into the globally-visible per-monitor fence so PruneOneRetiredPage can see it.
            threadRes.commandQueue->Signal(gpu.screens[monitorId].renderFence.Get(), currentFenceValue);
            gpu.screens[monitorId].renderFenceValue = currentFenceValue; // Submitted value. Not necessarily executed.

            // Tag picks recorded this frame with the fence they must wait on before readback.
            for (PickPassContext* pctx : pendingPickCtxs) FinalizePickFence(*pctx, currentFenceValue);

            // WAIT: Throttle CPU (Double Buffering Logic). We need to reuse the Command Allocator for the *next* frame.
            // Store the exact fence value we just signaled for THIS allocator
            allocatorFenceValues[threadRes.allocatorIndex] = currentFenceValue;
            // Advance to the next allocator (0 -> 1 -> 0 -> 1). This is separate from the window's back buffer index!
            threadRes.allocatorIndex = (threadRes.allocatorIndex + 1) % FRAMES_PER_RENDERTARGETS;
            // WAIT: Throttle CPU (Double Buffering Logic). Wait for the NEXT allocator to be free.
            const UINT64 fenceValueToWaitFor = allocatorFenceValues[threadRes.allocatorIndex];
            if (fenceValueToWaitFor > 0 && threadRes.fence->GetCompletedValue() < fenceValueToWaitFor) {
                threadRes.fence->SetEventOnCompletion(fenceValueToWaitFor, threadRes.fenceEvent);
                WaitForSingleObject(threadRes.fenceEvent, INFINITE);
            }
        }
        else { // Idle handling
            // If I have no windows, I just wait (maybe for a VSync or sleep). Maybe we are running without a monitor !
            // This fulfills "waiting for some windows to be dragged into it" ?
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }

#ifdef _DEBUG
        // Update FPS counter (Debug build only)
        // FPS calculation variables - add these as global or class members
        static const double FPS_REPORT_INTERVAL = 10.0; // Report every 10 seconds
        frameCount++;
        auto currentTime = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration<double>(currentTime - lastFpsTime).count();
        if (elapsed >= FPS_REPORT_INTERVAL) {
            double fps = frameCount / elapsed;
            std::wcout << L"FPS: " << std::fixed << std::setprecision(2) << fps
                << L" (" << frameCount << L" frames in "
                << std::setprecision(1) << elapsed << L" seconds)" << std::endl;
            frameCount = 0;// Reset counters
            lastFpsTime = currentTime;

            // 2D render-state heartbeat: what each tab's Page2D snapshot holds on the GPU and
            // where its input view is looking. Bridges "records ingested" and "pixels visible".
            uint16_t* fpsTabList = publishedTabIndexes.load(std::memory_order_acquire);
            const uint16_t fpsTabCount = publishedTabCount.load(std::memory_order_acquire);
            for (uint16_t t = 0; t < fpsTabCount; ++t) {
                DATASETTAB& fpsTab = allTabs[fpsTabList[t]];
                if (!fpsTab.cad2d) continue;
                Cad2DPageSnapshot* snapshot =
                    fpsTab.cad2d->activeSnapshot.load(std::memory_order_acquire);
                if (!snapshot || snapshot->pages.empty()) continue;
                const int inputSlot = InputViewSlot(fpsTab);
                const int viewSlot = (inputSlot >= 0 && inputSlot < MV_MAX_SUBTABS) ? inputSlot : 0;
                const Cad2DViewState& view = fpsTab.cad2d->views[viewSlot];
                std::cout << "[cad2d][dbg] tab=" << fpsTabList[t]
                          << " inputSlot=" << inputSlot
                          << " view center=(" << view.centerXCU.load(std::memory_order_acquire)
                          << ", " << view.centerYCU.load(std::memory_order_acquire)
                          << ") zoom=" << view.zoomPixelsPerCU.load(std::memory_order_acquire)
                          << " gpuPages:";
                for (const Cad2DPageGPU* page : snapshot->pages) {
                    if (!page) continue;
                    std::cout << " {container=" << page->containerMemoryId
                              << " lines=" << page->lineCount
                              << " curves=" << page->curveCount
                              << " textIdx=" << page->textIndexCount << "}";
                }
                std::cout << std::endl;
            }
        }
#endif

        // VRAM is completely managed by COPY thread. Hence this thread need not release any memory release.
    }

    // Cleanup Thread-Local Resources. We cannot destroy resources currently being read by the GPU!
    const UINT64 fenceValueToWaitFor = currentFenceValue;

    // If the GPU hasn't reached that point yet, we sleep the CPU thread.
    if (threadRes.fence->GetCompletedValue() < fenceValueToWaitFor) {
        threadRes.fence->SetEventOnCompletion(fenceValueToWaitFor, threadRes.fenceEvent);
        //At this cleanup stage, do not wait INFINITE. Otherwise we may get stuck waiting forever.
        DWORD waitResult = WaitForSingleObject(threadRes.fenceEvent, 5000);  // 5 sec timeout
        if (waitResult != WAIT_OBJECT_0) {
            std::cerr << "Render thread cleanup Fence wait timed out!\n" << std::endl;
            // Force exit or log
        }
    }

    // Close Synchronization Handles
    if (threadRes.fenceEvent) {
        CloseHandle(threadRes.fenceEvent);
        threadRes.fenceEvent = nullptr;
    }

    threadRes.commandQueue.Reset();// Command Objects cleanup.
    threadRes.fence.Reset();
    std::wcout << "Render Thread (Monitor " << monitorId << ") shutting down.\n" << std::endl;
}

void शंकर::InitD3DPerWindow(DX12ResourcesPerWindow& dx, HWND hwnd, ID3D12CommandQueue* commandQueue) {
    int i = 0; // Latter to be iterated over number of screens.
    dx.creatorQueue = commandQueue; // Track which queue this windows was created with. To assist with migrations.

    // commandAllocator is per render thread (i.e. per monitor), not per window.
    // gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&dx.commandAllocator));

    // Create swap chain
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = FRAMES_PER_RENDERTARGETS;
    swapChainDesc.Width = dx.WindowWidth;
    swapChainDesc.Height = dx.WindowHeight;
    swapChainDesc.Format = gpu.rttFormat;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> tempSwapChain;
    ThrowIfFailed(factory6->CreateSwapChainForHwnd(commandQueue, hwnd, &swapChainDesc,
        nullptr, nullptr, &tempSwapChain));

    tempSwapChain.As(&dx.swapChain);
    dx.frameIndex = dx.swapChain->GetCurrentBackBufferIndex();

    dx.viewport = CD3DX12_VIEWPORT(0.0f, 0.0f,
        static_cast<float>(dx.WindowWidth), static_cast<float>(dx.WindowHeight));
    dx.scissorRect = CD3DX12_RECT(0, 0, dx.WindowWidth, dx.WindowHeight);

    // Create descriptor heap for render target views
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = FRAMES_PER_RENDERTARGETS; //One RTV per frame buffer for multi-buffering support
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(gpu.device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&dx.rtvHeap)));

    // Create depth stencil descriptor heap
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(gpu.device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&dx.dsvHeap)));

    // Create depth stencil buffer
    D3D12_CLEAR_VALUE depthOptimizedClearValue = {};
    depthOptimizedClearValue.Format = DXGI_FORMAT_D32_FLOAT;
    depthOptimizedClearValue.DepthStencil.Depth = 1.0f;
    depthOptimizedClearValue.DepthStencil.Stencil = 0;

    auto depthHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto depthResourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_D32_FLOAT, dx.WindowWidth, dx.WindowHeight,
        1, 0, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);

    ThrowIfFailed(gpu.device->CreateCommittedResource(
        &depthHeapProps, D3D12_HEAP_FLAG_NONE,
        &depthResourceDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &depthOptimizedClearValue, IID_PPV_ARGS(&dx.depthStencilBuffer)
    ));
    //Observe that depthStencilBuffer has been created on D3D12_HEAP_TYPE_DEFAULT, i.e. main GPU memory
    //dsvHeap is created on D3D12_DESCRIPTOR_HEAP_TYPE_DSV. 
    //All DESCRIPTOR HEAPs are stored on Small Fast gpu memory. It is normal D3D12 design.

    gpu.device->CreateDepthStencilView(dx.depthStencilBuffer.Get(), nullptr,
        dx.dsvHeap->GetCPUDescriptorHandleForHeapStart());

    // Create render target views
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(dx.rtvHeap->GetCPUDescriptorHandleForHeapStart());
    for (UINT j = 0; j < FRAMES_PER_RENDERTARGETS; j++) {
        dx.swapChain->GetBuffer(j, IID_PPV_ARGS(&dx.renderTargets[j]));
        gpu.device->CreateRenderTargetView(dx.renderTargets[j].Get(), nullptr, rtvHandle);
        rtvHandle.Offset(1, gpu.rtvDescriptorSize);
    }

    // Check for any device removal reasons before we start creating resources. This can help catch issues early in initialization.
    PrintHResult(1000);

    // CREATE RENDER TEXTURES
    D3D12_DESCRIPTOR_HEAP_DESC rttRtvHeapDesc = {};
    rttRtvHeapDesc.NumDescriptors = FRAMES_PER_RENDERTARGETS;
    rttRtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(gpu.device->CreateDescriptorHeap(&rttRtvHeapDesc, IID_PPV_ARGS(&dx.rttRtvHeap)));

    D3D12_DESCRIPTOR_HEAP_DESC rttSrvHeapDesc = {};
    rttSrvHeapDesc.NumDescriptors = FRAMES_PER_RENDERTARGETS;
    rttSrvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    rttSrvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(gpu.device->CreateDescriptorHeap(&rttSrvHeapDesc, IID_PPV_ARGS(&dx.rttSrvHeap)));

    CD3DX12_CPU_DESCRIPTOR_HANDLE rttRtvHandle(dx.rttRtvHeap->GetCPUDescriptorHandleForHeapStart());
    CD3DX12_CPU_DESCRIPTOR_HANDLE rttSrvHandle(dx.rttSrvHeap->GetCPUDescriptorHandleForHeapStart());
    UINT cbvSrvUavDescriptorSize = gpu.cbvSrvUavDescriptorSize;

    //Gemini placed clearValue  / texDesc outside the loop. ChatGPT placed it inside the loop. 
    //Since clearValue doesn't change per iteration, we can optimize by defining it once outside the loop.
    auto texDesc = CD3DX12_RESOURCE_DESC::Tex2D(gpu.rttFormat, dx.WindowWidth, dx.WindowHeight,
        1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    D3D12_CLEAR_VALUE clearValue{ .Format = gpu.rttFormat,
        .Color = {kSceneSkyTopR, kSceneSkyTopG, kSceneSkyTopB, 1.0f} }; //C++20 allows this beauty!

    for (UINT i = 0; i < FRAMES_PER_RENDERTARGETS; i++) {
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        ThrowIfFailed(gpu.device->CreateCommittedResource( // Create the Resource in RENDER_TARGET state by default
            &heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &clearValue, IID_PPV_ARGS(&dx.renderTextures[i])));

        gpu.device->CreateRenderTargetView(dx.renderTextures[i].Get(), nullptr, rttRtvHandle); // Create RTV
        rttRtvHandle.Offset(1, gpu.rtvDescriptorSize);

        // Create SRV (For passing into Pixel Shader later). Can we create this struct also outside the loop ?
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = gpu.rttFormat;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        gpu.device->CreateShaderResourceView(dx.renderTextures[i].Get(), &srvDesc, rttSrvHandle);
        rttSrvHandle.Offset(1, cbvSrvUavDescriptorSize); //Gemini 3 Pro
    }

    // Create constant buffer descriptor heap
    D3D12_DESCRIPTOR_HEAP_DESC cbvHeapDesc = {};
    cbvHeapDesc.NumDescriptors = 1;
    cbvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    cbvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    ThrowIfFailed(gpu.device->CreateDescriptorHeap(&cbvHeapDesc, IID_PPV_ARGS(&dx.cbvHeap)));

    // Create constant buffer
    /* Our HLSL constant buffer contains: float4x4 worldViewProjection; 64 bytes (4x4 floats = 16*4=64)
    float4x4 world; 64 bytes. Total: 128 bytes, but padded to 256 bytes for D3D12 alignment requirements */
    auto cbHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto cbResourceDesc = CD3DX12_RESOURCE_DESC::Buffer(256); // Constant buffers must be 256-byte aligned

    ThrowIfFailed(gpu.device->CreateCommittedResource(&cbHeapProps, D3D12_HEAP_FLAG_NONE,
        &cbResourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&dx.constantBuffer)));

    // Create constant buffer view
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    cbvDesc.BufferLocation = dx.constantBuffer->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = 256;
    gpu.device->CreateConstantBufferView(&cbvDesc, dx.cbvHeap->GetCPUDescriptorHandleForHeapStart());

    // Map constant buffer i.e. Map Constant Buffer for CPU Access
    CD3DX12_RANGE readRange(0, 0); //CPU won't read from this buffer (write-only optimization)
    dx.constantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&dx.cbvDataBegin));

    // Upto this point, setting of Graphics Engine is complete. Now we generate the actual 

    // DO WE NEED THIS ? Is GPU INITIALIZED flag needed ?
    //WaitForPreviousFrame(dx);// Wait for initialization to complete
}

void शंकर::CleanupWindowResources(DX12ResourcesPerWindow& winRes) {
    // Wait for GPU to finish with this window's current frame
    if (winRes.cbvDataBegin) { // Unmap Window-Specific Constant Buffers
        winRes.constantBuffer->Unmap(0, nullptr);
        winRes.cbvDataBegin = nullptr;
    }

    // Release Window-Specific D3D Objects
    // Smart Pointers (ComPtr) will automatically Release() when reset.
    // We explicitly reset them to ensure deterministic destruction order.

    // SwapChain & Targets
    winRes.swapChain.Reset();
    for (int i = 0; i < FRAMES_PER_RENDERTARGETS; ++i) {
        winRes.renderTargets[i].Reset();
    }
    winRes.rtvHeap.Reset();

    // Depth Buffer
    winRes.depthStencilBuffer.Reset();
    winRes.dsvHeap.Reset();

    // Pipeline Objects (Specific to this window context)
    winRes.constantBuffer.Reset();
    winRes.cbvHeap.Reset();

    // Per-window dynamic UI overlay buffers.
    if (winRes.pUIVertexDataBegin) { winRes.uiVertexBuffer->Unmap(0, nullptr); winRes.pUIVertexDataBegin = nullptr; }
    if (winRes.pUIIndexDataBegin) { winRes.uiIndexBuffer->Unmap(0, nullptr); winRes.pUIIndexDataBegin = nullptr; }
    if (winRes.pUIOrthoDataBegin) { winRes.uiOrthoConstantBuffer->Unmap(0, nullptr); winRes.pUIOrthoDataBegin = nullptr; }
    winRes.uiVertexBuffer.Reset();
    winRes.uiIndexBuffer.Reset();
    winRes.uiOrthoConstantBuffer.Reset();

    // Per-window Page2D view constant buffer.
    if (winRes.pCad2DViewConstantDataBegin) {
        winRes.cad2dViewConstantBuffer->Unmap(0, nullptr);
        winRes.pCad2DViewConstantDataBegin = nullptr;
    }
    winRes.cad2dViewConstantBuffer.Reset();

    for (int i = 0; i < FRAMES_PER_RENDERTARGETS; ++i) {
        winRes.renderTextures[i].Reset();
    }
    winRes.rttRtvHeap.Reset();
    winRes.rttSrvHeap.Reset();

    std::wcout << "Cleaned up Window Resources." << std::endl;
}

// Following function is currently called in main UI thread, latter this responsibility will be moved to Render thread.
void शंकर::ResizeD3DWindow(DX12ResourcesPerWindow& dx, UINT newWidth, UINT newHeight)
{
    if (!dx.swapChain) return;
    if (newWidth == 0 || newHeight == 0) return; // Minimized

    ComPtr<ID3D12Fence> resizeFence;// Wait for GPU to finish using current buffers
    ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&resizeFence)));
    dx.creatorQueue->Signal(resizeFence.Get(), 1);

    HANDLE hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (resizeFence->GetCompletedValue() < 1) {
        resizeFence->SetEventOnCompletion(1, hEvent);
        WaitForSingleObject(hEvent, INFINITE);
    }
    CloseHandle(hEvent);

    for (UINT i = 0; i < FRAMES_PER_RENDERTARGETS; ++i) dx.renderTargets[i].Reset();// Release old back buffers
    dx.depthStencilBuffer.Reset(); // Release old depth buffer
    for (UINT i = 0; i < FRAMES_PER_RENDERTARGETS; ++i) dx.renderTextures[i].Reset();// Release RTT textures

    DXGI_SWAP_CHAIN_DESC desc = {};
    dx.swapChain->GetDesc(&desc);
    ThrowIfFailed(dx.swapChain->ResizeBuffers(FRAMES_PER_RENDERTARGETS, // Resize swap-chain buffers
        newWidth, newHeight, desc.BufferDesc.Format, desc.Flags));
    dx.frameIndex = dx.swapChain->GetCurrentBackBufferIndex();

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(dx.rtvHeap->GetCPUDescriptorHandleForHeapStart());// Recreate RTVs
    for (UINT i = 0; i < FRAMES_PER_RENDERTARGETS; i++) {
        ThrowIfFailed(dx.swapChain->GetBuffer(i, IID_PPV_ARGS(&dx.renderTargets[i])));
        device->CreateRenderTargetView(dx.renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.Offset(1, gpu.rtvDescriptorSize);
    }

    // Recreate depth buffer
    D3D12_CLEAR_VALUE depthClear = {};
    depthClear.Format = DXGI_FORMAT_D32_FLOAT;
    depthClear.DepthStencil.Depth = 1.0f;
    depthClear.DepthStencil.Stencil = 0;
    auto depthHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto depthDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_D32_FLOAT,
        newWidth, newHeight, 1, 0, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);

    ThrowIfFailed(device->CreateCommittedResource(&depthHeapProps, D3D12_HEAP_FLAG_NONE,
        &depthDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear, IID_PPV_ARGS(&dx.depthStencilBuffer)));

    device->CreateDepthStencilView(dx.depthStencilBuffer.Get(), nullptr,
        dx.dsvHeap->GetCPUDescriptorHandleForHeapStart());

    // Recreate RTT Textures
    CD3DX12_CPU_DESCRIPTOR_HANDLE rttRtvHandle(dx.rttRtvHeap->GetCPUDescriptorHandleForHeapStart());
    CD3DX12_CPU_DESCRIPTOR_HANDLE rttSrvHandle(dx.rttSrvHeap->GetCPUDescriptorHandleForHeapStart());
    D3D12_CLEAR_VALUE clearValue{ .Format = gpu.rttFormat,
        .Color = {kSceneSkyTopR, kSceneSkyTopG, kSceneSkyTopB, 1.0f} };

    for (UINT i = 0; i < FRAMES_PER_RENDERTARGETS; i++) {
        auto texDesc = CD3DX12_RESOURCE_DESC::Tex2D(gpu.rttFormat,
            newWidth, newHeight, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        ThrowIfFailed(device->CreateCommittedResource(&heapProps,
            D3D12_HEAP_FLAG_NONE, &texDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &clearValue, IID_PPV_ARGS(&dx.renderTextures[i])));
        device->CreateRenderTargetView(dx.renderTextures[i].Get(), nullptr, rttRtvHandle);// RTV
        rttRtvHandle.Offset(1, gpu.rtvDescriptorSize);

        // SRV
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = gpu.rttFormat;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(dx.renderTextures[i].Get(), &srvDesc, rttSrvHandle);
        rttSrvHandle.Offset(1, gpu.cbvSrvUavDescriptorSize);
    }

    dx.WindowWidth = newWidth; // Update stored dimensions
    dx.WindowHeight = newHeight;

    // Update viewport
    dx.viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<float>(newWidth), static_cast<float>(newHeight));
    dx.scissorRect = CD3DX12_RECT(0, 0, newWidth, newHeight);
}

// WinAPI related codes:

// Callback function for EnumDisplayMonitors. Notice: This function is NOT async. Just runs inline.
// But windows API design needs a separate function to be defined, which is called for each monitor.
BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData)
{
    if (gpu.currentMonitorCount >= MV_MAX_MONITORS) {
        std::wcout << L"Max monitors exceeded (" << MV_MAX_MONITORS << L"). Skipping additional monitor." << std::endl;
        return FALSE; // Stop enumeration if limit reached
    }
    OneMonitorController* currentScreen = &gpu.screens[gpu.currentMonitorCount];

    currentScreen->hMonitor = hMonitor;// Store monitor handle
    currentScreen->isVirtualMonitor = false;
    currentScreen->dpiX = 96;// Default DPI to 96 to prevent divide-by-zero later if GetDpiForMonitor fails
    currentScreen->dpiY = 96;
    currentScreen->rawDpiX = 96;
    currentScreen->rawDpiY = 96;
    currentScreen->physicalDpiX = 96;
    currentScreen->physicalDpiY = 96;

    MONITORINFOEXW monitorInfo = {};// Get monitor info
    monitorInfo.cbSize = sizeof(MONITORINFOEXW);

    // CRITICAL CHECK: If this fails, we have a "Ghost Monitor". Remove it and skip.
    if (!GetMonitorInfoW(hMonitor, &monitorInfo)) {
        // No increment happened yet, so just skip.
        std::wcout << L"GetMonitorInfoW returned nothing. Skipping this monitor." << std::endl;
        return TRUE; // Continue enumeration hoping for valid monitors
    }

    currentScreen->monitorName = std::wstring(monitorInfo.szDevice);
    currentScreen->monitorRect = monitorInfo.rcMonitor;// Store monitor rectangle (full screen)
    currentScreen->workAreaRect = monitorInfo.rcWork;// Store work area (screen minus taskbar/docked toolbars)
    // Calculate pixel dimensions
    currentScreen->screenPixelWidth = monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left;
    currentScreen->screenPixelHeight = monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top;
    // Check if this is the primary monitor
    currentScreen->isPrimary = (monitorInfo.dwFlags & MONITORINFOF_PRIMARY) != 0;

    UINT dpiX, dpiY;// Get DPI information
    if (SUCCEEDED(GetDpiForMonitor(hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
        currentScreen->dpiX = static_cast<int>(dpiX);
        currentScreen->dpiY = static_cast<int>(dpiY);
        currentScreen->scaleFactor = static_cast<double>(dpiX) / 96.0; // 96 DPI = 100% scale
    }
    if (SUCCEEDED(GetDpiForMonitor(hMonitor, MDT_RAW_DPI, &dpiX, &dpiY))) {
        currentScreen->rawDpiX = static_cast<int>(dpiX);
        currentScreen->rawDpiY = static_cast<int>(dpiY);
    }
    else {
        currentScreen->rawDpiX = currentScreen->dpiX;
        currentScreen->rawDpiY = currentScreen->dpiY;
    }
    DISPLAY_DEVICEW displayDevice = {};// Get physical dimensions and additional display properties
    displayDevice.cb = sizeof(DISPLAY_DEVICEW);
    // TODO: Windows requires displayDevice.cb to be reset every iteration. ?

    // Find the display device that matches this monitor
    for (DWORD deviceNum = 0; EnumDisplayDevicesW(NULL, deviceNum, &displayDevice, 0); deviceNum++) {
        if (wcscmp(displayDevice.DeviceName, monitorInfo.szDevice) == 0) {
            currentScreen->friendlyName = std::wstring(displayDevice.DeviceString);
            DEVMODEW devMode = {};// Get current display mode for additional info
            devMode.dmSize = sizeof(DEVMODEW);

            if (EnumDisplaySettingsW(displayDevice.DeviceName, ENUM_CURRENT_SETTINGS, &devMode)) {
                currentScreen->refreshRate = devMode.dmDisplayFrequency;
                currentScreen->colorDepth = devMode.dmBitsPerPel;
                currentScreen->orientation = devMode.dmDisplayOrientation;

                // Try to get physical dimensions from device mode. Note: These might be 0 if not available from driver
                if (devMode.dmFields & DM_PAPERSIZE) {
                    // Physical dimensions in 0.1mm units (if available)
                    currentScreen->screenPhysicalWidth = devMode.dmPaperWidth / 10;
                    currentScreen->screenPhysicalHeight = devMode.dmPaperLength / 10;
                }
            }
            break;
        }
    }

    // If physical dimensions not available from device mode, try to calculate from DPI
    if (currentScreen->screenPhysicalWidth == 0 || currentScreen->screenPhysicalHeight == 0) {
        // Prefer raw monitor DPI for the physical size estimate.
        if (currentScreen->rawDpiX > 0 && currentScreen->rawDpiY > 0) {
            currentScreen->screenPhysicalWidth = static_cast<int>((static_cast<double>(currentScreen->screenPixelWidth) / currentScreen->rawDpiX) * 25.4);
            currentScreen->screenPhysicalHeight = static_cast<int>((static_cast<double>(currentScreen->screenPixelHeight) / currentScreen->rawDpiY) * 25.4);
        }
        else {
            double inchesWidth = static_cast<double>(currentScreen->screenPixelWidth) / currentScreen->dpiX;
            double inchesHeight = static_cast<double>(currentScreen->screenPixelHeight) / currentScreen->dpiY;
            currentScreen->screenPhysicalWidth = static_cast<int>(inchesWidth * 25.4);
            currentScreen->screenPhysicalHeight = static_cast<int>(inchesHeight * 25.4);
        }
    }

    // Default initialization if physical calc fails. Fallback: 1 inch = 25.4 mm
    if (currentScreen->screenPhysicalWidth == 0) {
        currentScreen->screenPhysicalWidth = static_cast<int>((currentScreen->screenPixelWidth / (float)currentScreen->rawDpiX) * 25.4f);
        currentScreen->screenPhysicalHeight = static_cast<int>((currentScreen->screenPixelHeight / (float)currentScreen->rawDpiY) * 25.4f);
    }

    if (currentScreen->screenPhysicalWidth > 0) {
        currentScreen->physicalDpiX = static_cast<int>((static_cast<double>(currentScreen->screenPixelWidth) * 25.4) / currentScreen->screenPhysicalWidth + 0.5);
    }
    if (currentScreen->screenPhysicalHeight > 0) {
        currentScreen->physicalDpiY = static_cast<int>((static_cast<double>(currentScreen->screenPixelHeight) * 25.4) / currentScreen->screenPhysicalHeight + 0.5);
    }

    // Calculate drawable area dimensions (work area)
    currentScreen->WindowWidth = currentScreen->workAreaRect.right - currentScreen->workAreaRect.left;
    currentScreen->WindowHeight = currentScreen->workAreaRect.bottom - currentScreen->workAreaRect.top;
    currentScreen->isScreenInitalized = true;// Mark as initialized

    // Debug output
    std::wcout << L"Monitor " << gpu.currentMonitorCount << L":" << std::endl;
    std::wcout << L"Device: " << currentScreen->monitorName << std::endl;
    std::wcout << L"Name: " << currentScreen->friendlyName << std::endl;
    std::wcout << L"Resolution: " << currentScreen->screenPixelWidth << L"x" << currentScreen->screenPixelHeight << std::endl;
    std::wcout << L"Physical: " << currentScreen->screenPhysicalWidth << L"x" << currentScreen->screenPhysicalHeight << L" mm" << std::endl;
    std::wcout << L"DPI: effective " << currentScreen->dpiX << L"x" << currentScreen->dpiY
        << L", raw " << currentScreen->rawDpiX << L"x" << currentScreen->rawDpiY
        << L", physical " << currentScreen->physicalDpiX << L"x" << currentScreen->physicalDpiY << std::endl;
    std::wcout << L"Scale: " << static_cast<int>(currentScreen->scaleFactor * 100) << L"%" << std::endl;
    std::wcout << L"Work Area: " << currentScreen->WindowWidth << L"x" << currentScreen->WindowHeight << std::endl;
    std::wcout << L"Primary: " << (currentScreen->isPrimary ? L"Yes" : L"No") << std::endl;
    std::wcout << L"Refresh: " << currentScreen->refreshRate << L" Hz" << std::endl;
    std::wcout << L"Color Depth: " << currentScreen->colorDepth << L" bits" << std::endl;
    std::wcout << std::endl;

    gpu.currentMonitorCount++; // Increment count after successful initialization
    return TRUE; // Continue enumeration
}

void FetchAllMonitorDetails() // Main function to fetch all monitor details
{
    std::unique_lock<std::shared_mutex> lock(monitorMutex); // Blocks everyone
    gpu.currentMonitorCount = 0; // Reset monitor count and clear all screen data
    //memset(gpu.screens, 0, sizeof(gpu.screens)); // "Clear" the array. TODO latter without memset.
    std::wcout << L"Enumerating monitors..." << std::endl; // Try to enumerate real monitors
    EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, 0);

    /* Default values I expect. This was used during initial development.
    Latter we started relying on Windows API results. This can also be used for headless runs.
        gpu.screens[i].isScreenInitalized = false;
        gpu.screens[i].screenPixelWidth = 800;
        gpu.screens[i].screenPixelHeight = 600;
        gpu.screens[i].screenPhysicalWidth = 0;
        gpu.screens[i].screenPhysicalHeight = 0;
        gpu.screens[i].hMonitor = NULL;
        gpu.screens[i].deviceName.clear();
        gpu.screens[i].friendlyName.clear();
        ZeroMemory(&gpu.screens[i].monitorRect, sizeof(RECT));
        ZeroMemory(&gpu.screens[i].workAreaRect, sizeof(RECT));
        gpu.screens[i].dpiX = 96;
        gpu.screens[i].dpiY = 96;
        gpu.screens[i].scaleFactor = 1.0;
        gpu.screens[i].isPrimary = false;
        gpu.screens[i].orientation = DMDO_DEFAULT;
        gpu.screens[i].refreshRate = 60;
        gpu.screens[i].colorDepth = 32;
        gpu.screens[i].WindowWidth = 800;
        gpu.screens[i].WindowHeight = 600;
    */
    std::wcout << L"Total monitors found =" << gpu.currentMonitorCount << std::endl;
    // HEADLESS CHECK: If the API returned success but found 0 monitors, OR if the API failed...
    if (gpu.currentMonitorCount == 0) {
        std::wcout << L"Headless mode detected (No monitors found). Creating Virtual Display." << std::endl;

        // Force-add one virtual monitor so the rest of the app doesn't crash
        if (gpu.currentMonitorCount >= MV_MAX_MONITORS) {
            std::wcout << L"Cannot add virtual monitor: Max exceeded." << std::endl;
            return;
        }
        OneMonitorController* s = &gpu.screens[gpu.currentMonitorCount];

        s->isScreenInitalized = true;
        s->monitorName = L"HEADLESS_DISPLAY";
        s->friendlyName = L"Virtual Adapter";

        // Default to a safe resolution (e.g., SVGA or FullHD)
        s->screenPixelWidth = 1024;
        s->screenPixelHeight = 768;

        // If GetSystemMetrics fails (returns 0), we stick to the hardcoded 1024x768
        int sysW = GetSystemMetrics(SM_CXSCREEN);
        int sysH = GetSystemMetrics(SM_CYSCREEN);
        if (sysW > 0) s->screenPixelWidth = sysW;
        if (sysH > 0) s->screenPixelHeight = sysH;

        s->WindowWidth = s->screenPixelWidth;
        s->WindowHeight = s->screenPixelHeight;

        // Critical: Mark as primary so the index 0 logic works
        s->isPrimary = true;
        s->dpiX = 96;
        s->dpiY = 96;
        s->rawDpiX = 96;
        s->rawDpiY = 96;
        s->physicalDpiX = 96;
        s->physicalDpiY = 96;
        s->scaleFactor = 1.0;
        s->refreshRate = 60; // Virtual 60Hz
        s->isVirtualMonitor = true;
        gpu.currentMonitorCount++;
    }
}

void UpdateWindowMonitorAffinity(SingleUIWindow& window, int newMonitorIndex) {
    /*Each window is backed by 1 swap chain. Each swap chain is permanently tied to a commandQueue.
    We have 1 commandQueue per monitor. So when the window move from 1 monitor to another,
    we must purge existing swap chain, and create a new one associated with destination monitor's commandQueue.
    Associating a new swap chain to an existing windows is well supported and fast enough. */

    window.isMigrating = true;// SWITCH OFF RENDERING. The threads will now skip this window in their loops.
    // Memory fence: ensure the flag is visible to all CPU cores before we proceed, 
    // so the render thread cannot observe a stale false.
    std::atomic_thread_fence(std::memory_order_seq_cst);

    // We must ensure the GPU is done with the old SwapChain before destroying it.
    int oldMonitorIndex = window.currentMonitorIndex;
    if (oldMonitorIndex >= 0 && oldMonitorIndex < gpu.currentMonitorCount) {
        OneMonitorController& oldMonitor = gpu.screens[oldMonitorIndex];
        ID3D12CommandQueue* oldQueue = oldMonitor.commandQueue.Get();

        if (oldQueue && oldMonitor.renderFence) {
            // Pick a drain value that is strictly ABOVE anything the render thread has already signalled so this Signal is 
            // guaranteed to arrive AFTER all previously submitted work.
            uint64_t drainValue = oldMonitor.renderFenceValue + 1;

            HRESULT hr = oldQueue->Signal(oldMonitor.renderFence.Get(), drainValue);
            if (SUCCEEDED(hr)) {
                // Lazily create the fence event if the render thread hasn't
                // initialised it yet (can happen if migration fires very early).
                HANDLE hEvent = oldMonitor.renderFenceEvent;
                bool   ownedEvent = false;
                if (!hEvent) {
                    hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                    ownedEvent = true;
                }

                if (oldMonitor.renderFence->GetCompletedValue() < drainValue) {
                    oldMonitor.renderFence->SetEventOnCompletion(drainValue, hEvent);
                    // 3-second timeout: if we exceed this the device is likely
                    // already lost, so we log and continue rather than hang forever.
                    DWORD waitResult = WaitForSingleObject(hEvent, 3000);
                    if (waitResult != WAIT_OBJECT_0) {
                        std::wcerr << L"[Migration] WARNING: GPU drain timed out for Monitor "
                            << oldMonitorIndex << L". Device may be lost." << std::endl;
                        // We proceed anyway: if the device is truly lost,
                        // CleanupWindowResources will encounter COM errors that are already handled gracefully.
                    }
                }
                // Update the stored fence value so future callers know the last signalled value.
                oldMonitor.renderFenceValue = drainValue;
                if (ownedEvent) CloseHandle(hEvent);
            }
            else {
                std::wcerr << L"[Migration] WARNING: Signal on old queue failed (hr="
                    << std::hex << hr << L"). Skipping GPU drain." << std::endl;
            }
        }
        else {
            // Queue or fence not yet initialised (window was never rendered on
            // this monitor — e.g. created programmatically and never shown).
            // No GPU work was ever submitted, so no drain needed.
            std::wcout << L"[Migration] Old monitor " << oldMonitorIndex
                << L" has no queue/fence. Skipping GPU drain." << std::endl;
        }
    }

    gpu.CleanupWindowResources(window.dx);// Now it is safe to destroy the old SwapChain.

    // Update the monitor index so the destination monitor's render thread picks it up.
    window.currentMonitorIndex = newMonitorIndex;

    // Create the NEW SwapChain on the NEW Monitor's Queue.
    ID3D12CommandQueue* destinationCommandQueue = gpu.screens[newMonitorIndex].commandQueue.Get();
    gpu.InitD3DPerWindow(window.dx, window.hWnd, destinationCommandQueue);
    window.isMigrating = false;// SWITCH ON RENDERING

    std::wcout << "Window migrated to Monitor " << newMonitorIndex << std::endl;
}

void RestartRenderThreads() { // The "Safe" Restart Function. Runs for initial render thread creation as well.
    // We want to preserve CommandQueues because they are tied to the physical monitor and Swap Chains.
    // However actual render threads are lightweight, so we can just restart them.
    // Adding a monitor is a RARE event, so a brief pause in rendering is acceptable.
    // Either the user intentionally added / removed a monitor manually, so he expects windows to move around.
    // Or maybe electricity went out and external display connected to laptop turned off. These are rare event.

    pauseRenderThreads = true; // Pause Logic. Causes render threads (if running) to exit their loops safely.
    for (auto& t : renderThreads) { if (t.joinable()) t.join(); }
    renderThreads.clear();

    std::wcout << "RestartRenderThreads called. Analyzing..." << std::endl;
    // Capture OLD topology to salvage queues. TODO: Remove garbage values in case monitor count decreased.
    OneMonitorController oldScreens[MV_MAX_MONITORS];
    for (int i = 0; i < gpu.currentMonitorCount; i++) {
        oldScreens[i] = gpu.screens[i];
    }
    int oldMonitorCount = gpu.currentMonitorCount;
    gpu.currentMonitorCount = 0; // Global monitor count. It can be 0 when no monitors are found (headless mode)

    // Enumerate NEW topology. This populates gpu.screens with FRESH data, no queues yet.
    // Never exceeds MAX_MONITORS count. If it does, extra monitors are ignored with a warning.
    EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, 0);
    // Handle Headless/Virtual Monitor logic if needed (copy from your FetchAllMonitorDetails)
    //if (gpu.screens.empty()) { /* ... Insert headless logic here ... */ }

    // MERGE: Create Queues OR Salvage them from oldScreens
    bool matchFound = false;
    for (int i = 0; i < gpu.currentMonitorCount; i++) {
        matchFound = false;
        for (int j = 0; j < oldMonitorCount; j++) {
            if (oldScreens[j].monitorName == gpu.screens[i].monitorName) {
                gpu.screens[i].commandQueue = oldScreens[j].commandQueue;
                gpu.screens[i].renderFence = oldScreens[j].renderFence;
                gpu.screens[i].renderFenceEvent = oldScreens[j].renderFenceEvent;
                gpu.screens[i].renderFenceValue = oldScreens[j].renderFenceValue;
                std::wcout << L"Preserved Queue for: " << gpu.screens[i].monitorName << std::endl;
                matchFound = true;
                break;
            }
        }
        if (matchFound == false) {
            // NEW MONITOR (or first run). Create a new Queue.
            D3D12_COMMAND_QUEUE_DESC queueDesc = {};
            queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            gpu.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&gpu.screens[i].commandQueue));
            ThrowIfFailed(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.screens[i].renderFence)));
            gpu.screens[i].renderFenceValue = 1;
            gpu.screens[i].renderFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            std::wcout << L"Created New Queue for: " << gpu.screens[i].monitorName << std::endl;
        }

        if (gpu.screens[i].renderFence.Get() == nullptr) { //TODO: Temporary fix. Move this pto proper place.   
            ThrowIfFailed(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.screens[i].renderFence)));
            gpu.screens[i].renderFenceValue = 1;
            gpu.screens[i].renderFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        }

        PrecomputeTopRibbonLayout(gpu.screens[i].topRibbonLayout,
            static_cast<float>(gpu.screens[i].physicalDpiX), static_cast<float>(gpu.screens[i].physicalDpiY));
    }

    uint16_t* windowList = publishedWindowIndexes.load(std::memory_order_acquire);
    uint16_t windowCount = publishedWindowCount.load(std::memory_order_acquire);
    for (uint16_t wi = 0; wi < windowCount; ++wi) {
        SingleUIWindow& window = allWindows[windowList[wi]];

        // Ask Windows: "Where is this window physically right now?"
        HMONITOR hWinMonitor = MonitorFromWindow(window.hWnd, MONITOR_DEFAULTTONEAREST);
        // Find the index in our NEW gpu.screens list
        int newMonitorIdx = 0;
        for (int i = 0; i < gpu.currentMonitorCount; ++i) {
            if (gpu.screens[i].hMonitor == hWinMonitor) {
                newMonitorIdx = i;
                break;
            }
        }

        // CHECK: Do we need to migrate this window? We migrate if:
        // a) The monitor index changed (moved logically)
        // b) The underlying CommandQueue changed (e.g. moved to a totally new monitor)
        ID3D12CommandQueue* newQueue = gpu.screens[newMonitorIdx].commandQueue.Get();

        // We can't easily check 'oldQueue' since we don't store it in Window, 
        // but we can check if the monitor index changed.
        // OPTIMIZATION: If the monitor index is the same, and we preserved the queue,
        // we do NOTHING. The window stays happy.
        bool needsMigration = (window.currentMonitorIndex != newMonitorIdx);

        // Edge case: If the monitor index stayed the same (e.g. 0 -> 0), but it's actually
        // a different physical monitor (old 0 unplugged, new 0 took its place), 
        // the 'commandQueue' pointer check handles it.
        // Since we don't store the old queue in Window, we rely on the index check +
        // the fact that we preserved queues by DeviceName.
        if (needsMigration) {
            std::wcout << "Migrating Window to Monitor " << newMonitorIdx << std::endl;
            gpu.CleanupWindowResources(window.dx);// Cleanup OLD resources (Swap Chain, RTVs)

            // Initialize NEW resources with the NEW Queue
            gpu.InitD3DPerWindow(window.dx, window.hWnd, newQueue);
            window.currentMonitorIndex = newMonitorIdx;
        }
    }

    pauseRenderThreads = false; //When this is true, render threads terminate their loops.
    for (int i = 0; i < gpu.currentMonitorCount; i++) { // Spawn Threads (One per Monitor)
        int refreshRate = gpu.screens[i].refreshRate;
        renderThreads.emplace_back(GpuRenderThread, i, refreshRate);
        std::wcout << "Spawned Render Thread for Monitor " << i << " (" << refreshRate << "Hz)" << std::endl;
    }
}

// ===================== Multi-window management (UI thread only) =====================
// Slot 0 is the main window created in wWinMain. Slots are never reused within a session,
// mirroring how tab slots work (nextTabSlot).
static uint16_t nextWindowSlot = 1;

static void PublishWindowList(uint16_t* nextList, uint16_t nextCount) {
    publishedWindowIndexes.store(nextList, std::memory_order_release);
    publishedWindowCount.store(nextCount, std::memory_order_release);
}

// Creates a secondary top-level window (tab-host with full ribbon, or content-only extracted view)
// near the cursor and publishes it for the render threads. Returns nullptr when out of slots.
static SingleUIWindow* CreateSecondaryWindow(uint8_t kind, const std::wstring& title,
    int parentTabIndex, uint16_t subTabSlot) {
    if (nextWindowSlot >= MV_MAX_WINDOWS) return nullptr;

    POINT cursor{};
    GetCursorPos(&cursor);
    HMONITOR hMonitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    int monitorIndex = primaryMonitorIndex;
    for (int i = 0; i < gpu.currentMonitorCount; ++i) {
        if (gpu.screens[i].hMonitor == hMonitor) { monitorIndex = i; break; }
    }

    const uint16_t windowSlot = nextWindowSlot++;
    SingleUIWindow& window = allWindows[windowSlot];
    window.windowKind = kind;
    window.viewParentTabIndex = parentTabIndex;
    window.viewSubTabSlot = subTabSlot;
    window.activeTabIndex = parentTabIndex;
    window.currentMonitorIndex = monitorIndex;
    window.dx.contentOnly = (kind == WINDOW_KIND_VIEW);

    window.hWnd = CreateWindowExW(WS_EX_OVERLAPPEDWINDOW, szWindowClass, title.c_str(),
        WS_OVERLAPPEDWINDOW | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
        cursor.x - 60, cursor.y - 20,
        gpu.screens[monitorIndex].WindowWidth / 2, gpu.screens[monitorIndex].WindowHeight / 2,
        NULL, NULL, hInst, NULL);
    if (!window.hWnd) {
        nextWindowSlot--;
        return nullptr;
    }

    RECT clientRect{};
    if (GetClientRect(window.hWnd, &clientRect)) {
        window.currentWidth = static_cast<uint16_t>(clientRect.right - clientRect.left);
        window.currentHeight = static_cast<uint16_t>(clientRect.bottom - clientRect.top);
        window.nextRequestedWidth.store(window.currentWidth, std::memory_order_relaxed);
        window.nextRequestedHeight.store(window.currentHeight, std::memory_order_relaxed);
    }
    gpu.InitD3DPerWindow(window.dx, window.hWnd, gpu.screens[monitorIndex].commandQueue.Get());

    uint16_t* currentList = publishedWindowIndexes.load(std::memory_order_acquire);
    const uint16_t windowCount = publishedWindowCount.load(std::memory_order_acquire);
    uint16_t* nextList = (currentList == activeWindowIndexesA) ? activeWindowIndexesB : activeWindowIndexesA;
    for (uint16_t i = 0; i < windowCount; ++i) nextList[i] = currentList[i];
    nextList[windowCount] = windowSlot;
    PublishWindowList(nextList, windowCount + 1);

    ShowWindow(window.hWnd, SW_SHOW);
    UpdateWindow(window.hWnd);
    return &window;
}

// Unpublishes a secondary window, waits out any in-flight frame, releases its DX resources and
// destroys the OS window. The main window (slot 0) never goes through here.
void CloseSecondaryWindow(uint16_t windowSlot) {
    if (windowSlot == 0 || windowSlot >= MV_MAX_WINDOWS) return;
    SingleUIWindow& window = allWindows[windowSlot];
    if (!window.hWnd) return;

    window.isMigrating = true; // Render threads skip this window from their next frame on.
    std::atomic_thread_fence(std::memory_order_seq_cst);

    uint16_t* currentList = publishedWindowIndexes.load(std::memory_order_acquire);
    const uint16_t windowCount = publishedWindowCount.load(std::memory_order_acquire);
    uint16_t* nextList = (currentList == activeWindowIndexesA) ? activeWindowIndexesB : activeWindowIndexesA;
    uint16_t nextCount = 0;
    for (uint16_t i = 0; i < windowCount; ++i) {
        if (currentList[i] == windowSlot) continue;
        nextList[nextCount++] = currentList[i];
    }
    PublishWindowList(nextList, nextCount);

    // A render thread may still be recording a frame that includes this window (it read the window
    // list before we unpublished). Two frame periods are enough for it to finish and present.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Drain the monitor queue so no submitted GPU work still references this swap chain.
    const int monitorIndex = window.currentMonitorIndex;
    if (monitorIndex >= 0 && monitorIndex < gpu.currentMonitorCount) {
        OneMonitorController& monitor = gpu.screens[monitorIndex];
        if (monitor.commandQueue && monitor.renderFence) {
            const uint64_t drainValue = gpu.renderFenceValue.fetch_add(1);
            if (SUCCEEDED(monitor.commandQueue->Signal(monitor.renderFence.Get(), drainValue))) {
                HANDLE hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                if (hEvent) {
                    if (monitor.renderFence->GetCompletedValue() < drainValue) {
                        monitor.renderFence->SetEventOnCompletion(drainValue, hEvent);
                        WaitForSingleObject(hEvent, 3000); // Device likely lost past this; proceed anyway.
                    }
                    CloseHandle(hEvent);
                }
            }
        }
    }

    gpu.CleanupWindowResources(window.dx);
    HWND hWndToDestroy = window.hWnd;
    window.hWnd = nullptr;
    window.activeTabIndex = -1;
    window.viewParentTabIndex = -1;
    DestroyWindow(hWndToDestroy);
}

// Scene input routed to a sub-tab slot falls back to the inline view when that slot's dedicated
// window goes away (close / merge-back).
static void ResetInputViewRouting(DATASETTAB& tab, uint16_t subTabSlot) {
    int32_t expected = static_cast<int32_t>(subTabSlot);
    tab.inputViewSubTabSlot.compare_exchange_strong(expected, -1, std::memory_order_acq_rel);
}

// Extracts a tab out of its hosting window into a new dedicated tab-host window (full ribbon).
void ExtractTabToNewWindow(uint16_t tabID) {
    if (tabID >= MV_MAX_TABS) return;
    DATASETTAB& tab = allTabs[tabID];
    const int16_t sourceSlot = tab.hostWindowSlot.load(std::memory_order_acquire);

    uint16_t* tabList = publishedTabIndexes.load(std::memory_order_acquire);
    const uint16_t tabCount = publishedTabCount.load(std::memory_order_acquire);
    bool tabIsOpen = false;
    uint16_t hostedBySource = 0;
    int replacementTab = -1;
    for (uint16_t i = 0; i < tabCount; ++i) {
        if (tabList[i] == tabID) tabIsOpen = true;
        if (allTabs[tabList[i]].hostWindowSlot.load(std::memory_order_acquire) == sourceSlot) {
            ++hostedBySource;
            if (tabList[i] != tabID && replacementTab < 0) replacementTab = tabList[i];
        }
    }
    if (!tabIsOpen || hostedBySource < 2) return; // The last hosted tab stays with its window.

    SingleUIWindow* newWindow = CreateSecondaryWindow(WINDOW_KIND_TABHOST, tab.fileName, tabID, 0);
    if (!newWindow) return;

    tab.hostWindowSlot.store(static_cast<int16_t>(newWindow - allWindows), std::memory_order_release);
    if (sourceSlot >= 0 && sourceSlot < MV_MAX_WINDOWS &&
        allWindows[sourceSlot].activeTabIndex == tabID) {
        allWindows[sourceSlot].activeTabIndex = replacementTab;
    }
}

// Extracts one sub-tab (view) into a dedicated content-only window rendering the same
// GeometryPage; focuses the existing window when the view is already extracted.
void ExtractViewToNewWindow(uint16_t tabIndex, uint64_t containerMemoryId) {
    if (tabIndex >= MV_MAX_TABS) return;
    DATASETTAB& tab = allTabs[tabIndex];
    const int subTabSlot = FindPublishedSubTabSlot(tab, containerMemoryId);
    if (subTabSlot < 0) return;

    const int16_t existingWindow = tab.subTabHostWindowSlots[subTabSlot].load(std::memory_order_acquire);
    if (existingWindow >= 0) {
        if (allWindows[existingWindow].hWnd) SetForegroundWindow(allWindows[existingWindow].hWnd);
        return;
    }

    const std::string& asciiTitle = tab.subTabs[subTabSlot].title;
    std::wstring title(asciiTitle.begin(), asciiTitle.end());
    SingleUIWindow* newWindow = CreateSecondaryWindow(WINDOW_KIND_VIEW, title, tabIndex,
        static_cast<uint16_t>(subTabSlot));
    if (!newWindow) return;
    tab.subTabHostWindowSlots[subTabSlot].store(
        static_cast<int16_t>(newWindow - allWindows), std::memory_order_release);
    // The extracted view no longer renders inline: scene input follows it into its new window,
    // and the engineering thread hands the inline band over to the next still-inline sub-tab.
    tab.inputViewSubTabSlot.store(subTabSlot, std::memory_order_release);
    PushSystemTodoToTab(&tab, ACTION_TYPE::INTERNAL_SUB_TAB_EXTRACTED, 0, 0, 0, containerMemoryId);
}

// Closes the extracted window of one sub-tab (used when the sub-tab itself is closed).
void CloseViewWindowFor(uint16_t tabIndex, uint16_t subTabSlot) {
    if (tabIndex >= MV_MAX_TABS || subTabSlot >= MV_MAX_SUBTABS) return;
    const int16_t windowSlot =
        allTabs[tabIndex].subTabHostWindowSlots[subTabSlot].exchange(-1, std::memory_order_acq_rel);
    ResetInputViewRouting(allTabs[tabIndex], subTabSlot);
    if (windowSlot > 0 && windowSlot < MV_MAX_WINDOWS) {
        CloseSecondaryWindow(static_cast<uint16_t>(windowSlot));
    }
}

// User clicked the OS close button of a secondary window: hosted content returns to where it
// came from (views go back inline, tabs re-host into the main window), then the window dies.
void HandleSecondaryWindowClose(SingleUIWindow& window) {
    const uint16_t windowSlot = static_cast<uint16_t>(&window - allWindows);
    if (window.windowKind == WINDOW_KIND_VIEW) {
        if (window.viewParentTabIndex >= 0 && window.viewParentTabIndex < MV_MAX_TABS &&
            window.viewSubTabSlot < MV_MAX_SUBTABS) {
            DATASETTAB& parentTab = allTabs[window.viewParentTabIndex];
            parentTab.subTabHostWindowSlots[window.viewSubTabSlot]
                .store(-1, std::memory_order_release);
            ResetInputViewRouting(parentTab, window.viewSubTabSlot);
            // Make the returning view the inline-active one so it stays visible.
            PushSystemTodoToTab(&parentTab, ACTION_TYPE::ACTIVATE_INTERNAL_SUB_TAB, 0, 0, 0,
                parentTab.subTabs[window.viewSubTabSlot].containerMemoryId);
        }
    }
    else {
        uint16_t* tabList = publishedTabIndexes.load(std::memory_order_acquire);
        const uint16_t tabCount = publishedTabCount.load(std::memory_order_acquire);
        for (uint16_t i = 0; i < tabCount; ++i) {
            DATASETTAB& tab = allTabs[tabList[i]];
            if (tab.hostWindowSlot.load(std::memory_order_acquire) == static_cast<int16_t>(windowSlot)) {
                tab.hostWindowSlot.store(0, std::memory_order_release);
                if (allWindows[0].activeTabIndex < 0) allWindows[0].activeTabIndex = tabList[i];
            }
        }
    }
    CloseSecondaryWindow(windowSlot);
}

// Drag-drop merge: dropping a secondary window onto another window's top UI band merges it back.
// A view window may ONLY be dropped on the window hosting its parent tab (consistency rule).
bool TryMergeWindowOnDrop(SingleUIWindow& dragged) {
    const uint16_t draggedSlot = static_cast<uint16_t>(&dragged - allWindows);
    if (draggedSlot == 0 || !dragged.hWnd) return false;

    POINT cursor{};
    if (!GetCursorPos(&cursor)) return false;

    uint16_t* windowList = publishedWindowIndexes.load(std::memory_order_acquire);
    const uint16_t windowCount = publishedWindowCount.load(std::memory_order_acquire);
    for (uint16_t i = 0; i < windowCount; ++i) {
        const uint16_t targetSlot = windowList[i];
        SingleUIWindow& target = allWindows[targetSlot];
        if (&target == &dragged || !target.hWnd) continue;
        if (target.windowKind != WINDOW_KIND_TABHOST) continue;

        // Drop zone of the target window in screen coordinates: the whole top UI band
        // (tab bar + ribbon + sub-tab band), so the drop is easy to hit.
        RECT clientRect{};
        if (!GetClientRect(target.hWnd, &clientRect)) continue;
        POINT origin{ 0, 0 };
        ClientToScreen(target.hWnd, &origin);
        const float dropBandHeightPx = GetTopRibbonHeightPxForWindow(&target);
        if (dropBandHeightPx <= 0.0f) continue;
        if (cursor.x < origin.x || cursor.x >= origin.x + (clientRect.right - clientRect.left)) continue;
        if (cursor.y < origin.y || cursor.y >= origin.y + static_cast<LONG>(dropBandHeightPx)) continue;

        if (dragged.windowKind == WINDOW_KIND_VIEW) {
            const int parentTab = dragged.viewParentTabIndex;
            if (parentTab < 0 || parentTab >= MV_MAX_TABS) continue;
            if (allTabs[parentTab].hostWindowSlot.load(std::memory_order_acquire) !=
                static_cast<int16_t>(targetSlot)) {
                continue; // Not the window hosting the parent tab: not a valid drop target.
            }
            DATASETTAB& parent = allTabs[parentTab];
            parent.subTabHostWindowSlots[dragged.viewSubTabSlot]
                .store(-1, std::memory_order_release); // View returns inline.
            ResetInputViewRouting(parent, dragged.viewSubTabSlot);
            // Make the returning view the inline-active one so the merge is visible.
            PushSystemTodoToTab(&parent, ACTION_TYPE::ACTIVATE_INTERNAL_SUB_TAB, 0, 0, 0,
                parent.subTabs[dragged.viewSubTabSlot].containerMemoryId);
            target.activeTabIndex = parentTab; // Show the parent tab in the drop window.
            CloseSecondaryWindow(draggedSlot);
            return true;
        }

        // Dragged tab-host window: every tab it hosts moves into the target window.
        uint16_t* tabList = publishedTabIndexes.load(std::memory_order_acquire);
        const uint16_t tabCount = publishedTabCount.load(std::memory_order_acquire);
        int firstMovedTab = -1;
        for (uint16_t t = 0; t < tabCount; ++t) {
            DATASETTAB& tab = allTabs[tabList[t]];
            if (tab.hostWindowSlot.load(std::memory_order_acquire) == static_cast<int16_t>(draggedSlot)) {
                tab.hostWindowSlot.store(static_cast<int16_t>(targetSlot), std::memory_order_release);
                if (firstMovedTab < 0) firstMovedTab = tabList[t];
            }
        }
        if (dragged.activeTabIndex >= 0) target.activeTabIndex = dragged.activeTabIndex;
        else if (firstMovedTab >= 0) target.activeTabIndex = firstMovedTab;
        CloseSecondaryWindow(draggedSlot);
        return true;
    }
    return false;
}
