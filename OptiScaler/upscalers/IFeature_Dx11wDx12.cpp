#include <pch.h>
#include "IFeature_Dx11wDx12.h"

#include <Util.h>
#include <Config.h>

#include <proxies/DXGI_Proxy.h>
#include <proxies/D3D12_Proxy.h>
#include <misc/IdentifyGpu.h>

#include <with_dx12/with_dx12.h>
#include <misc/DeferredRelease.h>
#include <hooks/D3D11_Hooks.h>

using Microsoft::WRL::ComPtr;

void IFeature_Dx11wDx12::ResourceBarrier(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource,
                                         D3D12_RESOURCE_STATES beforeState, D3D12_RESOURCE_STATES afterState)
{
    if (beforeState == afterState)
        return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = beforeState;
    barrier.Transition.StateAfter = afterState;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &barrier);
}

bool IFeature_Dx11wDx12::CreateD3D12Objects()
{
    HRESULT result;

    for (size_t i = 0; i < DX11WDX12_COMMAND_BUFFER_COUNT; i++)
    {
        if (Dx12CommandAllocator[i] == nullptr)
        {
            result =
                _dx11on12Device->CreateCommandAllocator(Dx12CommandListType, IID_PPV_ARGS(&Dx12CommandAllocator[i]));

            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandAllocator error: {:X}", result);
                return false;
            }
        }

        if (Dx12CommandList[i] == nullptr && Dx12CommandAllocator[i] != nullptr)
        {
            // CreateCommandList
            result = _dx11on12Device->CreateCommandList(0, Dx12CommandListType, Dx12CommandAllocator[i], nullptr,
                                                        IID_PPV_ARGS(&Dx12CommandList[i]));

            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandList error: {:X}", result);
                return false;
            }

            Dx12CommandList[i]->Close();
        }
    }

    if (Dx12Fence == nullptr)
    {
        result = _dx11on12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Dx12Fence));

        if (result != S_OK)
        {
            LOG_ERROR("CreateFence error: {0:X}", result);
            return false;
        }

        Dx12FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

        if (Dx12FenceEvent == nullptr)
        {
            LOG_ERROR("CreateEvent error!");
            return false;
        }
    }

    return true;
}

void IFeature_Dx11wDx12::ReleaseSharedResources()
{
    for (size_t i = 0; i < DX11WDX12_COMMAND_BUFFER_COUNT; i++)
    {
        SAFE_RELEASE(Dx12CommandList[i]);
        SAFE_RELEASE(Dx12CommandAllocator[i]);
        Dx12CommandAllocatorFenceValue[i] = 0;
    }

    SAFE_RELEASE(Dx12Fence);
    Dx12FenceValue = 0;

    if (Dx12FenceEvent)
    {
        CloseHandle(Dx12FenceEvent);
        Dx12FenceEvent = nullptr;
    }

    // SAFE_RELEASE(Dx12Device);
}

