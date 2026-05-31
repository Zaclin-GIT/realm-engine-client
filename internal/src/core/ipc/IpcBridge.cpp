#include "pch-il2cpp.h"
#include "IpcBridgeIncludes.hpp"
#include "IpcTileState.h"

// Debug logging

#ifdef _DEBUG
#define DbgLog(fmt, ...) do { \
    char _b[512]; snprintf(_b, sizeof(_b), fmt, ##__VA_ARGS__); \
    std::cout << "[IpcBridge] " << _b << std::endl; \
} while(0)
#else
#define DbgLog(fmt, ...) (void)0
#endif

// Pipe constants and shared state

static const char* PipeName() { return BUILD_PIPE_NAME; }
static const DWORD PIPE_BUFFER_SIZE = 65536;

static std::mutex s_pluginFloatingTextMutex;
static char s_pluginFloatingText[128] = {};
static bool s_pluginFloatingTextPending = false;

struct UnityNullableColor32Abi {
    bool hasValue;
    uint8_t padding[3];
    uint32_t rgba;
};

static_assert(sizeof(UnityNullableColor32Abi) == 8, "Nullable<Color32> ABI must be 8 bytes");
constexpr uint32_t PackColor32(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) { return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(a) << 24); }

// Tile map API

bool IpcBridge_IsTileWalkable(float worldX, float worldY)
{
    return IpcTileState::IsWalkable(worldX, worldY);
}

void IpcBridge_GetTileStats(int* outTileCount, int* outNoWalkTypeCount)
{
    IpcTileState::GetStats(outTileCount, outNoWalkTypeCount);
}

int IpcBridge_CopyUniqueTypeEntries(IpcTileTypeEntry* buf, int maxCount)
{
    return IpcTileState::CopyUniqueTypeEntries(buf, maxCount);
}

static Handshake::AuthState s_auth = {};

// Session and overlay state

const char* IpcBridge_GetUserId()       { return s_auth.userId; }
bool        IpcBridge_IsAuthenticated() { return Handshake::IsHealthy(&s_auth); }

static std::atomic<bool> s_overlayEnabled{false};

bool IpcBridge_IsOverlayEnabled() { return s_overlayEnabled.load(std::memory_order_relaxed); }

static std::atomic<int> s_featAutoAimEnabled{0}, s_featAutoAimMode{0}, s_featProjectileNoclipEnabled{0};
static std::atomic<int> s_featDodgeMode{0}, s_featDodgeWallAvoid{1}, s_featAutoAbilityEnabled{0}, s_featAutoAbilityWizardMode{0};
static std::atomic<int> s_featPlayerNoclipActive{0}, s_featPlayerNoclipEnabled{0}, s_featPlayerNoclipHotkeyVk{'N'}, s_pendingPlayerNoclipEnabled{-1};
static std::atomic<int> s_featSocketHotkeyActive{0}, s_featSocketHotkeyVk{'L'}, s_featWalkTargetActive{0};
static std::atomic<int> s_featCameraZoomActive{0}, s_featCameraAngleActive{0}, s_featCameraAngleValue{0}, s_featCameraCenteringActive{0}, s_featCameraCentered{1};
static std::atomic<int> s_featSkinOverrideEnabled{0}, s_featSkinOverrideId{0};
static std::atomic<float> s_featDodgeHorizonMs{800.f}, s_featDodgeHitboxPadding{0.f}, s_featAutoAbilityMpPct{0.f};
static std::atomic<float> s_featWalkTargetX{0.f}, s_featWalkTargetY{0.f}, s_featCameraZoomValue{8.f};
static std::atomic<int32_t> s_featClientDefense{static_cast<int32_t>(0x80000000u)}, s_featClientClassType{0};

