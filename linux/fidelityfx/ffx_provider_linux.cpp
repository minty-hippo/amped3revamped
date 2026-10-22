// Linux build of the FidelityFX API runtime (amd_fidelityfx_vk): the provider
// table of AMD's ffx_provider.cpp with only the FSR 3.1 upscaler (no frame
// generation, no Direct3D 12 driver providers).

#include "ffx_provider.h"
#include "ffx_provider_fsr3upscale.h"

static const ffxProvider* const providers[] = {
    &ffxProvider_FSR3Upscale::Instance,
};
static constexpr size_t providerCount = sizeof(providers) / sizeof(providers[0]);

const ffxProvider* GetffxProvider(ffxStructType_t descType, uint64_t overrideId, void*)
{
    for (size_t i = 0; i < providerCount; ++i)
        if (overrideId ? providers[i]->GetId() == overrideId && providers[i]->CanProvide(descType)
                       : providers[i]->CanProvide(descType))
            return providers[i];
    return nullptr;
}

const ffxProvider* GetAssociatedProvider(ffxContext* context)
{
    const InternalContextHeader* hdr = static_cast<const InternalContextHeader*>(*context);
    return hdr->provider;
}

uint64_t GetProviderCount(ffxStructType_t descType, void* device)
{
    return GetProviderVersions(descType, device, UINT64_MAX, nullptr, nullptr);
}

uint64_t GetProviderVersions(ffxStructType_t descType, void*, uint64_t capacity, uint64_t* versionIds, const char** versionNames)
{
    uint64_t count = 0;
    for (size_t i = 0; i < providerCount && count < capacity; ++i)
    {
        if (!providers[i]->CanProvide(descType))
            continue;
        if (versionIds)
            versionIds[count] = providers[i]->GetId();
        if (versionNames)
            versionNames[count] = providers[i]->GetVersionName();
        ++count;
    }
    return count;
}