// Pick the command allocator slot for this frame. The previous code always took `_frameCount % 3` and then blocked
// until the fence stored for that slot three frames ago had passed. Any slot whose fence has already been reached is
// equally safe to reset and does not stall the render thread, so prefer one of those and only fall back to the oldest
// in-flight slot when the GPU is far enough behind that every slot is still busy. Both paths reset the same allocator
// and record the same fence, so the submission order is unchanged.
bool IFeature_Dx11wDx12::AcquireCommandFrame(UINT* outFrame)
{
    constexpr UINT slotCount = DX11WDX12_COMMAND_BUFFER_COUNT;

    if (outFrame == nullptr)
        return false;

    if (Dx12Fence == nullptr)
    {
        *outFrame = (UINT) (_frameCount % slotCount);
        return true;
    }

    const auto completed = Dx12Fence->GetCompletedValue();

    UINT freeSlot = slotCount;
    UINT oldestSlot = 0;
    UINT64 oldestFence = 0;
    UINT inFlight = 0;

    for (UINT i = 0; i < slotCount; i++)
    {
        const auto slotFence = Dx12CommandAllocatorFenceValue[i];

        if (slotFence == 0 || slotFence <= completed)
        {
            if (freeSlot == slotCount)
                freeSlot = i;

            continue;
        }

        inFlight++;

        if (oldestFence == 0 || slotFence < oldestFence)
        {
            oldestFence = slotFence;
            oldestSlot = i;
        }
    }

    if (_allocatorMaxInFlight < inFlight)
        _allocatorMaxInFlight = inFlight;

    const UINT chosen = freeSlot != slotCount ? freeSlot : oldestSlot;
    const UINT64 requiredFence = freeSlot != slotCount ? 0 : oldestFence;

    if (freeSlot != slotCount)
        _allocatorFreeSlotHits++;

    if (requiredFence != 0)
    {
        const auto setResult = Dx12Fence->SetEventOnCompletion(requiredFence, Dx12FenceEvent);
        if (setResult != S_OK)
        {
            LOG_ERROR("SetEventOnCompletion error for allocator {} fence {}: {:X}", chosen, requiredFence,
                      (UINT) setResult);
            return false;
        }

        const auto waitStart = Util::MillisecondsNow();
        const auto waitResult = WaitForSingleObject(Dx12FenceEvent, 5000);
        const auto waitMs = Util::MillisecondsNow() - waitStart;

        _allocatorWaits++;
        _allocatorWaitTotalMs += waitMs;
        if (waitMs > _allocatorWaitMaxMs)
            _allocatorWaitMaxMs = waitMs;

        if (waitResult != WAIT_OBJECT_0)
        {
            if (waitResult == WAIT_TIMEOUT)
            {
                LOG_ERROR("Dx11wDx12 allocator wait timed out after {:.3f} ms, slot: {}, required fence: {}, "
                          "completed: {}",
                          waitMs, chosen, requiredFence, Dx12Fence->GetCompletedValue());
            }
            else
            {
                LOG_ERROR("WaitForSingleObject failed for allocator {} fence {}: {:X}", chosen, requiredFence,
                          (UINT) waitResult);
            }

            return false;
        }
    }

    // Aggregate the wait statistics into one line per second. Per-wait logging produced ~12.5k lines for a 15 minute
    // session and made the log useless; this keeps the numbers that answer whether the ring is saturated by GPU
    // back-pressure or by our own slot choice.
    const auto now = Util::MillisecondsNow();

    if (_allocatorTelemetryNextLog == 0.0)
        _allocatorTelemetryNextLog = now + 1000.0;

    if (now >= _allocatorTelemetryNextLog)
    {
        const auto interop = Dx11WithDx12::ConsumeInteropCopyStats();

        LOG_INFO("Dx11wDx12 allocator: in-flight max {}/{}, free slot hits {}, waits {}, wait total {:.0f} ms, wait "
                 "max {:.2f} ms | interop in copies {} direct {}, out copies {} direct {}",
                 _allocatorMaxInFlight, slotCount, _allocatorFreeSlotHits, _allocatorWaits, _allocatorWaitTotalMs,
                 _allocatorWaitMaxMs, interop.InputCopies, interop.InputDirectShares, interop.OutputCopies,
                 interop.OutputDirectShares);

        _allocatorTelemetryNextLog = now + 1000.0;
        _allocatorWaits = 0;
        _allocatorFreeSlotHits = 0;
        _allocatorWaitTotalMs = 0.0;
        _allocatorWaitMaxMs = 0.0;
        _allocatorMaxInFlight = inFlight;
    }

    *outFrame = chosen;
    return true;
}