static int ClampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float ClampFloat(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Render-thread feature applicators

namespace {
    void ApplyAutoAimFeatureState()
    {
        static int s_lastEnabled = -1, s_lastMode = -1;
        const int enabled = s_featAutoAimEnabled.load(std::memory_order_relaxed) != 0 ? 1 : 0;
        if (enabled != s_lastEnabled) { s_lastEnabled = enabled; AutoAim::SetEnabled(enabled != 0); }
        const int aimMode = s_featAutoAimMode.load(std::memory_order_relaxed);
        if (aimMode != s_lastMode) {
            s_lastMode = aimMode;
            AutoAim::AimMode resolved = AutoAim::AimMode::ClosestToPlayer;
            if (aimMode == 1)      resolved = AutoAim::AimMode::HighestHP;
            else if (aimMode == 2) resolved = AutoAim::AimMode::ClosestToMouse;
            AutoAim::SetAimMode(resolved);
        }
    }

    void ApplyProjectileNoclipFeatureState()
    {
        static int s_lastEnabled = -1;
        const int enabled = s_featProjectileNoclipEnabled.load(std::memory_order_relaxed) != 0 ? 1 : 0;
        if (enabled != 0 && !ProjNoclip::IsInstalled()) ProjNoclip::Install();
        if (enabled != s_lastEnabled || ProjNoclip::IsEnabled() != (enabled != 0)) { s_lastEnabled = enabled; ProjNoclip::SetEnabled(enabled != 0); }
    }

    void ApplyAutoDodgeFeatureState()
    {
        static int s_lastMode = INT32_MIN;
        static float s_lastHorizonMs = -1.f;
        int dodgeMode = ClampInt(s_featDodgeMode.load(std::memory_order_relaxed), 0, static_cast<int>(TestTAB::DodgeMode::Rollout));
        if (dodgeMode != s_lastMode) { s_lastMode = dodgeMode; TestTAB::SetDodgeModeWithEnter(static_cast<TestTAB::DodgeMode>(dodgeMode)); }
        if (dodgeMode != static_cast<int>(TestTAB::DodgeMode::Off)) DangerPlanner::TryInstall();
        float horizonMs = ClampFloat(s_featDodgeHorizonMs.load(std::memory_order_relaxed), 100.f, 4000.f);
        if (horizonMs != s_lastHorizonMs) { s_lastHorizonMs = horizonMs; TestTAB::SetDodgeLookaheadMs(horizonMs); }
    }

    void ApplyAutoAbilityFeatureState()
    {
        static int s_lastEnabled = -1, s_lastWizMode = INT32_MIN;
        static float s_lastMpPct = -1.f;
        const int enabled = s_featAutoAbilityEnabled.load(std::memory_order_relaxed) != 0 ? 1 : 0;
        const float mpPct = s_featAutoAbilityMpPct.load(std::memory_order_relaxed);
        const int wizMode = s_featAutoAbilityWizardMode.load(std::memory_order_relaxed);
        if (enabled != s_lastEnabled) { s_lastEnabled = enabled; CombatTAB::SetAutoAbility(enabled != 0); }
        if (mpPct != s_lastMpPct) { s_lastMpPct = mpPct; CombatTAB::SetAbilityMpPct(mpPct); }
        if (wizMode != s_lastWizMode) { s_lastWizMode = wizMode; CombatTAB::SetWizardAbilityTargetMode(wizMode); }
    }

    int ResolveHotkeyVk(const char* raw)
    {
        if (!raw) return 0;
        std::string key;
        for (const char* p = raw; *p; ++p) {
            const unsigned char ch = static_cast<unsigned char>(*p);
            if (!std::isspace(ch)) key.push_back(static_cast<char>(std::toupper(ch)));
        }
        if (key.empty() || key == "NONE" || key == "OFF") return 0;
        if (key.rfind("VK_", 0) == 0) key.erase(0, 3);
        if (key.size() == 1) {
            const char ch = key[0];
            if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) return static_cast<int>(ch);
        }
        if (key.size() >= 2 && key[0] == 'F') {
            const int fn = atoi(key.c_str() + 1);
            if (fn >= 1 && fn <= 24) return VK_F1 + (fn - 1);
        }
        if (key.rfind("NUMPAD", 0) == 0) {
            const int n = atoi(key.c_str() + 6);
            if (n >= 0 && n <= 9) return VK_NUMPAD0 + n;
        }
        static const std::pair<const char*, int> named[] = {{"SPACE", VK_SPACE}, {"TAB", VK_TAB}, {"ESC", VK_ESCAPE}, {"ESCAPE", VK_ESCAPE}, {"SHIFT", VK_SHIFT}, {"CTRL", VK_CONTROL}, {"CONTROL", VK_CONTROL}, {"ALT", VK_MENU}, {"MENU", VK_MENU}, {"INSERT", VK_INSERT}, {"INS", VK_INSERT}, {"DELETE", VK_DELETE}, {"DEL", VK_DELETE}, {"HOME", VK_HOME}, {"END", VK_END}, {"PAGEUP", VK_PRIOR}, {"PGUP", VK_PRIOR}, {"PAGEDOWN", VK_NEXT}, {"PGDN", VK_NEXT}, {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT}};
        for (const auto& kv : named) if (key == kv.first) return kv.second;
        return 0;
    }

    bool IsCurrentProcessForeground()
    {
        HWND hwnd = GetForegroundWindow();
        if (!hwnd) return false;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        return pid == GetCurrentProcessId();
    }

    static bool IsHotkeyDown(int active, int vk) { return active != 0 && vk != 0 && IsCurrentProcessForeground() && ((GetAsyncKeyState(vk) & 0x8000) != 0); }
    template <typename T, typename Setter> static void ApplyActiveValue(std::atomic<int>& activeAtom, std::atomic<T>& valueAtom, int& lastActive, T& lastValue, Setter setter)
    {
        const int active = activeAtom.load(std::memory_order_relaxed) != 0 ? 1 : 0;
        const T value = valueAtom.load(std::memory_order_relaxed);
        if (active != 0 && (active != lastActive || value != lastValue)) { lastActive = active; lastValue = value; setter(value); }
        else if (active == 0) lastActive = active;
    }

    void ApplyPlayerNoclipFeatureState()
    {
        static int s_lastEnabled = -1;
        static bool s_lastHotkeyDown = false;
        const int active = s_featPlayerNoclipActive.load(std::memory_order_relaxed) != 0 ? 1 : 0;
        int enabled = active && s_featPlayerNoclipEnabled.load(std::memory_order_relaxed) != 0 ? 1 : 0;
        const int vk = s_featPlayerNoclipHotkeyVk.load(std::memory_order_relaxed);
        const bool hotkeyDown = IsHotkeyDown(active, vk);
        if (hotkeyDown && !s_lastHotkeyDown) {
            enabled = enabled ? 0 : 1;
            s_featPlayerNoclipEnabled.store(enabled, std::memory_order_relaxed);
            s_pendingPlayerNoclipEnabled.store(enabled, std::memory_order_relaxed);
        }
        s_lastHotkeyDown = hotkeyDown;
        if (enabled != s_lastEnabled) { s_lastEnabled = enabled; Noclip::SetEnabled(enabled != 0); Noclip::SetMode(enabled != 0 ? 1 : 0); }
    }

    bool PollSocketHotkeyEvent()
    {
        static bool s_lastHotkeyDown = false;
        const int active = s_featSocketHotkeyActive.load(std::memory_order_relaxed) != 0 ? 1 : 0;
        const int vk = s_featSocketHotkeyVk.load(std::memory_order_relaxed);
        const bool hotkeyDown = IsHotkeyDown(active, vk);
        const bool shouldFire = hotkeyDown && !s_lastHotkeyDown;
        s_lastHotkeyDown = hotkeyDown;
        return shouldFire;
    }

    void ApplyWalkTargetFeatureState()
    {
        static int   s_lastActive = -1;
        static float s_lastX      = std::numeric_limits<float>::quiet_NaN();
        static float s_lastY      = std::numeric_limits<float>::quiet_NaN();
        const int   walkActive = s_featWalkTargetActive.load(std::memory_order_relaxed) != 0 ? 1 : 0;
        const float walkX      = s_featWalkTargetX.load(std::memory_order_relaxed);
        const float walkY      = s_featWalkTargetY.load(std::memory_order_relaxed);
        const bool changed = walkActive != s_lastActive || walkX != s_lastX || walkY != s_lastY;
        if (changed) {
            s_lastActive = walkActive; s_lastX = walkX; s_lastY = walkY;
            TestTAB::SetBotWalkTarget(walkX, walkY, walkActive != 0);
        }
    }

    void ApplyCameraFeatureState()
    {
        static int zoomActive = -1, angleActive = -1, centerActive = -1, angle = INT32_MIN, centered = -1;
        static float zoom = std::numeric_limits<float>::quiet_NaN();
        ApplyActiveValue<float>(s_featCameraZoomActive, s_featCameraZoomValue, zoomActive, zoom, [](float v) { CameraTAB::SetZoomValue(v); });
        ApplyActiveValue<int>(s_featCameraAngleActive, s_featCameraAngleValue, angleActive, angle, [](int v) { CameraTAB::SetAngleDegrees(v); });
        ApplyActiveValue<int>(s_featCameraCenteringActive, s_featCameraCentered, centerActive, centered, [](int v) { CameraTAB::SetCenteredOnPlayer(v != 0); });
    }
} // namespace

// Public feature accessors

#define LOAD_BOOL(name, atom) bool name() { return atom.load(std::memory_order_relaxed) != 0; }
#define LOAD_INT(name, atom) int name() { return atom.load(std::memory_order_relaxed); }
#define LOAD_FLOAT(name, atom) float name() { return atom.load(std::memory_order_relaxed); }
#define STORE_BOOL(name, atom) void name(bool v) { atom.store(v ? 1 : 0, std::memory_order_relaxed); }

LOAD_BOOL(IpcBridge_GetAutoAimEnabled, s_featAutoAimEnabled)
LOAD_INT(IpcBridge_GetAutoAimMode, s_featAutoAimMode)
STORE_BOOL(IpcBridge_SetAutoAimEnabled, s_featAutoAimEnabled)
void IpcBridge_SetAutoAimMode(int mode) { s_featAutoAimMode.store(ClampInt(mode, 0, 2), std::memory_order_relaxed); }

LOAD_INT(IpcBridge_GetAutoDodgeMode, s_featDodgeMode)
void IpcBridge_SetAutoDodgeMode(int mode) { s_featDodgeMode.store(ClampInt(mode, 0, static_cast<int>(TestTAB::DodgeMode::Rollout)), std::memory_order_relaxed); }
LOAD_FLOAT(IpcBridge_GetAutoDodgeHorizonMs, s_featDodgeHorizonMs)
void IpcBridge_SetAutoDodgeHorizonMs(float ms) { s_featDodgeHorizonMs.store(ClampFloat(ms, 100.f, 4000.f), std::memory_order_relaxed); }
LOAD_FLOAT(IpcBridge_GetAutoDodgeHitboxPadding, s_featDodgeHitboxPadding)
void IpcBridge_SetAutoDodgeHitboxPadding(float paddingTiles) { s_featDodgeHitboxPadding.store(ClampFloat(paddingTiles, 0.f, 1.5f), std::memory_order_relaxed); }
LOAD_BOOL(IpcBridge_GetAutoDodgeWallAvoid, s_featDodgeWallAvoid)
STORE_BOOL(IpcBridge_SetAutoDodgeWallAvoid, s_featDodgeWallAvoid)

LOAD_BOOL(IpcBridge_GetAutoAbilityEnabled, s_featAutoAbilityEnabled)
STORE_BOOL(IpcBridge_SetAutoAbilityEnabled, s_featAutoAbilityEnabled)
LOAD_FLOAT(IpcBridge_GetAutoAbilityMpPct, s_featAutoAbilityMpPct)
void IpcBridge_SetAutoAbilityMpPct(float pctZeroTo100) { s_featAutoAbilityMpPct.store(ClampFloat(pctZeroTo100, 0.f, 100.f), std::memory_order_relaxed); }
LOAD_INT(IpcBridge_GetAutoAbilityWizardMode, s_featAutoAbilityWizardMode)
void IpcBridge_SetAutoAbilityWizardMode(int mode) { s_featAutoAbilityWizardMode.store(mode == 1 ? 1 : 0, std::memory_order_relaxed); }

