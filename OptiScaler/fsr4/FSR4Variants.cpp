#include "pch.h"
#include "FSR4Variants.h"

#include "FSR4Models.h"
#include "FSR4Presets.h"

#include "Config.h"
#include "Util.h"

#include <proxies/FfxApi_Proxy.h>
#include <proxies/KernelBase_Proxy.h>

#include <ffx_upscale.h>

#include <algorithm>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
// FFX packs the version of a provider into the low bits of its id, these ids are only used between Opti and its
// menu and are never handed to a module, the high bits keep them apart from anything FFX or the driver reports
constexpr uint64_t variantIdBase = 0xC0B0000000000000;

constexpr std::wstring_view variantFileNamePrefix = L"amd_fidelityfx_upscaler_dx12.";
constexpr const wchar_t* variantFolderName = L"fsr4-variants";
constexpr const wchar_t* upscalerFileNameStem = L"amd_fidelityfx_upscaler_dx12";

struct Variant
{
    uint64_t versionId {};
    std::string name {};
    std::wstring folder {};
    std::wstring fileName {};
    HMODULE module {};
    PfnFfxCreateContext CreateContext {};
    PfnFfxDestroyContext DestroyContext {};
    PfnFfxConfigure Configure {};
    PfnFfxQuery Query {};
    PfnFfxDispatch Dispatch {};
};

std::mutex variantsMutex;
std::deque<Variant> variants {};
bool variantsInitialized = false;

std::mutex contextsMutex;
std::unordered_map<ffxContext, Variant*> contextOwners {};

// Caller must hold variantsMutex
void InitializeLocked()
{
    if (variantsInitialized)
        return;

    auto mainPath = Config::Instance()->MainDllPath;

    // DllMain sets the path before the FFX modules load, the next call tries again when it isn't set yet
    if (!mainPath.has_value())
        return;

    variantsInitialized = true;

    std::filesystem::path folder = std::filesystem::absolute(mainPath.value()) / variantFolderName;
    std::error_code error;

    if (!std::filesystem::is_directory(folder, error))
        return;

    std::vector<std::filesystem::path> files;

    for (auto& entry : std::filesystem::directory_iterator(folder, error))
    {
        if (!entry.is_regular_file(error) || entry.path().extension() != L".dll")
            continue;

        files.push_back(entry.path());
    }

    std::ranges::sort(files, [](const std::filesystem::path& left, const std::filesystem::path& right)
                      { return left.filename().wstring() < right.filename().wstring(); });

    size_t index = 0;

    for (auto& file : files)
    {
        auto name = file.stem().wstring();

        // The upscaler of the SDK is loaded separately and is not a variant of itself
        if (name == upscalerFileNameStem)
            continue;

        if (name.rfind(variantFileNamePrefix, 0) == 0)
            name = name.substr(variantFileNamePrefix.size());

        Variant variant {};
        variant.versionId = variantIdBase | (++index);
        variant.name = wstring_to_string(name);
        variant.folder = folder.wstring();
        variant.fileName = file.filename().wstring();
        variants.emplace_back(std::move(variant));
    }

    LOG_INFO("Found {} FSR 4 upscaler variants in {}", variants.size(), wstring_to_string(folder.wstring()));
}

// Caller must hold variantsMutex
void LoadLocked(Variant& variant)
{
    if (variant.module != nullptr)
        return;

    HMODULE hookedModule = nullptr;
    Util::LoadProxyLibrary(variant.fileName, variant.folder, L"", &hookedModule, &variant.module);

    // Can't find Opti dlls, use just module loaded by game
    if (variant.module == nullptr)
        variant.module = hookedModule;

    if (variant.module == nullptr)
    {
        LOG_WARN("Can't load the {} upscaler variant", variant.name);
        return;
    }

    FSR4Presets::Hook(variant.module);
    FSR4Models::Hook(variant.module);

    if (!FSR4Presets::IsModuleHooked(variant.module))
    {
        LOG_WARN("No free preset hook slot for {}, its FSR 4 preset selection keeps the default", variant.name);
    }

    variant.Configure = (PfnFfxConfigure) KernelBaseProxy::GetProcAddress_()(variant.module, "ffxConfigure");
    variant.CreateContext =
        (PfnFfxCreateContext) KernelBaseProxy::GetProcAddress_()(variant.module, "ffxCreateContext");
    variant.DestroyContext =
        (PfnFfxDestroyContext) KernelBaseProxy::GetProcAddress_()(variant.module, "ffxDestroyContext");
    variant.Dispatch = (PfnFfxDispatch) KernelBaseProxy::GetProcAddress_()(variant.module, "ffxDispatch");
    variant.Query = (PfnFfxQuery) KernelBaseProxy::GetProcAddress_()(variant.module, "ffxQuery");

    LOG_INFO("Loaded the {} upscaler variant from {}", variant.name, wstring_to_string(variant.folder));
}