bool IFeature_Dx11wDx12::ProcessDx11Textures(const NVSDK_NGX_Parameter* InParameters)
{
    HRESULT result;

    const auto commandFrame = _currentCommandFrame;
    const auto cacheFrameKey = Dx11WithDx12::NextUpscalerFrameId();

    auto mask = Dx11WithDx12::ResourceMask::Color | Dx11WithDx12::ResourceMask::Mv | Dx11WithDx12::ResourceMask::Depth |
                Dx11WithDx12::ResourceMask::Output;

    if (!AutoExposure())
        mask |= Dx11WithDx12::ResourceMask::Exposure;
    else
        LOG_TRACE("AutoExposure enabled!");

    const bool reactiveDisabled = Config::Instance()->DisableReactiveMask.value_or(false);
    const bool reactiveRequired = Config::Instance()->Dx11Upscaler.value_or_default() == Upscaler::XeSS ||
                                  Config::Instance()->Dx11Upscaler.value_or_default() == Upscaler::XeSS_on12;

    if (!reactiveDisabled)
        mask |= Dx11WithDx12::ResourceMask::Reactive;
    else
        LOG_TRACE("ReactiveMask disabled!");

    // Linux keeps the conservative legacy sharing path, matching the CPU-wait special case in SyncDx12ToDx11.
    const bool dontUseNTShared =
        Config::Instance()->DontUseNTShared.value_or_default() || State::Instance().isRunningOnLinux;

    // Keep the texture-sharing filter scoped to the buffers this upscaler actually uses. Only touches atomics.
    if (Config::Instance()->LimitSharedTexturesToUpscalerSize.value_or_default())
        D3D11Hooks::SetUpscalerTextureSizes(RenderWidth(), RenderHeight(), DisplayWidth(), DisplayHeight());

    const auto prepareResult = Dx11WithDx12::PrepareUpscalerResources(
        ResourceCache, InParameters, mask, (UINT) _frameCount, cacheFrameKey, dontUseNTShared, reactiveRequired, true);

    if (!prepareResult.Success)
    {
        if (prepareResult.MissingExposure)
        {
            LOG_WARN("AutoExposure disabled but ExposureTexture does not exist, enabling auto exposure and changing "
                     "backend");
            State::Instance().autoExposure = true;
            State::Instance().changeBackend[Handle()->Id] = true;
            return true;
        }

        if (prepareResult.MissingReactive && reactiveRequired)
        {
            LOG_WARN("Bias mask does not exist and is required by the current DX11 upscaler, disabling reactive mask");
            Config::Instance()->DisableReactiveMask.set_volatile_value(true);
            State::Instance().changeBackend[Handle()->Id] = true;
            return true;
        }

        LOG_ERROR("Dx11wDx12 resource cache preparation failed");
        return false;
    }

    // The allocator for this frame was already acquired (and waited on) in AcquireCommandFrame.
    if (Dx12CommandAllocator[commandFrame] == nullptr || Dx12CommandList[commandFrame] == nullptr)
    {
        LOG_ERROR("Dx11wDx12 command objects not created for slot {}", commandFrame);
        return false;
    }

    result = Dx12CommandAllocator[commandFrame]->Reset();
    if (result != S_OK)
    {
        LOG_ERROR("CommandAllocator Reset error for frame {}, allocator fence {}, completed {}: {:X}", commandFrame,
                  Dx12CommandAllocatorFenceValue[commandFrame], Dx12Fence->GetCompletedValue(), (UINT) result);
        return false;
    }

    result = Dx12CommandList[commandFrame]->Reset(Dx12CommandAllocator[commandFrame], nullptr);
    if (result != S_OK)
    {
        LOG_ERROR("CommandList Reset error: {:X}", (UINT) result);
        return false;
    }

    LOG_TRACE("Shared handles prepared and synchronized by Dx11WithDx12 cache, frameKey: {}", cacheFrameKey);
    return true;
}

bool IFeature_Dx11wDx12::CopyBackOutput()
{
    const auto resourceFrame = (UINT) (_frameCount % DX11_WITH_DX12_CACHED_FRAMES);
    return Dx11WithDx12::CopyUpscalerOutputToDx11(ResourceCache, resourceFrame);
}