LOAD_FLOAT(IpcBridge_GetWalkTargetX, s_featWalkTargetX)
LOAD_FLOAT(IpcBridge_GetWalkTargetY, s_featWalkTargetY)
LOAD_BOOL(IpcBridge_GetWalkTargetActive, s_featWalkTargetActive)
void IpcBridge_SetWalkTarget(float worldX, float worldY, bool active) { s_featWalkTargetX.store(worldX, std::memory_order_relaxed); s_featWalkTargetY.store(worldY, std::memory_order_relaxed); s_featWalkTargetActive.store(active ? 1 : 0, std::memory_order_relaxed); }

LOAD_BOOL(IpcBridge_GetCameraZoomActive, s_featCameraZoomActive)
LOAD_FLOAT(IpcBridge_GetCameraZoomValue, s_featCameraZoomValue)
void IpcBridge_SetCameraZoom(bool active, float zoom) { s_featCameraZoomActive.store(active ? 1 : 0, std::memory_order_relaxed); s_featCameraZoomValue.store(zoom, std::memory_order_relaxed); }
LOAD_BOOL(IpcBridge_GetCameraAngleActive, s_featCameraAngleActive)
LOAD_INT(IpcBridge_GetCameraAngleValue, s_featCameraAngleValue)
void IpcBridge_SetCameraAngle(bool active, int angle) { s_featCameraAngleActive.store(active ? 1 : 0, std::memory_order_relaxed); s_featCameraAngleValue.store(angle, std::memory_order_relaxed); }
LOAD_BOOL(IpcBridge_GetCameraCenteringActive, s_featCameraCenteringActive)
LOAD_BOOL(IpcBridge_GetCameraCentered, s_featCameraCentered)
void IpcBridge_SetCameraCentering(bool active, bool centered) { s_featCameraCenteringActive.store(active ? 1 : 0, std::memory_order_relaxed); s_featCameraCentered.store(centered ? 1 : 0, std::memory_order_relaxed); }

LOAD_BOOL(IpcBridge_GetSkinOverrideEnabled, s_featSkinOverrideEnabled)
LOAD_INT(IpcBridge_GetSkinOverrideId, s_featSkinOverrideId)
void IpcBridge_SetSkinOverride(bool enabled, int skinId) { s_featSkinOverrideEnabled.store(enabled ? 1 : 0, std::memory_order_relaxed); s_featSkinOverrideId.store(skinId, std::memory_order_relaxed); SkinChanger::SetOverride(enabled, skinId); }
int32_t IpcBridge_GetClientDefense() { return s_featClientDefense.load(std::memory_order_relaxed); }
int32_t IpcBridge_GetClientClassType() { return s_featClientClassType.load(std::memory_order_relaxed); }

#undef LOAD_BOOL
#undef LOAD_INT
#undef LOAD_FLOAT
#undef STORE_BOOL

// Floating text and render-thread apply entry

static void ApplyPluginFloatingTextFeatureState()
{
    char text[128] = {};
    {
        std::lock_guard<std::mutex> lk(s_pluginFloatingTextMutex);
        if (!s_pluginFloatingTextPending) return;
        strncpy_s(text, sizeof(text), s_pluginFloatingText, _TRUNCATE);
    }

    Il2CppClass* klass = Resolver::FindClassLoose("MapObjectUIManager");
    const MethodInfo* mi = klass ? il2cpp_class_get_method_from_name(klass, "ShowFloatingText", 6) : nullptr;
    app::MapObjectUIManager* localMgr = nullptr;
    void* local = GameState::GetLocalPtr();

    Resolver::Protection::safe_call([&]() {
        auto* localView = *reinterpret_cast<app::ViewHandler**>(reinterpret_cast<uintptr_t>(local) + RuntimeOffsets::KJ_ViewHandler);
        if (localView) localMgr = localView->fields.GUIManager;
    });

    void* receiver = localMgr;
    if (!receiver && klass) { auto objs = Resolver::FindObjectsByType(klass); if (!objs.empty()) receiver = objs[0]; }
    if (!receiver || !mi || !mi->methodPointer) return;

    using Fn = void(*)(void*, app::DGKAANOAENH__Enum, app::String*, UnityNullableColor32Abi, float, float, float, const MethodInfo*);
    auto showFloatingText = reinterpret_cast<Fn>(mi->methodPointer);
    const bool off = strstr(text, "Disabled") != nullptr;

    UnityNullableColor32Abi col{};
    col.hasValue = true; col.rgba = off ? PackColor32(255, 0, 25) : PackColor32(32, 220, 0);

    static void* s_primedReceiver = nullptr;
    if (s_primedReceiver != receiver) {
        app::String* emptyText = reinterpret_cast<app::String*>(il2cpp_string_new(""));
        const bool primeOk = Resolver::Protection::safe_call([&]() { for (int i = 0; i < 12; ++i) showFloatingText(receiver, app::DGKAANOAENH__Enum::Xp, emptyText, col, 0.f, 0.f, 0.f, mi); });
        if (primeOk) s_primedReceiver = receiver;
        return;
    }

    app::String* ilText = reinterpret_cast<app::String*>(il2cpp_string_new(text));
    const bool ok = Resolver::Protection::safe_call([&]() { showFloatingText(receiver, app::DGKAANOAENH__Enum::Xp, ilText, col, 0.f, 0.f, 0.f, mi); });
    if (ok) {
        std::lock_guard<std::mutex> lk(s_pluginFloatingTextMutex);
        if (strcmp(s_pluginFloatingText, text) == 0) s_pluginFloatingTextPending = false;
    }
}

void IpcBridge_ApplyFeatureOverrides()
{
    ApplyPlayerNoclipFeatureState();
    if (GameState::GetLocalPtr() == nullptr) return;
    if (GameState::GetWorldMgr() == nullptr) return;
    ApplyAutoAimFeatureState(); ApplyProjectileNoclipFeatureState(); ApplyAutoDodgeFeatureState(); ApplyAutoAbilityFeatureState();
    ApplyWalkTargetFeatureState(); ApplyCameraFeatureState(); ApplyPluginFloatingTextFeatureState();
}

static std::atomic<bool> s_shutdown{false};
void IpcBridge_RequestShutdown() { s_shutdown = true; }

// Pending pipe events

struct PendingEvent { char pluginId[32]; char action[128]; };
static std::mutex s_pendingEventsMutex;
static std::vector<PendingEvent> s_pendingEvents;
static constexpr size_t kPendingEventsCap = 64;
void IpcBridge_EmitPredictedHit(int ownerObjId, int bulletId)
{
    PendingEvent ev{};
    std::snprintf(ev.pluginId, sizeof(ev.pluginId), "%s", "ghostHit");
    std::snprintf(ev.action, sizeof(ev.action), "%d:%d", ownerObjId, bulletId);
    std::lock_guard<std::mutex> lk(s_pendingEventsMutex);
    if (s_pendingEvents.size() < kPendingEventsCap) s_pendingEvents.push_back(ev);
}

// Pipe framing

static bool PipeWriteMessage(HANDLE hPipe, const char* json, int len)
{
    uint32_t netLen = static_cast<uint32_t>(len);
    DWORD written = 0;
    if (!WriteFile(hPipe, &netLen, 4, &written, NULL) || written != 4) return false;
    if (!WriteFile(hPipe, json, netLen, &written, NULL) || written != netLen) return false;
    return true;
}

