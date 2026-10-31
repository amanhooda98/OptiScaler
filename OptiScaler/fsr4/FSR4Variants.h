#pragma once

#include "SysUtils.h"

#include <string>
#include <vector>

#include <ffx_api.h>

// FSR 4 upscaler builds that replace the network of amd_fidelityfx_upscaler_dx12.dll
//
// Every build is a complete copy of the upscaler which reports the same FFX version id, so FFX can't tell them
// apart. Opti lists them next to the upscalers of its own FFX modules and routes the calls of the selected one to
// its module
class FSR4Variants
{
  public:
    // True when the version id belongs to a variant, whether it is loaded or not
    static bool IsVariantVersion(uint64_t versionId);

    // Module that runs the upscaler with this version id, loads it on the first call, nullptr when the id does not
    // belong to a variant or when its dll can't be loaded
    static HMODULE VariantModule(uint64_t versionId);

    // True for modules of the variants, they must never get the driver upscaler from IAmdExtFfxApi
    static bool IsVariantModule(HMODULE module);

    // Adds the upscaler of every variant found next to the Opti dll to a version list of Opti's menu
    static void AddToVersionList(std::vector<uint64_t>& ids, std::vector<const char*>& names);

    static bool OwnsContext(ffxContext* context);

    static ffxReturnCode_t CreateContext(uint64_t versionId, ffxContext* context, ffxCreateContextDescHeader* desc,
                                         const ffxAllocationCallbacks* memCb);
    static ffxReturnCode_t DestroyContext(ffxContext* context, const ffxAllocationCallbacks* memCb);
    static ffxReturnCode_t Configure(ffxContext* context, const ffxConfigureDescHeader* desc);
    static ffxReturnCode_t Query(ffxContext* context, ffxQueryDescHeader* desc);
    static ffxReturnCode_t Dispatch(ffxContext* context, const ffxDispatchDescHeader* desc);
};
