#include "pch-il2cpp.h"
#include "SharedMemory.h"
#include "LocalPlayer.h"
#include "FeatureState.h"

#include <Windows.h>
#include <cmath>
#include <cstring>

static_assert(sizeof(RotMGBotSharedLayout) == 128, "RotMGBotSharedLayout must be 128 bytes");

namespace SharedMemory {

namespace {

HANDLE           s_hMap = nullptr;
void*            s_pView = nullptr;
RotMGBotSharedLayout* s_blob = nullptr;

} // namespace

bool Init()
{
    if (s_blob)
        return true;

    s_hMap = CreateFileMappingA(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        sizeof(RotMGBotSharedLayout),
        "Local\\RotMGBotShared");
    if (!s_hMap)
        return false;

    s_pView = MapViewOfFile(s_hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(RotMGBotSharedLayout));
    if (!s_pView) {
        CloseHandle(s_hMap);
        s_hMap = nullptr;
        return false;
    }

    s_blob = static_cast<RotMGBotSharedLayout*>(s_pView);
    std::memset(s_blob, 0, sizeof(*s_blob));
    s_blob->magic = kMagic;
    s_blob->defense = kDefenseUnset;
    s_blob->classType = 0;
    return true;
}

void Shutdown()
{
    if (s_pView) {
        UnmapViewOfFile(s_pView);
        s_pView = nullptr;
    }
    if (s_hMap) {
        CloseHandle(s_hMap);
        s_hMap = nullptr;
    }
    s_blob = nullptr;
}

void Tick()
{
    if (!s_blob || s_blob->magic != kMagic)
        return;

    // C++ → Node: fast position telemetry. All other feature applies are now
    // driven by IpcBridge_ApplyFeatureOverrides() via the named-pipe bus —
    // moving them out of shared memory eliminated the per-frame IL2CPP stomps
    // that corrupted rendering (see ApplyCameraFeatureState change detection).
    if (LocalPlayer::GetPtr()) {
        s_blob->posX = LocalPlayer::GetX();
        s_blob->posY = LocalPlayer::GetY();
    } else {
        s_blob->posX = 0.f;
        s_blob->posY = 0.f;
    }
}

int32_t GetClientDefense()
{
    const int32_t bridged = FeatureState::GetClientDefense();
    if (bridged != kDefenseUnset)
        return bridged;
    if (!s_blob || s_blob->magic != kMagic)
        return LocalPlayer::GetDefense();
    const int32_t v = s_blob->defense;
    if (v == kDefenseUnset)
        return LocalPlayer::GetDefense();
    return v;
}

void SetNeedsNexus(bool v)
{
    if (s_blob) s_blob->needsNexus = v ? 1u : 0u;
}

int32_t GetClientClassType()
{
    const int32_t bridged = FeatureState::GetClientClassType();
    if (bridged != 0)
        return bridged;
    if (!s_blob || s_blob->magic != kMagic)
        return LocalPlayer::GetObjType();
    const int32_t v = s_blob->classType;
    if (v == 0)
        return LocalPlayer::GetObjType();
    return v;
}

} // namespace SharedMemory