static int PipeReadMessage(HANDLE hPipe, char* buf, int bufSize)
{
    DWORD bytesAvail = 0;
    if (!PeekNamedPipe(hPipe, NULL, 0, NULL, &bytesAvail, NULL)) return -1;
    if (bytesAvail < 4) return 0;
    uint32_t msgLen = 0;
    DWORD bytesRead = 0;
    if (!ReadFile(hPipe, &msgLen, 4, &bytesRead, NULL) || bytesRead != 4) return -1;
    if (msgLen == 0 || msgLen >= (uint32_t)bufSize) return -1;
    if (!ReadFile(hPipe, buf, msgLen, &bytesRead, NULL) || bytesRead != msgLen) return -1;
    buf[msgLen] = '\0';
    return (int)msgLen;
}

// Outbound JSON builders

static int BuildHelloJson(char* buf, int bufSize, const char* challenge)
{
    return snprintf(buf, bufSize, "{\"type\":\"hello\",\"version\":3,\"protocol\":\"bridge-v3\",\"challenge\":\"%s\",\"features\":[\"autoDodge\",\"autoAim\",\"tileMap\"]}", challenge);
}

static int BuildAuthResultJson(char* buf, int bufSize, bool ok, const char* response)
{
    return ok ? snprintf(buf, bufSize, "{\"type\":\"authResult\",\"ok\":true,\"response\":\"%s\"}", response) : snprintf(buf, bufSize, "{\"type\":\"authResult\",\"ok\":false}");
}

static int BuildSignedStringJson(char* buf, int bufSize, const char* type, const char* key, const char* value, uint64_t seq, const char* mac)
{
    return snprintf(buf, bufSize, "{\"type\":\"%s\",\"%s\":\"%s\",\"seq\":\"%llu\",\"mac\":\"%s\"}", type, key, value, static_cast<unsigned long long>(seq), mac);
}

static int BuildHeartbeatJson(char* b, int n, const char* nonce, uint64_t seq, const char* mac) { return BuildSignedStringJson(b, n, "heartbeat", "nonce", nonce, seq, mac); }
static int BuildHeartbeatRespJson(char* b, int n, const char* resp, uint64_t seq, const char* mac) { return BuildSignedStringJson(b, n, "heartbeatResp", "response", resp, seq, mac); }
static int BuildUnresolvedClassesJson(char* b, int n, const char* classes, uint64_t seq, const char* mac) { return BuildSignedStringJson(b, n, "unresolvedClasses", "classes", classes, seq, mac); }
static int BuildPlayerJson(char* buf, int bufSize, uint64_t seq, const char* mac)
{
    float posX = LocalPlayer::GetX(), posY = LocalPlayer::GetY();
    int32_t hp = LocalPlayer::GetHP(), maxHp = LocalPlayer::GetMaxHP(), def = LocalPlayer::GetDefense();
    if (!LocalPlayer::GetPtr())
        return snprintf(buf, bufSize, "{\"type\":\"player\",\"alive\":false,\"seq\":\"%llu\",\"mac\":\"%s\"}", static_cast<unsigned long long>(seq), mac);
    return snprintf(buf, bufSize, "{\"type\":\"player\",\"alive\":true,\"hp\":%d,\"maxHp\":%d,\"def\":%d,\"posX\":%.3f,\"posY\":%.3f,\"seq\":\"%llu\",\"mac\":\"%s\"}", hp, maxHp, def, (double)posX, (double)posY, static_cast<unsigned long long>(seq), mac);
}

static int BuildHotkeyEventJson(char* buf, int bufSize, const char* pluginId, const char* action, bool value, uint64_t seq, const char* mac)
{
    return snprintf(buf, bufSize, "{\"type\":\"hotkeyEvent\",\"pluginId\":\"%s\",\"action\":\"%s\",\"value\":%s,\"seq\":\"%llu\",\"mac\":\"%s\"}", pluginId, action, value ? "true" : "false", static_cast<unsigned long long>(seq), mac);
}

// JSON parsing helpers

static char* JsonGetString(char* json, const char* key, char* valBuf, int valBufSize)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    char* start = strstr(json, pattern);
    if (!start) return NULL;
    start += strlen(pattern);
    char* end = strchr(start, '"');
    if (!end) return NULL;
    int len = (int)(end - start);
    if (len >= valBufSize) len = valBufSize - 1;
    memcpy(valBuf, start, len);
    valBuf[len] = '\0';
    return valBuf;
}

static bool JsonGetBool(char* json, const char* key)
{
    char valBuf[16] = {};
    if (JsonGetString(json, key, valBuf, sizeof(valBuf))) return strcmp(valBuf, "true") == 0 || strcmp(valBuf, "1") == 0;
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char* p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    while (*p == ' ') p++;
    return strncmp(p, "true", 4) == 0;
}

static bool JsonGetNumberToken(char* json, const char* key, char* outBuf, int outBufSize)
{
    if (!json || !key || !outBuf || outBufSize <= 1) return false;
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char* p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    while (*p == ' ') p++;
    int i = 0;
    if (*p == '-') { if (i < outBufSize - 1) outBuf[i++] = *p; ++p; }
    bool seenDigit = false;
    while ((*p >= '0' && *p <= '9') || *p == '.') { seenDigit = true; if (i >= outBufSize - 1) return false; outBuf[i++] = *p++; }
    if (!seenDigit) return false;
    outBuf[i] = '\0';
    return true;
}

// Session validation and signing helpers

static bool IsAsciiIdSafe(const char* s)
{
    if (!s || !*s) return false;
    size_t len = strlen(s);
    if (len > 96) return false;
    for (size_t i = 0; i < len; ++i) {
        const char c = s[i];
        const bool ok =
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.';
        if (!ok) return false;
    }
    return true;
}

static bool ParseUint64Dec(const char* s, uint64_t* out)
{
    if (!s || !*s || !out) return false;
    uint64_t acc = 0;
    for (const char* p = s; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        const uint64_t digit = static_cast<uint64_t>(*p - '0');
        const uint64_t next = acc * 10ULL + digit;
        if (next < acc) return false;
        acc = next;
    }
    *out = acc;
    return true;
}

static bool ConstantTimeHexEq64(const char* a, const char* b)
{
    if (!a || !b) return false;
    if (strlen(a) != 64 || strlen(b) != 64) return false;
    uint8_t diff = 0;
    for (int i = 0; i < 64; ++i) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    return diff == 0;
}

static bool DeriveSessionKey(const char* serverChallenge, const char* clientChallenge, const char* userId, const char* clientPid, uint8_t outKey[32])
{
    if (!serverChallenge || !clientChallenge || !userId || !clientPid || !outKey) return false;
    if (strlen(serverChallenge) != 64 || !Handshake::IsHexString(serverChallenge, 64)) return false;
    if (strlen(clientChallenge) != 64 || !Handshake::IsHexString(clientChallenge, 64)) return false;
    if (!IsAsciiIdSafe(userId)) return false;
    uint64_t pidNum = 0;
    if (!ParseUint64Dec(clientPid, &pidNum) || pidNum == 0) return false;
    char data[512] = {};
    snprintf(
        data, sizeof(data),
        "%s|%s|%s|%s|%s|session-v2",
        serverChallenge, clientChallenge, userId, clientPid, PipeName()
    );
    return Handshake::HmacSha256(
        Handshake::GetSharedKey(), 32,
        reinterpret_cast<const uint8_t*>(data), strlen(data),
        outKey
    );
}

static bool ComputeSessionMacHex(const uint8_t key[32], uint64_t seq, const char* type, const char* payload, char outHex[65])
{
    if (!key || !type || !payload || !outHex) return false;
    char data[1024] = {};
    snprintf(data, sizeof(data), "%llu|%s|%s", static_cast<unsigned long long>(seq), type, payload);
    uint8_t mac[32] = {};
    if (!Handshake::HmacSha256(
        key, 32,
        reinterpret_cast<const uint8_t*>(data), strlen(data),
        mac
    )) return false;
    Handshake::ToHex(mac, 32, outHex);
    return true;
}

