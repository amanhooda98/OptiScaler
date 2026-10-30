#pragma once

#include "SysUtils.h"

#include <flag-set-cpp/flag_set.hpp>

// Game specific hacks that are checked deeper in code
// Per-game config defaults and the exe -> quirk table live in GameConfigs.cpp
enum class GameQuirk : uint64_t
{
    CyberpunkHudlessState,
    FSRFGHudlessMismatchFixup, // Shader extracts UI from swapchain, alpha cut off and apply to hudless
    SkipFsr3Method,
    FastFeatureReset,
    LoadD3D12Manually,
    LoadVulkanManually,
    KernelBaseHooks,
    VulkanDLSSBarrierFixup,
    ForceUnrealEngine,
    NoFSRFGFirstSwapchain,
    FixSlSimulationMarkers,
    HitmanReflexHacks,
    SkipD3D11FeatureLevelElevation,
    CreateD3D12DeviceForLuma,
    ForceCreateD3D12Device,
    ForceDepthD32S8,
    PregmataFixDLSSModes,
    IgnoreValidUntilEvaluateForFG,
    IgnoreTagsWithoutHudlessForFG,
    ForceFGRenderSizeMVs,
    CreateSLOnThe2ndDevice,
    XeFGCameraMotionFill,      // XeFG inputs: velocity has alpha 0 where only the camera moved (The Witcher 3)
    DLSSGFrameLatencyWaitable, // Game waits on the frame latency waitable, SL must not consume it too
    ForceD3D11On12, // Experimental: back the game's D3D 11 device with D3D 11On12
    // Don't forget to add the new entry to printQuirks
    _
};
