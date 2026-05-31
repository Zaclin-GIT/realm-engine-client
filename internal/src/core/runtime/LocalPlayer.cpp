#include "pch-il2cpp.h"
#include "LocalPlayer.h"
#include "GameState.h"
#include "RuntimeOffsets.h"
#include "Il2CppResolver.h"

#include <cmath>
#include <cstring>
#include <cstdio>

// ─────────────────────────────────────────────────────────────────────────────
// LocalPlayer — see LocalPlayer.h for design notes.
//
// Pointer resolution is delegated entirely to GameState::GetLocalPtr().
// This module owns only the per-frame stat cache and the consumer ref-count.
// ─────────────────────────────────────────────────────────────────────────────

namespace LocalPlayer {

// ── Cached pointer (mirrors GameState; kept here for change detection) ───────
static void*   s_ptr           = nullptr;

// ── Always-on stats (position — 2 reads, essentially free) ─────────────────
static float   s_x             = 0.f;
static float   s_y             = 0.f;

// ── Consumer-gated stats (only read when s_consumers > 0) ──────────────────
static int32_t s_hp                = 0;
static int32_t s_maxHp             = 0;
static int32_t s_defense           = 0;
static float   s_curMpF            = 0.f;
static int32_t s_maxMp             = 0;
static int32_t s_objType           = -1;
static float   s_cooldownRemaining   = 0.f;
static bool    s_abilityActive       = false;
static bool    s_abilityInCooldown   = false;

// ── DANCJNLCOFK (runtime-resolved) ──────────────────────────────────────────
// Returns FLT_MAX (0x7F7FFFFF) as "on cooldown" sentinel — not seconds remaining.
// Runtime logs: same sentinel when idle vs on CD; OHKEKGNHALJ stayed 40 in both states
// (not remaining time). UI uses IsAbilityInCooldown + optional numeric when not sentinel.
using GetCooldownFn = float(__fastcall*)(void* player, void* method);
static GetCooldownFn     s_getCooldownFn = nullptr;
static const MethodInfo* s_cooldownMethodInfo = nullptr;

static constexpr uint32_t kCooldownSentinelBits = 0x7F7FFFFFu; // FLT_MAX
static constexpr float    kCooldownMaxNumericSec = 600.f;

// ── Consumer ref-count ──────────────────────────────────────────────────────
static int s_consumers = 0;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

// Read position (always). Heavy stats only when consumers > 0.
static void ReadFromPtr()
{
    auto* obj = reinterpret_cast<Il2CppObject*>(s_ptr);

    // Position — raw offset, no ACTK shift, 2 reads, essentially free
    __try {
        uint8_t* p = reinterpret_cast<uint8_t*>(s_ptr);
        s_x = *reinterpret_cast<float*>(p + RuntimeOffsets::PosX);
        s_y = *reinterpret_cast<float*>(p + RuntimeOffsets::PosY);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    if (s_consumers <= 0) return;

    // Heavy stats — ObjType/MP/ability via FieldInfo (no ACTK issue for these classes)
    RuntimeOffsets::ReadField(obj, RuntimeOffsets::FI_ObjType,      s_objType);
    RuntimeOffsets::ReadField(obj, RuntimeOffsets::FI_CurMP,        s_curMpF);
    RuntimeOffsets::ReadField(obj, RuntimeOffsets::FI_MaxMP,        s_maxMp);
    RuntimeOffsets::ReadField(obj, RuntimeOffsets::FI_AbilityReady, s_abilityActive);

    // HP/MaxHP/Defense — LKHPPBEGNOM own fields get +0x50 ACTK shift at runtime.
    // il2cpp_field_get_value uses dump offsets, landing in ACTK-injected bytes.
    // Must use direct raw reads with the runtime-resolved offsets instead.
    __try {
        uint8_t* p = reinterpret_cast<uint8_t*>(s_ptr);
        s_hp      = *reinterpret_cast<int32_t*>(p + RuntimeOffsets::HP);
        s_maxHp   = *reinterpret_cast<int32_t*>(p + RuntimeOffsets::MaxHP);
        s_defense = *reinterpret_cast<int32_t*>(p + RuntimeOffsets::Defense);
        // Note: DAGEMHFLJLK (GroundDmgImmune) and BINDBHJLPMG (LocalInvincible) are no longer
        // read here. AbilityInCooldown is now derived entirely from DANCJNLCOFK below.
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    // Ability cooldown: DANCJNLCOFK returns FLT_MAX sentinel when on CD, positive seconds
    // when a numeric timer is available, or ≤0 when ready.
    // DAGEMHFLJLK (dump 0x458) is groundDamageImmune — it is NOT the cooldown flag.
    s_abilityInCooldown = false;
    s_cooldownRemaining = 0.f;
    if (s_getCooldownFn && s_cooldownMethodInfo) {
        float    cdRaw = 0.f;
        uint32_t bits  = 0;
        __try {
            void* miArg = const_cast<MethodInfo*>(s_cooldownMethodInfo);
            cdRaw       = s_getCooldownFn(s_ptr, miArg);
            std::memcpy(&bits, &cdRaw, sizeof(float));
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            cdRaw = 0.f;
            bits  = 0;
        }

        const bool sentinel = (bits == kCooldownSentinelBits) || !std::isfinite(cdRaw)
                           || (cdRaw >= 1.0e20f);
        if (sentinel) {
            // FLT_MAX or non-finite: ability is on cooldown, time unknown
            s_abilityInCooldown = true;
            s_cooldownRemaining = -1.f;
        } else if (cdRaw > 0.f && cdRaw <= kCooldownMaxNumericSec) {
            // Positive finite seconds: still cooling down
            s_abilityInCooldown = true;
            s_cooldownRemaining = cdRaw;
        }
        // else cdRaw <= 0 → ready; both remain false/0
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

void AddConsumer()    { ++s_consumers; }
void RemoveConsumer() { if (s_consumers > 0) --s_consumers; }

// Called by WorldTAB::DoRefresh — no longer required (GameState owns the ptr)
// but kept for backward compatibility; GameState::NotifyLocalPtr handles it.
void NotifyPtr(void* ptr)
{
    GameState::NotifyLocalPtr(ptr);
}

// ── Stat-drift diagnostic dump ──────────────────────────────────────────────
// Off by default. Set env RE_STAT_DUMP=1 before launching the game to append
// the memory-resolved player stats to <tmp>\realm-engine-dll-statdump.log at
// 2 Hz. Pair with the proxy's RE_STAT_DUMP wire dump to confirm whether the
// memory `defense` field (RuntimeOffsets::Defense, ~0x508) is BASE or EFFECTIVE:
//   - dll `defense` == wire DEFENSE(21)              → 21 is effective (the
//     `pd.defense + pd.defenseBonus` add double-counts — the AutoNexus bug)
//   - dll `defense` == wire DEFENSE(21)+DEFENSE_BOOST(49) → 21 is base.
// Enabling the dump self-registers a LocalPlayer consumer so the heavy stats
// (HP/Defense/…) are read even when no other feature is active.
static void DumpStatsIfEnabled()
{
    static int s_enabled = -1;  // -1 = not yet probed, 0 = off, 1 = on
    if (s_enabled == -1) {
        char buf[8] = {};
        const DWORD n = GetEnvironmentVariableA("RE_STAT_DUMP", buf, sizeof(buf));
        s_enabled = (n == 1 && buf[0] == '1') ? 1 : 0;
        if (s_enabled == 1) AddConsumer();  // ensure Tick() reads the heavy stats
    }
    if (s_enabled != 1 || !s_ptr) return;

    static ULONGLONG s_lastMs = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - s_lastMs < 500ULL) return;   // throttle to ~2 Hz
    s_lastMs = now;

    char path[MAX_PATH] = {};
    const DWORD len = GetTempPathA(MAX_PATH, path);
    if (len == 0 || len > MAX_PATH - 40) return;
    strcat_s(path, "realm-engine-dll-statdump.log");

    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") != 0 || !f) return;
    fprintf(f,
        "{\"ms\":%llu,\"hp\":%d,\"maxHp\":%d,\"defense\":%d,\"maxMp\":%d,\"objType\":%d,\"x\":%.2f,\"y\":%.2f}\n",
        now, s_hp, s_maxHp, s_defense, s_maxMp, s_objType, s_x, s_y);
    fclose(f);
}

void Tick()
{
    if (!s_getCooldownFn) {
        static ULONGLONG s_firstTick = 0;
        static bool      s_gaveUp    = false;
        if (!s_gaveUp) {
            const ULONGLONG now = GetTickCount64();
            if (s_firstTick == 0) s_firstTick = now;
            s_gaveUp = (now - s_firstTick) >= 5000ULL;
        }
        if (!s_gaveUp) {
            Il2CppClass* klass = Resolver::FindClassLoose("FKALGHJIADI");
            if (klass) {
                const MethodInfo* mi = il2cpp_class_get_method_from_name(klass, "DANCJNLCOFK", 0);
                if (mi && mi->methodPointer) {
                    s_cooldownMethodInfo = mi;
                    s_getCooldownFn      = reinterpret_cast<GetCooldownFn>(mi->methodPointer);
                }
            }
        }
    }

    // Ptr comes from GameState (already ticked this frame).
    void* newPtr = GameState::GetLocalPtr();

    // Clear stats when we lose the pointer (realm exit / death).
    if (!newPtr && s_ptr)
    {
        s_hp = s_maxHp = s_defense = s_maxMp = 0;
        s_curMpF = s_cooldownRemaining = 0.f;
        s_objType = -1;
        s_abilityActive     = false;
        s_abilityInCooldown = false;
        s_x = s_y = 0.f;
    }

    s_ptr = newPtr;

    if (s_ptr)
        ReadFromPtr();

    DumpStatsIfEnabled();
}

// ── Accessors ─────────────────────────────────────────────────────────────────
void*   GetPtr()               { return s_ptr; }
float   GetX()                 { return s_x; }
float   GetY()                 { return s_y; }
int32_t GetHP()                { return s_hp; }
int32_t GetMaxHP()             { return s_maxHp; }
int32_t GetDefense()           { return s_defense; }
float   GetCurMpF()            { return s_curMpF; }
int32_t GetMaxMP()             { return s_maxMp; }
int32_t GetObjType()           { return s_objType; }
float GetCooldownRemaining() { return s_cooldownRemaining; }
bool  IsAbilityInCooldown()  { return s_abilityInCooldown; }
bool  GetAbilityActive()     { return s_abilityActive; }

} // namespace LocalPlayer