static bool WriteSignedHotkeyEvent(HANDLE hPipe, char* msgBuf, int msgBufSize, const char* pluginId, const char* action, bool value)
{
    if (!hPipe || !msgBuf || !pluginId || !action) return false;
    char payload[128] = {};
    snprintf(payload, sizeof(payload), "%s|%s|%s", pluginId, action, value ? "true" : "false");
    const uint64_t outSeq = s_auth.nextServerSeq++;
    char outMac[65] = {};
    if (!ComputeSessionMacHex(s_auth.sessionKey, outSeq, "hotkeyEvent", payload, outMac)) return false;
    const int len = BuildHotkeyEventJson(msgBuf, msgBufSize, pluginId, action, value, outSeq, outMac);
    return PipeWriteMessage(hPipe, msgBuf, len);
}
static void BuildPlayerSigPayload(char* outBuf, int outBufSize)
{
    if (!outBuf || outBufSize <= 0) return;
    float posX = LocalPlayer::GetX();
    float posY = LocalPlayer::GetY();
    int32_t hp    = LocalPlayer::GetHP();
    int32_t maxHp = LocalPlayer::GetMaxHP();
    int32_t def   = LocalPlayer::GetDefense();
    if (!LocalPlayer::GetPtr()) {snprintf(outBuf, outBufSize, "alive:false"); return; }
    snprintf(outBuf, outBufSize, "alive:true|hp:%d|maxHp:%d|posX:%.3f|posY:%.3f|def:%d", hp, maxHp, (double)posX, (double)posY, def);
}
static bool VerifyClientSeqAndMac(const char* seqStr, const char* macHex, const char* type, const char* payload)
{
    (void)macHex; (void)type; (void)payload;
    if (!s_auth.authenticated) return false;
    if (!seqStr) return false;
    uint64_t seq = 0;
    if (!ParseUint64Dec(seqStr, &seq)) return false;
    if (seq <= s_auth.lastClientSeq) return false;
    s_auth.lastClientSeq = seq;
    return true;
}

// Auth and heartbeat dispatcher

static void WriteAuthResult(HANDLE hPipe, char* msgBuf, int msgBufSize, bool ok, const char* response = "")
{
    PipeWriteMessage(hPipe, msgBuf, BuildAuthResultJson(msgBuf, msgBufSize, ok, response));
}

static bool DispatchAuthMessage(char* json, HANDLE hPipe, char* msgBuf, int msgBufSize)
{
    char typeBuf[64] = {};
    if (!JsonGetString(json, "type", typeBuf, sizeof(typeBuf))) return false;
    if (strcmp(typeBuf, "auth") == 0) {
        char userId[128] = {}, response[128] = {}, clientChallenge[128] = {}, protocol[32] = {}, clientPid[32] = {};
        if (!JsonGetString(json, "userId", userId, sizeof(userId)) ||
            !JsonGetString(json, "response", response, sizeof(response)) ||
            !JsonGetString(json, "challenge", clientChallenge, sizeof(clientChallenge)) ||
            !JsonGetString(json, "protocol", protocol, sizeof(protocol)) ||
            !JsonGetString(json, "clientPid", clientPid, sizeof(clientPid))) { DbgLog("Auth message missing required fields."); WriteAuthResult(hPipe, msgBuf, msgBufSize, false); return true; }

        if (!IsAsciiIdSafe(userId) ||
            strcmp(protocol, "bridge-v3") != 0 ||
            strlen(response) != 64 || !Handshake::IsHexString(response, 64) ||
            strlen(clientChallenge) != 64 || !Handshake::IsHexString(clientChallenge, 64) ||
            strlen(s_auth.pendingChallenge) != 64 || !Handshake::IsHexString(s_auth.pendingChallenge, 64)) { DbgLog("Auth payload failed format validation."); WriteAuthResult(hPipe, msgBuf, msgBufSize, false); return true; }
        strncpy_s(s_auth.userId, sizeof(s_auth.userId), userId, _TRUNCATE);

        if (!DeriveSessionKey(s_auth.pendingChallenge, clientChallenge, s_auth.userId, clientPid, s_auth.sessionKey)) { DbgLog("Session key derivation failed."); WriteAuthResult(hPipe, msgBuf, msgBufSize, false); return true; }
        s_auth.authenticated = true; s_auth.sessionReady = true; s_auth.heartbeatMisses = 0; s_auth.lastHeartbeatRecv = GetTickCount64(); s_auth.lastClientSeq = 0; s_auth.nextServerSeq = 1;
        DbgLog("Auth OK: userId=%s", userId);
        char dllResponse[65] = {};
        if (!Handshake::ComputeResponse(clientChallenge, strlen(clientChallenge), dllResponse)) { DbgLog("Auth response generation failed."); WriteAuthResult(hPipe, msgBuf, msgBufSize, false); return true; }
        WriteAuthResult(hPipe, msgBuf, msgBufSize, true, dllResponse);
        return true;
    }

    if (strcmp(typeBuf, "heartbeatResp") == 0) {
        char response[128] = {}, seqStr[32] = {}, macHex[128] = {};
        if (!JsonGetString(json, "response", response, sizeof(response))) return true;
        if (!JsonGetString(json, "seq", seqStr, sizeof(seqStr))) return true;
        if (!JsonGetString(json, "mac", macHex, sizeof(macHex))) return true;
        if (strlen(response) != 64 || !Handshake::IsHexString(response, 64) || !VerifyClientSeqAndMac(seqStr, macHex, "heartbeatResp", response)) { s_auth.heartbeatMisses++; return true; }
        if (s_auth.challengePending && Handshake::VerifyResponse(s_auth.pendingChallenge, strlen(s_auth.pendingChallenge), response)) { s_auth.challengePending = false; s_auth.heartbeatMisses = 0; s_auth.lastHeartbeatRecv = GetTickCount64(); }
        else s_auth.heartbeatMisses++;
        return true;
    }

    if (strcmp(typeBuf, "heartbeat") == 0) {
        char nonce[128] = {}, seqStr[32] = {}, macHex[128] = {};
        if (!JsonGetString(json, "nonce", nonce, sizeof(nonce))) return true;
        if (!JsonGetString(json, "seq", seqStr, sizeof(seqStr))) return true;
        if (!JsonGetString(json, "mac", macHex, sizeof(macHex))) return true;
        if (strlen(nonce) != 64 || !Handshake::IsHexString(nonce, 64)) return true;
        if (!VerifyClientSeqAndMac(seqStr, macHex, "heartbeat", nonce)) return true;
        char resp[65] = {}, outMac[65] = {};
        if (!Handshake::ComputeResponse(nonce, strlen(nonce), resp)) return true;
        const uint64_t outSeq = s_auth.nextServerSeq++;
        if (!ComputeSessionMacHex(s_auth.sessionKey, outSeq, "heartbeatResp", resp, outMac)) return true;
        PipeWriteMessage(hPipe, msgBuf, BuildHeartbeatRespJson(msgBuf, msgBufSize, resp, outSeq, outMac));
        return true;
    }
    return false;
}

// Feature command parsing and dispatch

struct FeatureCommand {
    char key[64] = {};
    char valueType[8] = {};
    char value[128] = {};

    bool Is(const char* name) const { return strcmp(key, name) == 0; }
    bool Bool() const { return strcmp(value, "true") == 0 || strcmp(value, "1") == 0; }
    int Int() const { return atoi(value); }
    float Float() const { return static_cast<float>(atof(value)); }
};

static void StoreBool(std::atomic<int>& target, bool value) { target.store(value ? 1 : 0, std::memory_order_relaxed); }
static void StoreInt(std::atomic<int>& target, int value) { target.store(value, std::memory_order_relaxed); }
static void StoreInt32(std::atomic<int32_t>& target, int value) { target.store(static_cast<int32_t>(value), std::memory_order_relaxed); }
static void StoreFloat(std::atomic<float>& target, float value) { target.store(value, std::memory_order_relaxed); }

struct FeatureHandler { const char* key; bool (*apply)(const FeatureCommand&); };

static bool ApplyFeatureTable(const FeatureCommand& f, const FeatureHandler* handlers, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        if (f.Is(handlers[i].key)) return handlers[i].apply(f);
    return false;
}