// FFX refuses an override version id none of the providers of a module reports
ffxOverrideVersion* FindOverrideVersion(ffxCreateContextDescHeader* desc)
{
    for (auto header = desc; header != nullptr; header = header->pNext)
    {
        if (header->type == FFX_API_DESC_TYPE_OVERRIDE_VERSION)
            return reinterpret_cast<ffxOverrideVersion*>(header);
    }

    return nullptr;
}

Variant* OwnerOfContext(ffxContext* context)
{
    if (context == nullptr || *context == nullptr)
        return nullptr;

    std::scoped_lock lock(contextsMutex);
    auto it = contextOwners.find(*context);
    return it != contextOwners.end() ? it->second : nullptr;
}

Variant* FindVariant(uint64_t versionId)
{
    if (versionId == 0)
        return nullptr;

    std::scoped_lock lock(variantsMutex);
    InitializeLocked();

    for (auto& variant : variants)
    {
        if (variant.versionId == versionId)
            return &variant;
    }

    return nullptr;
}

} // namespace

bool FSR4Variants::IsVariantVersion(uint64_t versionId) { return FindVariant(versionId) != nullptr; }

HMODULE FSR4Variants::VariantModule(uint64_t versionId)
{
    auto variant = FindVariant(versionId);

    if (variant == nullptr)
        return nullptr;

    std::scoped_lock lock(variantsMutex);
    LoadLocked(*variant);
    return variant->module;
}

bool FSR4Variants::IsVariantModule(HMODULE module)
{
    if (module == nullptr)
        return false;

    std::scoped_lock lock(variantsMutex);
    InitializeLocked();

    return std::ranges::any_of(variants, [module](const Variant& variant) { return variant.module == module; });
}

void FSR4Variants::AddToVersionList(std::vector<uint64_t>& ids, std::vector<const char*>& names)
{
    std::scoped_lock lock(variantsMutex);
    InitializeLocked();

    for (auto& variant : variants)
    {
        if (std::ranges::find(ids, variant.versionId) != ids.end())
            continue;

        ids.push_back(variant.versionId);
        names.push_back(variant.name.c_str());
    }
}

bool FSR4Variants::OwnsContext(ffxContext* context) { return OwnerOfContext(context) != nullptr; }

ffxReturnCode_t FSR4Variants::CreateContext(uint64_t versionId, ffxContext* context, ffxCreateContextDescHeader* desc,
                                            const ffxAllocationCallbacks* memCb)
{
    auto variant = FindVariant(versionId);

    if (variant == nullptr)
        return FFX_API_RETURN_NO_PROVIDER;

    {
        std::scoped_lock lock(variantsMutex);
        LoadLocked(*variant);
    }

    if (variant->module == nullptr || variant->CreateContext == nullptr)
        return FFX_API_RETURN_NO_PROVIDER;

    // Every variant build reports the same FFX version id and FFX refuses an override id none of the providers of
    // a module has, so the module is asked to pick its own provider instead, they only have one upscaler each
    auto overrideVersion = FindOverrideVersion(desc);
    auto savedVersionId = overrideVersion != nullptr ? overrideVersion->versionId : 0;

    if (overrideVersion != nullptr)
        overrideVersion->versionId = 0;

    auto result = variant->CreateContext(context, desc, memCb);

    if (overrideVersion != nullptr)
        overrideVersion->versionId = savedVersionId;

    LOG_INFO("Created {} upscaler context, result: {}", variant->name, FfxApiProxy::ReturnCodeToString(result));

    if (result == FFX_API_RETURN_OK)
    {
        std::scoped_lock lock(contextsMutex);
        contextOwners[*context] = variant;
    }

    return result;
}

ffxReturnCode_t FSR4Variants::DestroyContext(ffxContext* context, const ffxAllocationCallbacks* memCb)
{
    auto variant = OwnerOfContext(context);

    if (variant == nullptr || variant->DestroyContext == nullptr)
        return FFX_API_RETURN_NO_PROVIDER;

    auto result = variant->DestroyContext(context, memCb);

    if (result == FFX_API_RETURN_OK)
    {
        std::scoped_lock lock(contextsMutex);
        contextOwners.erase(*context);
    }

    return result;
}

ffxReturnCode_t FSR4Variants::Configure(ffxContext* context, const ffxConfigureDescHeader* desc)
{
    auto variant = OwnerOfContext(context);

    if (variant == nullptr || variant->Configure == nullptr)
        return FFX_API_RETURN_NO_PROVIDER;

    return variant->Configure(context, desc);
}

ffxReturnCode_t FSR4Variants::Query(ffxContext* context, ffxQueryDescHeader* desc)
{
    auto variant = OwnerOfContext(context);

    if (variant == nullptr || variant->Query == nullptr)
        return FFX_API_RETURN_NO_PROVIDER;

    return variant->Query(context, desc);
}

ffxReturnCode_t FSR4Variants::Dispatch(ffxContext* context, const ffxDispatchDescHeader* desc)
{
    auto variant = OwnerOfContext(context);

    if (variant == nullptr || variant->Dispatch == nullptr)
        return FFX_API_RETURN_NO_PROVIDER;

    return variant->Dispatch(context, desc);
}