bool IFeature_Dx11wDx12::Init(ID3D11Device* InDevice, ID3D11DeviceContext* InContext, NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    if (IsInited())
        return true;

    if (State::Instance().NVNGX_Engine == NVSDK_NGX_ENGINE_TYPE_UNREAL ||
        State::Instance().gameEngine == GameEngineType::Unreal ||
        State::Instance().gameQuirks & GameQuirk::ForceUnrealEngine)
    {
        LOG_INFO("Dx11 detected, disabling UE resource barrier overrides");
        Config::Instance()->ColorResourceBarrier.set_volatile_value(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Config::Instance()->MVResourceBarrier.set_volatile_value(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    Device = InDevice;
    DeviceContext = InContext;

    if (!BaseInit(Device, InContext, InParameters))
    {
        LOG_DEBUG("BaseInit failed!");
        return false;
    }

    UpscalerTime = std::make_unique<GpuTime_Dx11>(InDevice);

    SetInitParameters(InParameters);

    // Non-DLSS upscalers don't use the cmdList during Init
    // We have more than one cmdList so unsure how that would even work
    SetInit(dx12Feature->Init(_dx11on12Device, Dx12CommandList[0], InParameters));

    return IsInited();
}

bool IFeature_Dx11wDx12::Evaluate(ID3D11DeviceContext* InDeviceContext, NVSDK_NGX_Parameter* InParameters)
{
    // Frees deferred objects even when presents aren't hooked
    if ((_frameCount & 3) == 0)
        DeferredRelease::Collect();

    if (!IsInited())
        return false;

    ID3D11DeviceContext4* dc = nullptr;
    HRESULT result = S_OK;
    if (InDeviceContext == Dx11DeviceContext)
    {
        dc = Dx11DeviceContext;
    }
    else
    {
        result = InDeviceContext->QueryInterface(IID_PPV_ARGS(&dc));
    }

    // GPU timing is only collected when the OSD is on screen; ScopedGpuTime_Dx11 tolerates a null timer.
    ScopedGpuTime_Dx11 scopedGpuTime(State::Instance().osdGpuTimingWanted ? UpscalerTime.get() : nullptr,
                                     InDeviceContext);

    if (InDeviceContext != Dx11DeviceContext && result != S_OK)
    {
        LOG_ERROR("QueryInterface error: {0:x}", result);
        return false;
    }

    if (dc != Dx11DeviceContext)
    {
        LOG_WARN("Dx11DeviceContext changed!");
        ReleaseSharedResources();
        Dx11DeviceContext = dc;

        if (!CreateD3D12Objects())
        {
            LOG_ERROR("Failed to recreate Dx11wDx12 D3D12 objects after context change");
            if (dc != nullptr)
                dc->Release();
            return false;
        }
    }
    else if (InDeviceContext != Dx11DeviceContext && dc != nullptr)
    {
        // The QueryInterface above added a reference to a context we already hold (a different interface of the same
        // object), so it is the only case where the reference must be dropped here. When the context actually changed
        // the member took ownership of it, and when no QueryInterface ran there is nothing to release at all.
        dc->Release();
    }

    // Acquire (and wait for) the command slot for this frame before the command list is recorded. ProcessDx11Textures
    // resets that same slot below.
    if (!AcquireCommandFrame(&_currentCommandFrame))
        return false;

    const auto commandFrame = _currentCommandFrame;
    const auto resourceFrame = (UINT) (_frameCount % DX11_WITH_DX12_CACHED_FRAMES);
    auto cmdList = Dx12CommandList[commandFrame];

    auto& cache = ResourceCache;
    auto& dx11Color = cache.Color;
    auto& dx11Mv = cache.Mv[_frameCount % DX11_WITH_DX12_INPUT_FRAMES];
    auto& dx11Depth = cache.Depth[_frameCount % DX11_WITH_DX12_INPUT_FRAMES];
    auto& dx11Reactive = cache.Reactive;
    auto& dx11Exp = cache.Exposure;
    auto& dx11Out = cache.Output[resourceFrame];

    auto getOriginalNgxResource = [](NVSDK_NGX_Parameter* parameters, const char* name, ID3D11Resource** outResource)
    {
        if (parameters == nullptr || name == nullptr || outResource == nullptr)
            return false;

        *outResource = nullptr;

        if (parameters->Get(name, outResource) != NVSDK_NGX_Result_Success)
            parameters->Get(name, (void**) outResource);

        return *outResource != nullptr;
    };

    ID3D11Resource* restoreParamColor = nullptr;
    ID3D11Resource* restoreParamMv = nullptr;
    ID3D11Resource* restoreParamOutput = nullptr;
    ID3D11Resource* restoreParamDepth = nullptr;
    ID3D11Resource* restoreParamExposure = nullptr;
    ID3D11Resource* restoreParamReactive = nullptr;

    const bool hasRestoreParamColor =
        getOriginalNgxResource(InParameters, NVSDK_NGX_Parameter_Color, &restoreParamColor);
    const bool hasRestoreParamMv =
        getOriginalNgxResource(InParameters, NVSDK_NGX_Parameter_MotionVectors, &restoreParamMv);
    const bool hasRestoreParamOutput =
        getOriginalNgxResource(InParameters, NVSDK_NGX_Parameter_Output, &restoreParamOutput);
    const bool hasRestoreParamDepth =
        getOriginalNgxResource(InParameters, NVSDK_NGX_Parameter_Depth, &restoreParamDepth);
    const bool hasRestoreParamExposure =
        getOriginalNgxResource(InParameters, NVSDK_NGX_Parameter_ExposureTexture, &restoreParamExposure);
    const bool hasRestoreParamReactive = getOriginalNgxResource(
        InParameters, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, &restoreParamReactive);

    // The upscaler runs on D3D12, so the D3D11 output merger state is irrelevant here. Unbinding and restoring the
    // game's render targets used to cost one OMGetRenderTargets plus two OMSetRenderTargets per frame, each of which
    // splits the game's render pass. Nothing in this path renders through D3D11, and the input copy (if any) reads a
    // bound render target without a hazard, so the state is left exactly as the game set it.
    bool dx12EvalResult = false;
    bool commandListRecording = false;
    bool commandListExecuted = false;
    do
    {
        if (!ProcessDx11Textures(InParameters))
        {
            LOG_ERROR("Can't process Dx11 textures!");
            break;
        }

        if (State::Instance().changeBackend[Handle()->Id])
        {
            break;
        }

        commandListRecording = true;

        InParameters->Set(NVSDK_NGX_Parameter_Color, (void*) dx11Color.Dx12Resource);
        InParameters->Set(NVSDK_NGX_Parameter_MotionVectors, (void*) dx11Mv.Dx12Resource);
        InParameters->Set(NVSDK_NGX_Parameter_Output, (void*) dx11Out.Dx12Resource);
        InParameters->Set(NVSDK_NGX_Parameter_Depth, (void*) dx11Depth.Dx12Resource);

        if (!AutoExposure() && dx11Exp.Dx12Resource != nullptr)
            InParameters->Set(NVSDK_NGX_Parameter_ExposureTexture, (void*) dx11Exp.Dx12Resource);

        if (!Config::Instance()->DisableReactiveMask.value_or(false) && dx11Reactive.Dx12Resource != nullptr)
            InParameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask,
                              (void*) dx11Reactive.Dx12Resource);

        dx12EvalResult = dx12Feature->Evaluate(cmdList, InParameters);

    } while (false);

    if (hasRestoreParamColor)
        InParameters->Set(NVSDK_NGX_Parameter_Color, (void*) restoreParamColor);

    if (hasRestoreParamMv)
        InParameters->Set(NVSDK_NGX_Parameter_MotionVectors, (void*) restoreParamMv);

    if (hasRestoreParamOutput)
        InParameters->Set(NVSDK_NGX_Parameter_Output, (void*) restoreParamOutput);

    if (hasRestoreParamDepth)
        InParameters->Set(NVSDK_NGX_Parameter_Depth, (void*) restoreParamDepth);

    if (hasRestoreParamExposure)
        InParameters->Set(NVSDK_NGX_Parameter_ExposureTexture, (void*) restoreParamExposure);

    if (hasRestoreParamReactive)
        InParameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, (void*) restoreParamReactive);

    if (commandListRecording)
    {
        const auto closeResult = cmdList->Close();
        if (closeResult != S_OK)
        {
            LOG_ERROR("CommandList Close error: {:X}", (UINT) closeResult);
            dx12EvalResult = false;
        }
    }

    if (dx12EvalResult)
    {
        ID3D12CommandList* ppCommandLists[] = { cmdList };
        Dx12CommandQueue->ExecuteCommandLists(1, ppCommandLists);
        commandListExecuted = true;

        const auto fenceValue = ++Dx12FenceValue;
        result = Dx12CommandQueue->Signal(Dx12Fence, fenceValue);
        if (result != S_OK)
        {
            LOG_ERROR("Dx12CommandQueue Signal failed for feature fence {}: {:X}", fenceValue, (UINT) result);
            dx12EvalResult = false;
        }
        else
        {
            Dx12CommandAllocatorFenceValue[commandFrame] = fenceValue;
        }
    }

    auto evalResult = false;

    do
    {
        if (!dx12EvalResult || !commandListExecuted)
            break;

        if (!CopyBackOutput())
        {
            LOG_ERROR("Can't copy output texture back!");
            break;
        }

        evalResult = true;

    } while (false);

    if (evalResult)
        _frameCount++;
    else
        Dx11WithDx12::ClearLastPreparedUpscalerFrameState(ResourceCache);

    return evalResult;
}

bool IFeature_Dx11wDx12::BaseInit(ID3D11Device* InDevice, ID3D11DeviceContext* InContext,
                                  NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    if (IsInited())
        return true;

    if (!InContext)
    {
        LOG_ERROR("context is null!");
        return false;
    }

    auto contextResult = InContext->QueryInterface(IID_PPV_ARGS(&Dx11DeviceContext));
    if (contextResult != S_OK)
    {
        LOG_ERROR("QueryInterface ID3D11DeviceContext4 result: {0:x}", contextResult);
        return false;
    }
    else
    {
        Dx11DeviceContext->Release();
    }

    if (!InDevice)
        Dx11DeviceContext->GetDevice(&InDevice);

    auto dx11DeviceResult = InDevice->QueryInterface(IID_PPV_ARGS(&Dx11Device));

    if (dx11DeviceResult != S_OK)
    {
        LOG_ERROR("QueryInterface ID3D11Device5 result: {0:x}", dx11DeviceResult);
        return false;
    }
    else
    {
        Dx11Device->Release();
    }

    if (!WithDx12::PrepareD3D12ForD3D11(InDevice, D3D_FEATURE_LEVEL_11_0))
    {
        LOG_ERROR("Cannot resolve D3D12 device/queue from WithDx12!");
        return false;
    }

    _dx11on12Device = WithDx12::GetD3D12Device();
    if (_dx11on12Device == nullptr)
    {
        LOG_ERROR("Cannot get D3D12 device from WithDx12!");
        return false;
    }

    Dx12CommandQueue = WithDx12::GetD3D12CommandQueue();
    if (Dx12CommandQueue == nullptr)
    {
        LOG_ERROR("Cannot get D3D12 command queue from WithDx12!");
        return false;
    }

    Dx12CommandListType = WithDx12::GetD3D12CommandListType();

    if (!CreateD3D12Objects())
    {
        LOG_ERROR("Failed to create D3D12 objects!");
        return false;
    }

    Dx11WithDx12::Init(Dx11Device, Dx11DeviceContext);

    return true;
}

IFeature_Dx11wDx12::IFeature_Dx11wDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters)
    : IFeature(InHandleId, InParameters), IFeature_Dx11(InHandleId, InParameters)
{
}

IFeature_Dx11wDx12::~IFeature_Dx11wDx12()
{
    if (State::Instance().isShuttingDown)
        return;

    ReleaseSharedResources();
    Dx11WithDx12::ResetUpscalerResourceCache(ResourceCache);
}