#define FH(key, body) { key, [](const FeatureCommand& f)->bool { body; return true; } }
#define FH_BOOL(key, fn) FH(key, fn(f.Bool()))
#define FH_INT(key, fn) FH(key, fn(f.Int()))
#define FH_INT_BOOL(key, fn) FH(key, fn(f.Int() != 0))
#define FH_FLOAT(key, fn) FH(key, fn(f.Float()))
#define FH_TEXT(key, fn) FH(key, fn(f.value))
#define FH_STORE_BOOL(key, atom) FH(key, StoreBool(atom, f.Bool()))
#define FH_STORE_INT(key, atom) FH(key, StoreInt(atom, f.Int()))
#define FH_STORE_INT32(key, atom) FH(key, StoreInt32(atom, f.Int()))
#define FH_STORE_FLOAT(key, atom) FH(key, StoreFloat(atom, f.Float()))

static bool ParseSetFeatureCommand(char* json, const char* seqStr, const char* macHex, FeatureCommand* out)
{
    if (!out) return false;
    if (!JsonGetString(json, "key", out->key, sizeof(out->key))) return false;
    if (!JsonGetString(json, "valueType", out->valueType, sizeof(out->valueType))) return false;
    if (strcmp(out->valueType, "b") == 0) {
        strncpy_s(out->value, sizeof(out->value), JsonGetBool(json, "value") ? "true" : "false", _TRUNCATE);
    } else if (strcmp(out->valueType, "n") == 0) {
        if (!JsonGetNumberToken(json, "value", out->value, sizeof(out->value))) return false;
    } else if (strcmp(out->valueType, "s") == 0) {
        if (!JsonGetString(json, "value", out->value, sizeof(out->value))) return false;
    } else {
        return false;
    }
    char payload[256] = {};
    snprintf(payload, sizeof(payload), "%s|%s|%s", out->key, out->valueType, out->value);
    if (!VerifyClientSeqAndMac(seqStr, macHex, "setFeature", payload)) {
        DBG_FILE_LOG("[IpcBridge] setFeature HMAC REJECTED: key=" << out->key << " valueType=" << out->valueType << " value=" << out->value);
        return false;
    }
    DBG_FILE_LOG("[IpcBridge] setFeature: key=" << out->key << " valueType=" << out->valueType << " value=" << out->value);
    return true;
}

static void QueuePluginFloatingText(const char* text)
{
    std::lock_guard<std::mutex> lk(s_pluginFloatingTextMutex);
    strncpy_s(s_pluginFloatingText, sizeof(s_pluginFloatingText), text ? text : "", _TRUNCATE);
    s_pluginFloatingTextPending = true;
}

static bool DispatchTileCommand(const char* type, char* json, const char* seqStr, const char* macHex)
{
    if (strcmp(type, "clearTiles") == 0) {
        if (!VerifyClientSeqAndMac(seqStr, macHex, "clearTiles", "")) return true;
        IpcTileState::ClearTiles();
        return true;
    }
    if (strcmp(type, "noWalkInit") == 0) {
        char typesBuf[8192] = {};
        if (!JsonGetString(json, "types", typesBuf, sizeof(typesBuf))) return true;
        if (!VerifyClientSeqAndMac(seqStr, macHex, "noWalkInit", typesBuf)) return true;
        IpcTileState::InitNoWalkTypes(typesBuf);
        return true;
    }
    if (strcmp(type, "tileUpdate") == 0) {
        char tilesBuf[65000] = {};
        if (!JsonGetString(json, "tiles", tilesBuf, sizeof(tilesBuf))) return true;
        if (!VerifyClientSeqAndMac(seqStr, macHex, "tileUpdate", tilesBuf)) return true;
        IpcTileState::ApplyTileUpdate(tilesBuf);
        return true;
    }
    return false;
}

static bool ApplyCoreFeature(const FeatureCommand& f)
{
    static const FeatureHandler h[] = {
        FH("overlayEnabled", {
            const bool on = f.Bool();
            s_overlayEnabled.store(on, std::memory_order_relaxed);
            if (!on) settings.bShowMenu = false;
            DbgLog("overlayEnabled = %s", on ? "true" : "false");
        }),
        FH("internalUnloadDll", {
            if (f.Bool()) {
                DBG_FILE_LOG("[IpcBridge] internalUnloadDll requested.");
                IpcBridge_RequestShutdown();
                if (hUnloadEvent) SetEvent(hUnloadEvent);
            }
        }),
        FH_BOOL("autoAimEnabled", IpcBridge_SetAutoAimEnabled),
        FH_INT("autoAimMode", IpcBridge_SetAutoAimMode),
        FH_BOOL("autoAimPrioritizeBosses", AutoAim::SetPrioritizeBosses),
        FH_BOOL("autoAimIgnoreWalls", AutoAim::SetIgnoreWalls),
        FH("projectileNoclipEnabled", {
            const bool on = f.Bool();
            StoreBool(s_featProjectileNoclipEnabled, on);
            if (!on) ProjNoclip::SetEnabled(false);
        }),
        FH_STORE_INT32("clientDefense", s_featClientDefense),
        FH_STORE_INT32("clientClassType", s_featClientClassType),
        FH_INT("autoDodgeMode", IpcBridge_SetAutoDodgeMode),
        FH_FLOAT("autoDodgeHorizonMs", IpcBridge_SetAutoDodgeHorizonMs),
        FH_FLOAT("autoDodgeHitboxPadding", IpcBridge_SetAutoDodgeHitboxPadding),
        FH_BOOL("autoDodgeWallAvoid", IpcBridge_SetAutoDodgeWallAvoid),
        FH("gameHitboxMult", TestTAB::SetGameHitboxOverride(f.Float() < 1.0f - 1e-4f, f.Float())),
        FH_FLOAT("speedHackMult", SpeedHack::SetMultiplier),
        FH_BOOL("autoAbilityEnabled", IpcBridge_SetAutoAbilityEnabled),
        FH_FLOAT("autoAbilityMpPct", IpcBridge_SetAutoAbilityMpPct),
        FH_INT("autoAbilityWizardMode", IpcBridge_SetAutoAbilityWizardMode),
        FH_INT("targetFrameRate", FpsSetter::SetTargetFps),
        FH_TEXT("showPluginFloatingText", QueuePluginFloatingText)
    };
    return ApplyFeatureTable(f, h, sizeof(h) / sizeof(h[0]));
}

