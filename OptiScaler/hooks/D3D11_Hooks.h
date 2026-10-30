#pragma once
#include "SysUtils.h"

class D3D11Hooks
{
  public:
    static void Hook(HMODULE dx11Module);
    static bool IsHooked();
    static void HookToDevice(ID3D11Device* InDevice);
    // Sizes of the buffers the upscaler really consumes, so the sharing filter can be scoped to them.
    static void SetUpscalerTextureSizes(uint32_t renderWidth, uint32_t renderHeight, uint32_t displayWidth,
                                        uint32_t displayHeight);
    static void Unhook();
};