static bool ApplyXDodgeFeature(const FeatureCommand& f)
{
    static const FeatureHandler h[] = {
        FH_FLOAT("xdodgeHitScale", XDodge::SetHitScale),
        FH_INT("xdodgeRebuildN", XDodge::SetRebuildN),
        FH_FLOAT("xdodgePlanStepMs", XDodge::SetPlanStepMs),
        FH_FLOAT("xdodgeSearchRadius", XDodge::SetSearchRadius),
        FH_INT_BOOL("xdodgeAstar", XDodge::SetAstarEnabled),
        FH_INT_BOOL("xdodgeWeighting", XDodge::SetWeightingEnabled),
        FH_INT_BOOL("xdodgeSmartGoal", XDodge::SetSmartGoalEnabled),
        FH_INT_BOOL("xdodgePerpBias", XDodge::SetPerpEnabled),
        FH_INT_BOOL("xdodgeSpeedMatch", XDodge::SetSpeedMatchEnabled),
        FH_INT_BOOL("xdodgeLockFollow", DangerPlanner::SetLockFollowEnabled),
        FH_INT("xdodgeAutoLock", DangerPlanner::SetAutoLockMode),
        FH_INT_BOOL("xdodgeWalkCache", XDodge::SetWalkCacheEnabled),
        FH_INT_BOOL("xdodgeWallAvoid", XDodge::SetWallAvoidEnabled),
        FH_INT_BOOL("xdodgeArbiter", XDodge::SetArbiterEnabled),
        FH_INT_BOOL("xdodgeBfsBias", XDodge::SetBfsBiasEnabled),
        FH_INT_BOOL("xdodgeCcd", XDodge::SetCcdEnabled),
        FH_FLOAT("xdodgeCcdPad", XDodge::SetCcdPad),
        FH_INT_BOOL("xdodgeCatalog", XDodge::SetCatalogEnabled),
        FH("xdodgeNotifyHit", XDodge::OnPlayerHit()),
        FH_INT_BOOL("xdodgeDrawPath", XDodge::SetDrawPathEnabled),
        FH_INT_BOOL("xdodgeDrawProjPred", XDodge::SetDrawProjPredEnabled),
        FH_FLOAT("xdodgeDebugPredLongMs", XDodge::SetDebugPredLongMs),
        FH_INT_BOOL("xdodgeAvoidEnemies", XDodge::SetAvoidEnemiesEnabled),
        FH_INT_BOOL("xdodgeGhostHit", GhostHit::SetEnabled),
        FH_INT_BOOL("xdodgeLosGoal", XDodge::SetLosGoalEnabled),
        FH_INT_BOOL("xdodgeWasdYield", XDodge::SetWasdYieldEnabled),
        FH_INT_BOOL("xdodgeLateralPref", XDodge::SetLateralPrefEnabled),
        FH_INT_BOOL("xdodgeGoalSticky", XDodge::SetGoalStickyEnabled),
        FH_FLOAT("xdodgeDangerPenalty", XDodge::SetDangerWeight)
    };
    if (ApplyFeatureTable(f, h, sizeof(h) / sizeof(h[0]))) return true;
    if (f.Is("xdodgeStayPenalty") || f.Is("xdodgeFutureSample") || f.Is("xdodgeFutureHorizon") || f.Is("xdodgeFutureStride")) return true;
    return false;
}

static bool ApplyRolloutFeature(const FeatureCommand& f)
{
    static const FeatureHandler h[] = {
        FH_FLOAT("rolloutHorizonTicks", RolloutDodge::SetHorizonTicks),
        FH_FLOAT("rolloutSampleStepMs", RolloutDodge::SetSampleStepMs),
        FH_INT("rolloutHeadings", RolloutDodge::SetHeadingCount),
        FH_FLOAT("rolloutHitScale", RolloutDodge::SetHitScale),
        FH_FLOAT("rolloutIntentWeight", RolloutDodge::SetIntentWeight),
        FH_INT("rolloutRebuildN", RolloutDodge::SetRebuildN),
        FH_INT_BOOL("rolloutForceBrute", RolloutDodge::SetForceBruteForce),
        FH_INT_BOOL("rolloutAvoidEnemies", RolloutDodge::SetAvoidEnemiesEnabled),
        FH_INT_BOOL("rolloutWasdYield", RolloutDodge::SetWasdYieldEnabled),
        FH_INT_BOOL("rolloutCommitDwell", RolloutDodge::SetCommitDwellEnabled),
        FH_INT_BOOL("rolloutDrawPath", RolloutDodge::SetDrawPathEnabled)
    };
    return ApplyFeatureTable(f, h, sizeof(h) / sizeof(h[0]));
}

static bool ApplyInputCameraSkinFeature(const FeatureCommand& f)
{
    static const FeatureHandler h[] = {
        FH_STORE_BOOL("playerNoclipActive", s_featPlayerNoclipActive),
        FH_STORE_BOOL("playerNoclipEnabled", s_featPlayerNoclipEnabled),
        FH("playerNoclipHotkey", StoreInt(s_featPlayerNoclipHotkeyVk, ResolveHotkeyVk(f.value))),
        FH_STORE_BOOL("socketHotkeyActive", s_featSocketHotkeyActive),
        FH("socketHotkey", StoreInt(s_featSocketHotkeyVk, ResolveHotkeyVk(f.value))),
        FH_STORE_FLOAT("walkTargetX", s_featWalkTargetX),
        FH_STORE_FLOAT("walkTargetY", s_featWalkTargetY),
        FH_STORE_BOOL("walkTargetActive", s_featWalkTargetActive),
        FH("cameraZoomActive", IpcBridge_SetCameraZoom(f.Bool(), s_featCameraZoomValue.load(std::memory_order_relaxed))),
        FH("cameraZoomValue", IpcBridge_SetCameraZoom(s_featCameraZoomActive.load(std::memory_order_relaxed) != 0, f.Float())),
        FH("cameraAngleActive", IpcBridge_SetCameraAngle(f.Bool(), s_featCameraAngleValue.load(std::memory_order_relaxed))),
        FH("cameraAngleValue", IpcBridge_SetCameraAngle(s_featCameraAngleActive.load(std::memory_order_relaxed) != 0, f.Int())),
        FH("cameraCenteringActive", IpcBridge_SetCameraCentering(f.Bool(), s_featCameraCentered.load(std::memory_order_relaxed) != 0)),
        FH("cameraCentered", IpcBridge_SetCameraCentering(s_featCameraCenteringActive.load(std::memory_order_relaxed) != 0, f.Bool())),
        FH("skinOverrideEnabled", IpcBridge_SetSkinOverride(f.Bool(), s_featSkinOverrideId.load(std::memory_order_relaxed))),
        FH("skinOverrideId", IpcBridge_SetSkinOverride(s_featSkinOverrideEnabled.load(std::memory_order_relaxed) != 0, f.Int()))
    };
    return ApplyFeatureTable(f, h, sizeof(h) / sizeof(h[0]));
}

static bool ApplyDangerPlannerFeature(const FeatureCommand& f)
{
    static const FeatureHandler h[] = {
        FH_FLOAT("dodgeWasdLookahead", DangerPlanner::SetWasdLookahead),
        FH_FLOAT("dodgeTightLeash", DangerPlanner::SetTightLeashRadius),
        FH_FLOAT("dodgeIdleMinGain", DangerPlanner::SetIdleMinGain),
        FH_FLOAT("dodgeStickiness", DangerPlanner::SetStickiness),
        FH_BOOL("dodgeReplanOnSpawn", DangerPlanner::SetReplanOnHazardSpawn),
        FH_BOOL("dodgeStrategicBias", DangerPlanner::SetStrategicBiasEnabled),
        FH_BOOL("dodgeStrategicNearWaypoint", DangerPlanner::SetStrategicUseNearWaypoint),
        FH_FLOAT("dodgeHitAversion", DangerPlanner::SetHitAversion),
        FH_FLOAT("dodgeHitScale", DangerPlanner::SetDodgeHitScale)
    };
    return ApplyFeatureTable(f, h, sizeof(h) / sizeof(h[0]));
}

#undef FH_STORE_FLOAT
#undef FH_STORE_INT32
#undef FH_STORE_INT
#undef FH_STORE_BOOL
#undef FH_TEXT
#undef FH_FLOAT
#undef FH_INT_BOOL
#undef FH_INT
#undef FH_BOOL
#undef FH

static void DispatchSetFeature(char* json, const char* seqStr, const char* macHex)
{
    FeatureCommand feature{};
    if (!ParseSetFeatureCommand(json, seqStr, macHex, &feature)) return;
    if (ApplyCoreFeature(feature)) return;
    if (ApplyXDodgeFeature(feature)) return;
    if (ApplyRolloutFeature(feature)) return;
    if (ApplyInputCameraSkinFeature(feature)) return;
    ApplyDangerPlannerFeature(feature);
}

static void DispatchCommand(char* json)
{
    char typeBuf[64] = {};
    char seqStr[32] = {};
    char macHex[128] = {};
    if (!JsonGetString(json, "type", typeBuf, sizeof(typeBuf))) return;
    if (!JsonGetString(json, "seq", seqStr, sizeof(seqStr))) return;
    if (!JsonGetString(json, "mac", macHex, sizeof(macHex))) return;
    if (DispatchTileCommand(typeBuf, json, seqStr, macHex)) return;
    if (strcmp(typeBuf, "setFeature") == 0) DispatchSetFeature(json, seqStr, macHex);
}

// Bridge thread

DWORD WINAPI IpcBridgeThread(LPVOID)
{
    DBG_FILE_LOG("[IpcBridgeThread] Entered (DLL-as-client mode).");
    DbgLog("Thread started.");
    DBG_FILE_LOG("[IpcBridgeThread] Handshake key OK. Connecting to pipe: " << PipeName());
    while (!s_shutdown) {
        if (!WaitNamedPipeA(PipeName(), 2000)) {
            if (s_shutdown) break;
            Sleep(500);
            continue;
        }

        HANDLE hPipe = CreateFileA(PipeName(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hPipe == INVALID_HANDLE_VALUE) {
            const DWORD err = GetLastError();
            DBG_FILE_LOG("[IpcBridgeThread] CreateFile failed: " << err);
            if (err == ERROR_PIPE_BUSY) WaitNamedPipeA(PipeName(), 2000);
            else Sleep(2000);
            continue;
        }

        DWORD pipeMode = PIPE_READMODE_BYTE;
        SetNamedPipeHandleState(hPipe, &pipeMode, NULL, NULL);

        DBG_FILE_LOG("[IpcBridgeThread] Connected to Node.js pipe server. Sending hello...");
        DbgLog("Connected. Sending hello...");
        Handshake::ResetAuthState(&s_auth);

        char msgBuf[PIPE_BUFFER_SIZE];
        char helloChallenge[65] = {};
        if (!Handshake::GenerateChallenge(helloChallenge)) {
            DbgLog("Failed to generate hello challenge.");
            CloseHandle(hPipe);
            Sleep(1000);
            continue;
        }

        strncpy_s(s_auth.pendingChallenge, sizeof(s_auth.pendingChallenge), helloChallenge, _TRUNCATE);
        int len = BuildHelloJson(msgBuf, sizeof(msgBuf), helloChallenge);
        if (!PipeWriteMessage(hPipe, msgBuf, len)) {
            DbgLog("Failed to send hello.");
            CloseHandle(hPipe);
            Sleep(1000);
            continue;
        }

        char readBuf[PIPE_BUFFER_SIZE];
        bool authOk = false;
        ULONGLONG authDeadline = GetTickCount64() + 5000;

        while (GetTickCount64() < authDeadline && !s_shutdown) {
            int readLen = PipeReadMessage(hPipe, readBuf, sizeof(readBuf) - 1);
            if (readLen < 0) break;
            if (readLen > 0) {
                readBuf[readLen] = '\0';
                if (DispatchAuthMessage(readBuf, hPipe, msgBuf, sizeof(msgBuf))) {
                    authOk = s_auth.authenticated;
                    break;
                }
            }
            Sleep(25);
        }

        if (!authOk) {
            DbgLog("Auth failed or timed out. Retrying.");
            Handshake::ResetAuthState(&s_auth);
            CloseHandle(hPipe);
            Sleep(2000);
            continue;
        }

        DbgLog("Authenticated. userId=%s", s_auth.userId);
        ULONGLONG lastPlayerPush = 0;
        s_auth.lastHeartbeatSent = GetTickCount64();
        s_auth.lastHeartbeatRecv = GetTickCount64();
        bool connected = true;
        bool sentUnresolvedClasses = false;

        while (connected && !s_shutdown) {
            int readLen = PipeReadMessage(hPipe, readBuf, sizeof(readBuf) - 1);
            if (readLen < 0) {
                DbgLog("Server disconnected.");
                connected = false;
                break;
            }
            if (readLen > 0) {
                readBuf[readLen] = '\0';
                if (!DispatchAuthMessage(readBuf, hPipe, msgBuf, sizeof(msgBuf)) && Handshake::IsHealthy(&s_auth))
                    DispatchCommand(readBuf);
            }

            ULONGLONG now = GetTickCount64();
            if (now - s_auth.lastHeartbeatSent >= Handshake::HEARTBEAT_INTERVAL_MS) {
                if (s_auth.challengePending) {
                    s_auth.heartbeatMisses++;
                    DbgLog("Heartbeat miss #%d.", s_auth.heartbeatMisses);
                    if (s_auth.heartbeatMisses >= Handshake::HEARTBEAT_MAX_MISSES) {
                        DbgLog("Too many misses - disconnecting.");
                        connected = false;
                        break;
                    }
                }

                char nonce[65] = {};
                if (Handshake::GenerateChallenge(nonce)) {
                    strncpy_s(s_auth.pendingChallenge, sizeof(s_auth.pendingChallenge), nonce, _TRUNCATE);
                    s_auth.challengePending = true; s_auth.lastHeartbeatSent = now;
                    const uint64_t outSeq = s_auth.nextServerSeq++;
                    char outMac[65] = {};
                    if (!ComputeSessionMacHex(s_auth.sessionKey, outSeq, "heartbeat", nonce, outMac)) {
                        connected = false;
                        break;
                    }
                    len = BuildHeartbeatJson(msgBuf, sizeof(msgBuf), nonce, outSeq, outMac);
                    if (!PipeWriteMessage(hPipe, msgBuf, len)) {
                        connected = false;
                        break;
                    }
                }
            }

            if (Handshake::IsHealthy(&s_auth) && now - lastPlayerPush >= 200) {
                lastPlayerPush = now;
                const uint64_t outSeq = s_auth.nextServerSeq++;
                char payload[256] = {};
                BuildPlayerSigPayload(payload, sizeof(payload));
                char outMac[65] = {};
                if (!ComputeSessionMacHex(s_auth.sessionKey, outSeq, "player", payload, outMac)) {
                    connected = false;
                    break;
                }
                len = BuildPlayerJson(msgBuf, sizeof(msgBuf), outSeq, outMac);
                if (!PipeWriteMessage(hPipe, msgBuf, len)) {
                    connected = false;
                    break;
                }
            }

            if (Handshake::IsHealthy(&s_auth)) {
                if (PollSocketHotkeyEvent() && !WriteSignedHotkeyEvent(hPipe, msgBuf, sizeof(msgBuf), "socket", "toggle", true)) {
                    connected = false;
                    break;
                }

                const int noclipEnabled = s_pendingPlayerNoclipEnabled.exchange(-1, std::memory_order_relaxed);
                if (noclipEnabled >= 0 && !WriteSignedHotkeyEvent(hPipe, msgBuf, sizeof(msgBuf), "player-noclip", "noclipEnabled", noclipEnabled != 0)) {
                    connected = false;
                    break;
                }

                std::vector<PendingEvent> drained;
                {
                    std::lock_guard<std::mutex> lk(s_pendingEventsMutex);
                    if (!s_pendingEvents.empty()) drained.swap(s_pendingEvents);
                }
                for (const auto& ev : drained) {
                    if (!WriteSignedHotkeyEvent(hPipe, msgBuf, sizeof(msgBuf), ev.pluginId, ev.action, true)) {
                        connected = false;
                        break;
                    }
                }
                if (!connected) break;
            }

            if (!sentUnresolvedClasses && RuntimeOffsets::HasGivenUp()) {
                sentUnresolvedClasses = true;
                const char* classes = RuntimeOffsets::GetUnresolvedClassNames();
                if (classes && classes[0] != '\0') {
                    const uint64_t outSeq = s_auth.nextServerSeq++;
                    char outMac[65] = {};
                    if (ComputeSessionMacHex(s_auth.sessionKey, outSeq, "unresolvedClasses", classes, outMac)) {
                        len = BuildUnresolvedClassesJson(msgBuf, sizeof(msgBuf), classes, outSeq, outMac);
                        PipeWriteMessage(hPipe, msgBuf, len);
                    }
                }
            }
            Sleep(25);
        }

        Handshake::ResetAuthState(&s_auth);
        CloseHandle(hPipe);
        DbgLog("Disconnected. Will reconnect.");
    }

    DbgLog("Thread exiting.");
    Handshake::ClearSharedKeyCache();
    return 0;
}
