#include "pch-il2cpp.h"

#define IMGUI_DEFINE_MATH_OPERATORS

#include "gui/tabs/CameraTAB.h"
#include "gui/tabs/TestTAB.h"
#include "gui/tabs/WorldTAB.h"
#include "W2S.h"
#include "Il2CppResolver.h"
#include "DirectX.h"
#include "FeatureState.h"
#include "DbgFileLog.h"
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <cmath>

// ─────────────────────────────────────────────────────────────────────────────
// Offsets (runtime-confirmed via DIA4A and types_deobf.cs)
// ─────────────────────────────────────────────────────────────────────────────
//
// CameraManager (DecaGames.RotMG.Managers.CameraManager) field layout:
//   +0x28  mainCameraContainer  Transform*    (world-space camera container)
//   +0x50  [UnityEngine.Camera* KNAIAEFDCLM]  (main gameplay camera, not minimap)
//
// UnityEngine.Camera native object — orthographicSize candidates:
//   Try 0x4C first, then 0x44, 0x48, 0x40, 0x50, 0x54 (Unity version varies)
//
// Unity Transform.localPosition:
//   +0x38  float x,  +0x3C  float y,  +0x40  float z
//
// Camera angle is read via the NHPPJHAMCBL getter property (float) on
// CameraManager — NOT SetCameraAngle(int) which is write-only.
// NHPPJHAMCBL getter RVA: 0x01999970  (same as DIA4A "nhppZoomGetter" probe).
// ─────────────────────────────────────────────────────────────────────────────

static constexpr uint32_t OFF_CM_TRANSFORM  = 0x28;  // mainCameraContainer Transform*
static constexpr uint32_t OFF_CM_UNITY_CAM  = 0x50;  // UnityEngine.Camera*


// ─────────────────────────────────────────────────────────────────────────────
// State
// ─────────────────────────────────────────────────────────────────────────────
static std::string  g_status        = "Press Refresh.";
static bool         g_statusOk      = true;

static uintptr_t    g_camMgrPtr     = 0;
static uintptr_t    g_unityCamPtr   = 0;

// Cached class/instance to avoid FindObjectsByType (expensive Unity scan) every refresh.
static Il2CppClass*  s_camMgrClass  = nullptr;
static Il2CppObject* s_cachedCamMgr = nullptr;

static float        g_angle         = 0.f;
static float        g_zoom          = 0.f;
static float        g_posX          = 0.f;
static float        g_posY          = 0.f;
static bool         g_offsetMode    = false;  // IOABMGFJLLP — true = camera NOT centred on player

// Camera.pixelRect: the actual Unity viewport rect in pixels (bottom-left origin).
// x/y = bottom-left corner, w/h = viewport size.
// W2S uses: cx = x + w/2, cy = screenH - (y + h/2)
static float        g_pixelRectX    = 0.f;
static float        g_pixelRectY    = 0.f;
static float        g_pixelRectW    = 0.f;
static float        g_pixelRectH    = 0.f;

// Input fields
static float        g_setZoom       = 8.0f;
static int          g_setAngle      = 0;

// Auto-refresh
static bool         g_autoRefresh   = false;
static float        g_autoTimer     = 0.f;
static float        g_autoInterval  = 1.0f;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static Il2CppClass* FindClassByName(const char* name)
{
    struct Ctx { const char* name; Il2CppClass* result; };
    Ctx ctx{ name, nullptr };
    il2cpp_class_for_each([](Il2CppClass* klass, void* ud) {
        auto* c = static_cast<Ctx*>(ud);
        if (c->result) return;
        if (strcmp(il2cpp_class_get_name(klass), c->name) == 0)
            c->result = klass;
    }, &ctx);
    return ctx.result;
}

template<typename T>
static bool SafeRead(const void* base, uint32_t offset, T& out)
{
    return Resolver::Protection::safe_call([&]() {
        out = *reinterpret_cast<const T*>(
            reinterpret_cast<const uint8_t*>(base) + offset);
    });
}

static bool AddrValid(const void* p)
{
    uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return v > 0x10000 && v < 0x7FFFFFFFFFFull;
}

static bool IsPlausibleOrtho(float f) { return (f == f) && f > 0.05f && f < 500.f; }

// ─────────────────────────────────────────────────────────────────────────────
// Refresh — resolves CameraManager and reads all values
// ─────────────────────────────────────────────────────────────────────────────
static void DoRefresh()
{
    g_camMgrPtr   = 0;
    g_unityCamPtr = 0;
    g_angle = g_zoom = g_posX = g_posY = 0.f;
    g_pixelRectX = g_pixelRectY = g_pixelRectW = g_pixelRectH = 0.f;

    // ── Cached CameraManager lookup — avoids FindObjectsByType every refresh ──
    auto ValidateCamMgr = [&]() -> bool {
        if (!AddrValid(s_cachedCamMgr)) return false;
        void* k = nullptr;
        if (!Resolver::Protection::safe_call([&](){
            k = *reinterpret_cast<void**>(s_cachedCamMgr);
        })) return false;
        return k == s_camMgrClass;
    };

    if (!ValidateCamMgr()) {
        if (!s_camMgrClass)
            s_camMgrClass = FindClassByName("CameraManager");
        if (!s_camMgrClass) {
            g_status  = "ERROR: CameraManager class not found.";
            g_statusOk = false;
            return;
        }
        auto cams = Resolver::FindObjectsByType(s_camMgrClass);
        if (cams.empty()) {
            s_cachedCamMgr = nullptr;
            g_status  = "CameraManager not found — not in-game?";
            g_statusOk = false;
            return;
        }
        s_cachedCamMgr = cams[0];
    }

    Il2CppObject* camMgrObj = s_cachedCamMgr;
    if (!AddrValid(camMgrObj)) {
        g_status  = "ERROR: CameraManager pointer invalid.";
        g_statusOk = false;
        return;
    }
    g_camMgrPtr = reinterpret_cast<uintptr_t>(camMgrObj);

    // ── Camera angle via mainCameraContainer Transform.eulerAngles.z ─────────
    // SetCameraAngle(int) rotates the mainCameraContainer Transform around Z.
    // Reading eulerAngles.z gives the actual current rotation in degrees.
    // (NHPPJHAMCBL getter returns 0 even after angle change — not the angle field)
    {
        void* xfrmForAngle = nullptr;
        SafeRead(camMgrObj, OFF_CM_TRANSFORM, xfrmForAngle);
        if (AddrValid(xfrmForAngle)) {
            Il2CppClass* xk = il2cpp_object_get_class(reinterpret_cast<Il2CppObject*>(xfrmForAngle));
            if (xk) {
                const MethodInfo* getEuler = il2cpp_class_get_method_from_name(xk, "get_eulerAngles", 0);
                if (getEuler) {
                    Il2CppObject* eulerObj = Resolver::Protection::SafeRuntimeInvoke(
                        getEuler, reinterpret_cast<Il2CppObject*>(xfrmForAngle), nullptr);
                    if (eulerObj) {
                        void* unboxed = il2cpp_object_unbox(eulerObj);
                        if (unboxed) {
                            const float* v = reinterpret_cast<const float*>(unboxed);
                            g_angle = v[2];  // z = rotation around world Z axis (yaw)
                        }
                    }
                }
            }
        }
    }
    if (g_setAngle == 0 && g_angle != 0.f)
        g_setAngle = static_cast<int>(g_angle);

    // ── Zoom via orthographicSize property getter on the Unity Camera object ──
    // UnityEngine.Camera stores its data in native Unity memory — raw offset reads
    // don't work reliably. Must call the IL2CPP getter: Camera.get_orthographicSize().
    //
    // Resolve the CameraManager field whose type is UnityEngine.Camera at runtime.
    // The obfuscator reused the name "inputModule" for both the Camera field (0x50
    // in the Apr 6 dump) and a separate CustomInputModule field (0x58), so we can't
    // trust get_field_from_name alone — we iterate and disambiguate by type.
    uint32_t camFieldOff = OFF_CM_UNITY_CAM; // hardcoded fallback
    bool     camFieldResolved = false;
    {
        Il2CppClass* cmKlass = il2cpp_object_get_class(camMgrObj);
        if (cmKlass) {
            void* iter = nullptr;
            while (true) {
                FieldInfo* f = il2cpp_class_get_fields(cmKlass, &iter);
                if (!f) break;
                const Il2CppType* ft = il2cpp_field_get_type(f);
                if (!ft) continue;
                Il2CppClass* fc = il2cpp_type_get_class_or_element_class(ft);
                if (!fc) continue;
                const char* tn = il2cpp_class_get_name(fc);
                if (tn && strcmp(tn, "Camera") == 0) {
                    camFieldOff      = static_cast<uint32_t>(il2cpp_field_get_offset(f));
                    camFieldResolved = true;
                    DBG_FILE_LOG("[CameraTAB] UnityCam field resolved dynamically: name=\""
                        << (il2cpp_field_get_name(f) ? il2cpp_field_get_name(f) : "?")
                        << "\" offset=0x" << std::hex << camFieldOff << std::dec);
                    break;
                }
            }
        }
        if (!camFieldResolved) {
            DBG_FILE_LOG("[CameraTAB] UnityCam field NOT found dynamically — using fallback 0x"
                << std::hex << OFF_CM_UNITY_CAM << std::dec);
        }
    }
    void* unityCam = nullptr;
    SafeRead(camMgrObj, camFieldOff, unityCam);
    if (AddrValid(unityCam)) {
        g_unityCamPtr = reinterpret_cast<uintptr_t>(unityCam);
        g_zoom = Resolver::GetProperty<float>(reinterpret_cast<Il2CppObject*>(unityCam), "orthographicSize");
        if (g_setZoom == 8.0f && IsPlausibleOrtho(g_zoom))
            g_setZoom = g_zoom;

        // ── Camera.pixelRect — the actual game viewport in pixels ────────────
        // Unity returns a Rect value type boxed as Il2CppObject*.
        // Unboxing yields [x, y, width, height] as 4 contiguous floats.
        // x/y = bottom-left corner (Unity Y-up), w/h = extent.
        Il2CppObject* camObj = reinterpret_cast<Il2CppObject*>(unityCam);
        Il2CppClass* camKlass = il2cpp_object_get_class(camObj);
        if (camKlass) {
            const MethodInfo* getPixelRect = il2cpp_class_get_method_from_name(camKlass, "get_pixelRect", 0);
            if (getPixelRect) {
                Il2CppObject* res = Resolver::Protection::SafeRuntimeInvoke(getPixelRect, camObj, nullptr);
                if (res) {
                    float* pr = reinterpret_cast<float*>(il2cpp_object_unbox(res));
                    if (pr) {
                        g_pixelRectX = pr[0];
                        g_pixelRectY = pr[1];
                        g_pixelRectW = pr[2];
                        g_pixelRectH = pr[3];
                    }
                }
            }
        }
    }

    // ── Camera position via Transform.get_position() IL2CPP invoke ─────────────
    // Unity Transform stores position in native memory — raw reads at fixed offsets
    // return localPosition in local space (always 0,0 for anchored containers).
    // Call get_position() to get world-space coordinates.
    void* xfrm = nullptr;
    SafeRead(camMgrObj, OFF_CM_TRANSFORM, xfrm);
    if (AddrValid(xfrm)) {
        Il2CppClass* xfrmKlass = il2cpp_object_get_class(reinterpret_cast<Il2CppObject*>(xfrm));
        if (xfrmKlass) {
            const MethodInfo* getPos = il2cpp_class_get_method_from_name(xfrmKlass, "get_position", 0);
            if (getPos) {
                Il2CppObject* posObj = Resolver::Protection::SafeRuntimeInvoke(
                    getPos, reinterpret_cast<Il2CppObject*>(xfrm), nullptr);
                if (posObj) {
                    void* unboxed = il2cpp_object_unbox(posObj);
                    if (unboxed) {
                        const float* v = reinterpret_cast<const float*>(unboxed);
                        g_posX = v[0];  // Vector3.x
                        g_posY = v[1];  // Vector3.y
                    }
                }
            }
        }
    }

    // ── Offset mode (IOABMGFJLLP — "Toggle Centering of Player") ─────────────
    // IOABMGFJLLP is a bool property on CameraManager; getter = "get_IOABMGFJLLP".
    // true  → camera is NOT centred on the player (player can wander off-screen).
    // false → camera follows the player (default).
    {
        Il2CppClass* camKlass = il2cpp_object_get_class(camMgrObj);
        if (camKlass) {
            // Try all known name variants (BeeByte may store getter as ANBDPNHJBHG)
            const MethodInfo* getter = il2cpp_class_get_method_from_name(camKlass, "ANBDPNHJBHG",    0);
            if (!getter) getter = il2cpp_class_get_method_from_name(camKlass, "get_IOABMGFJLLP", 0);
            if (!getter) getter = il2cpp_class_get_method_from_name(camKlass, "IOABMGFJLLP",    0);
            if (getter) {
                Il2CppObject* res = Resolver::Protection::SafeRuntimeInvoke(getter, camMgrObj, nullptr);
                if (res) {
                    void* ub = il2cpp_object_unbox(res);
                    if (ub) g_offsetMode = *reinterpret_cast<bool*>(ub);
                }
            }
        }
    }

    char buf[80];
    snprintf(buf, sizeof(buf), "Refreshed.  Angle=%.1f  Zoom=%.2f  Pos=(%.1f,%.1f)",
        g_angle, g_zoom, g_posX, g_posY);
    g_status   = buf;
    g_statusOk = true;
}

// Apply a new zoom value by calling Camera.set_orthographicSize() via IL2CPP
static void ApplyZoom(float newZoom)
{
    Il2CppObject* unityCam = reinterpret_cast<Il2CppObject*>(g_unityCamPtr);
    if (!AddrValid(unityCam)) {
        // Lazy-resolve on first apply so the dashboard doesn't need the user to
        // open the DLL menu and click Refresh before camera controls work.
        DoRefresh();
        unityCam = reinterpret_cast<Il2CppObject*>(g_unityCamPtr);
        DBG_FILE_LOG("[ApplyZoom] DoRefresh after null ptr — unityCam=" << (void*)unityCam);
        if (!AddrValid(unityCam)) {
            g_status  = "ERROR: Unity Camera not resolved (lazy refresh failed).";
            g_statusOk = false;
            return;
        }
    }
    DBG_FILE_LOG("[ApplyZoom] writing orthographicSize=" << newZoom << " to unityCam=" << (void*)unityCam);
    Resolver::SetProperty<float>(unityCam, "orthographicSize", newZoom);
    g_zoom  = newZoom;
    char buf[64]; snprintf(buf, sizeof(buf), "Zoom set to %.2f", newZoom);
    g_status = buf; g_statusOk = true;
}

// Apply a new camera angle by calling SetCameraAngle(int) on the CameraManager
static void ApplyAngle(int newAngle)
{
    Il2CppObject* camMgrObj = reinterpret_cast<Il2CppObject*>(g_camMgrPtr);
    if (!AddrValid(camMgrObj)) {
        g_status  = "ERROR: CameraManager not resolved — press Refresh first.";
        g_statusOk = false;
        return;
    }
    Il2CppClass* klass = il2cpp_object_get_class(camMgrObj);
    if (!klass) { g_status = "ERROR: il2cpp_object_get_class returned null."; g_statusOk = false; return; }

    const MethodInfo* setAngle = il2cpp_class_get_method_from_name(klass, "SetCameraAngle", 1);
    if (!setAngle) {
        g_status  = "ERROR: SetCameraAngle(int) method not found.";
        g_statusOk = false;
        return;
    }
    void* params[] = { &newAngle };
    Resolver::Protection::SafeRuntimeInvoke(setAngle, camMgrObj, params);

    char buf[64]; snprintf(buf, sizeof(buf), "Angle set to %d", newAngle);
    g_status = buf; g_statusOk = true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Render
// ─────────────────────────────────────────────────────────────────────────────
void CameraTAB::Render()
{
    // Auto-refresh tick
    const float dt = ImGui::GetIO().DeltaTime;
    if (g_autoRefresh) {
        g_autoTimer -= dt;
        if (g_autoTimer <= 0.f) {
            DoRefresh();
            g_autoTimer = g_autoInterval;
        }
    }

    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.4f, 0.85f, 1.0f, 1.0f), "CAMERA");
    ImGui::SameLine(0.f, 16.f);
    if (ImGui::Button("Refresh")) { DoRefresh(); g_autoTimer = g_autoInterval; }
    ImGui::SameLine();
    ImGui::Checkbox("Auto", &g_autoRefresh);
    if (g_autoRefresh) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(50.f);
        ImGui::DragFloat("##cival", &g_autoInterval, 0.1f, 0.2f, 10.f, "%.1fs");
    }
    ImGui::Separator();

    // Status
    {
        ImVec4 sc = g_statusOk ? ImVec4(0.5f,0.5f,0.5f,1.f) : ImVec4(1.f,0.3f,0.3f,1.f);
        ImGui::TextColored(sc, "%s", g_status.c_str());
    }

    if (g_camMgrPtr) {
        ImGui::Spacing();
        ImGui::TextDisabled("CameraManager  0x%llX", (unsigned long long)g_camMgrPtr);
        if (g_unityCamPtr)
            ImGui::TextDisabled("Unity Camera   0x%llX", (unsigned long long)g_unityCamPtr);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Live IOABMGFJLLP read — re-read every frame so in-game hotkey changes are tracked ──
    // BeeByte names the getter ANBDPNHJBHG (same VA as the property accessor get_IOABMGFJLLP).
    if (AddrValid(s_cachedCamMgr)) {
        static const MethodInfo* s_getterIOAB = nullptr;
        static bool s_getterSearched = false;
        if (!s_getterIOAB && !s_getterSearched) {
            s_getterSearched = true;
            Il2CppClass* k = il2cpp_object_get_class(s_cachedCamMgr);
            if (k) {
                // Try all known names for this getter (BeeByte can store it under any)
                s_getterIOAB = il2cpp_class_get_method_from_name(k, "ANBDPNHJBHG",    0);
                if (!s_getterIOAB)
                    s_getterIOAB = il2cpp_class_get_method_from_name(k, "get_IOABMGFJLLP", 0);
                if (!s_getterIOAB)
                    s_getterIOAB = il2cpp_class_get_method_from_name(k, "IOABMGFJLLP",    0);
            }
        }
        if (s_getterIOAB) {
            Il2CppObject* res = Resolver::Protection::SafeRuntimeInvoke(
                s_getterIOAB, s_cachedCamMgr, nullptr);
            if (res) {
                void* ub = il2cpp_object_unbox(res);
                if (ub) g_offsetMode = *reinterpret_cast<bool*>(ub);
            }
        }
    }

    // ── CENTERING (IOABMGFJLLP / Toggle Centering of Player) ──────────────────
    ImGui::TextColored(ImVec4(0.8f, 0.6f, 1.0f, 1.0f), "CENTERING  (IOABMGFJLLP)");
    ImGui::Indent(8.f);
    bool centeringActive = FeatureState::GetCameraCenteringActive();
    bool centeredOnPlayer = FeatureState::GetCameraCentered();
    if (ImGui::Checkbox("Force centering##cam_force_center", &centeringActive))
        FeatureState::SetCameraCentering(centeringActive, centeredOnPlayer);
    if (centeringActive) {
        ImGui::SameLine();
        if (ImGui::Checkbox("Centered on player##cam_centered", &centeredOnPlayer))
            FeatureState::SetCameraCentering(true, centeredOnPlayer);
    }
    {
        const char* modeLabel = g_offsetMode ? "OFF  (player NOT centred)" : "ON  (following player)";
        ImVec4      modeCol   = g_offsetMode ? ImVec4(1.f, 0.4f, 0.3f, 1.f) : ImVec4(0.3f, 1.f, 0.5f, 1.f);
        ImGui::TextColored(modeCol, "State: %s", modeLabel);
    }
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.25f, 0.15f, 0.40f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.40f, 0.25f, 0.60f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.18f, 0.10f, 0.28f, 1.f));
    if (ImGui::Button("Toggle Centering"))
        FeatureState::SetCameraCentering(true, g_offsetMode != 0);
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    ImGui::TextDisabled("(calls ChangeOffsetMode() — same as H key)");
    ImGui::Unindent(8.f);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── POSITION ──────────────────────────────────────────────────────────────
    ImGui::TextColored(ImVec4(0.8f,0.8f,0.4f,1.f), "POSITION  (mainCameraContainer)");
    ImGui::Indent(8.f);
    ImGui::Text("X: %.4f", g_posX);
    ImGui::SameLine(0.f, 20.f);
    ImGui::Text("Y: %.4f", g_posY);
    ImGui::Unindent(8.f);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── ANGLE ─────────────────────────────────────────────────────────────────
    ImGui::TextColored(ImVec4(0.4f,1.f,0.7f,1.f), "ANGLE  (Transform.eulerAngles.z — degrees)");
    ImGui::Indent(8.f);
    bool angleActive = FeatureState::GetCameraAngleActive();
    int angleValue = FeatureState::GetCameraAngleValue();
    if (ImGui::Checkbox("Lock angle##cam_lock_angle", &angleActive))
        FeatureState::SetCameraAngle(angleActive, angleValue);
    ImGui::Text("Current: %.4f", g_angle);
    ImGui::Spacing();
    ImGui::SetNextItemWidth(100.f);
    if (ImGui::InputInt("##angleInput", &angleValue, 1, 10)) {
        g_setAngle = angleValue;
        FeatureState::SetCameraAngle(angleActive, angleValue);
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.15f,0.4f,0.15f,1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.6f,0.2f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.1f, 0.25f,0.1f,1.f));
    if (ImGui::Button("Set Angle")) FeatureState::SetCameraAngle(true, angleValue);
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    ImGui::TextDisabled("(calls SetCameraAngle(int))");
    ImGui::Unindent(8.f);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── ZOOM ──────────────────────────────────────────────────────────────────
    ImGui::TextColored(ImVec4(1.f,0.6f,0.3f,1.f), "ZOOM  (orthographicSize — smaller = closer)");
    ImGui::Indent(8.f);
    bool zoomActive = FeatureState::GetCameraZoomActive();
    float zoomValue = FeatureState::GetCameraZoomValue();
    if (ImGui::Checkbox("Lock zoom##cam_lock_zoom", &zoomActive))
        FeatureState::SetCameraZoom(zoomActive, zoomValue);
    ImGui::Text("Current: %.4f", g_zoom);
    ImGui::Spacing();
    ImGui::SetNextItemWidth(100.f);
    if (ImGui::DragFloat("##zoomInput", &zoomValue, 0.1f, 0.5f, 100.f, "%.2f")) {
        g_setZoom = zoomValue;
        FeatureState::SetCameraZoom(zoomActive, zoomValue);
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.4f,0.2f,0.05f,1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.6f,0.3f,0.1f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.25f,0.12f,0.02f,1.f));
    if (ImGui::Button("Set Zoom")) FeatureState::SetCameraZoom(true, zoomValue);
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    ImGui::TextDisabled("(writes orthographicSize)");
    ImGui::Unindent(8.f);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Preset angles ─────────────────────────────────────────────────────────
    ImGui::TextDisabled("Quick angle presets:");
    ImGui::Indent(8.f);
    const int presets[] = { 0, 45, 90, 135, 180, 225, 270, 315 };
    for (int i = 0; i < 8; ++i) {
        char lbl[12]; snprintf(lbl, sizeof(lbl), "%d##a%d", presets[i], i);
        if (i > 0) ImGui::SameLine();
        if (ImGui::Button(lbl)) {
            g_setAngle = presets[i];
            FeatureState::SetCameraAngle(true, presets[i]);
        }
    }
    ImGui::Unindent(8.f);

    // ── Preset zooms ──────────────────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::TextDisabled("Quick zoom presets:");
    ImGui::Indent(8.f);
    const float zPresets[] = { 4.f, 6.f, 8.f, 10.f, 14.f, 20.f };
    const char* zLabels[]  = { "x4","x6","x8","x10","x14","x20" };
    for (int i = 0; i < 6; ++i) {
        char lbl[16]; snprintf(lbl, sizeof(lbl), "%s##z%d", zLabels[i], i);
        if (i > 0) ImGui::SameLine();
        if (ImGui::Button(lbl)) {
            g_setZoom = zPresets[i];
            FeatureState::SetCameraZoom(true, zPresets[i]);
        }
    }
    ImGui::Unindent(8.f);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── W2S / S2W DEBUG ───────────────────────────────────────────────────────
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.f, 1.f), "W2S / S2W DEBUG");
    ImGui::Indent(8.f);

    {
        static constexpr float kPI = 3.14159265358979323846f;

        // Build per-frame camera state (same logic as TestTAB::BuildCamState)
        void* livePlayer = WorldTAB::GetLocalPtr();
        float camX = WorldTAB::GetLocalX();
        float camY = WorldTAB::GetLocalY();
        if (livePlayer) {
            __try {
                float lx = *(float*)((uint8_t*)livePlayer + 0x3C);
                float ly = *(float*)((uint8_t*)livePlayer + 0x40);
                if (lx != 0.f || ly != 0.f) { camX = lx; camY = ly; }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }

        float angleDeg = g_angle;
        float ortho    = g_zoom;
        if (ortho == 0.f) ortho = 8.f;
        float angleRad = angleDeg * (kPI / 180.f);

        HWND wnd = DirectX::window;
        float screenW = 0.f, screenH = 0.f;
        if (wnd) {
            RECT r; GetClientRect(wnd, &r);
            screenW = static_cast<float>(r.right  - r.left);
            screenH = static_cast<float>(r.bottom - r.top);
        }

        float cx, cy, zoom;
        if (g_pixelRectW > 16.f && g_pixelRectH > 16.f) {
            cx   = g_pixelRectX + g_pixelRectW * 0.5f;
            cy   = screenH - (g_pixelRectY + g_pixelRectH * 0.5f);
            zoom = g_pixelRectH / (2.f * ortho);
        } else {
            cx   = screenW * 0.5f;
            cy   = screenH * 0.5f;
            zoom = screenH / (2.f * ortho);
        }

        bool stateOk = (screenW > 0.f && (camX != 0.f || camY != 0.f));
        bool w2sOk   = TestTAB::IsW2SValid();

        ImGui::Text("State:  %s", stateOk ? "OK" : "waiting (refresh World + Camera)");
        if (stateOk) {
            ImGui::Text("  Player world:    (%.2f,  %.2f)", camX, camY);
            ImGui::Text("  Camera angle:    %.2f deg", angleDeg);
            ImGui::Text("  OrthoSize:       %.2f   |  zoom: %.2f px/tile", ortho, zoom);
            ImGui::Text("  Screen:          %.0f x %.0f", screenW, screenH);
            ImGui::Text("  Viewport centre: (%.0f, %.0f)  [cx, cy]", cx, cy);
            if (g_pixelRectW > 16.f)
                ImGui::Text("  PixelRect:       x=%.0f  w=%.0f  h=%.0f  (camera rect)",
                    g_pixelRectX, g_pixelRectW, g_pixelRectH);
            else
                ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f),
                    "  PixelRect:       not yet read — refresh Camera tab first");
        }

        ImGui::Spacing();
        float msx = TestTAB::GetMouseScreenX();
        float msy = TestTAB::GetMouseScreenY();
        float mwx = TestTAB::GetMouseWorldX();
        float mwy = TestTAB::GetMouseWorldY();
        ImGui::Text("Mouse  screen:  (%.1f, %.1f)", msx, msy);
        ImGui::Text("Mouse  world:   (%.2f, %.2f)%s", mwx, mwy, w2sOk ? "" : "  [stale]");

        if (stateOk && w2sOk) {
            float rtSX, rtSY;
            W2S(mwx, mwy, rtSX, rtSY, camX, camY, angleRad, zoom, cx, cy);
            ImGui::Text("W2S roundtrip:  (%.1f, %.1f)  err=(%.2f, %.2f)",
                rtSX, rtSY, rtSX - msx, rtSY - msy);
            float dx = mwx - camX, dy = mwy - camY;
            ImGui::Text("World delta:    (%.2f, %.2f)   dist: %.2f tiles",
                dx, dy, sqrtf(dx*dx + dy*dy));
        }
    }
    ImGui::Unindent(8.f);
}

// ── Public API (for TestTAB / W2S) ───────────────────────────────────────────
namespace CameraTAB {
    void  ForceRefresh()
    {
        // Coalesce double-refreshes — TestTAB and DebugTAB both ForceRefresh
        // at ~100 ms with independent timers, so when they align we'd walk
        // the camera-state reads twice in one frame. 50 ms gate lets
        // scheduled ticks through but drops duplicates.
        static ULONGLONG s_lastRefreshMs = 0;
        const ULONGLONG nowMs = GetTickCount64();
        if (nowMs - s_lastRefreshMs < 50ULL) return;
        s_lastRefreshMs = nowMs;
        DoRefresh();
    }
    float GetAngle()         { return g_angle;    }
    float GetZoom()          { return g_zoom;     }
    void* GetCamMgrPtr()     { return reinterpret_cast<void*>(g_camMgrPtr); }
    float GetPixelRectX()    { return g_pixelRectX; }
    float GetPixelRectY()    { return g_pixelRectY; }
    float GetPixelRectW()    { return g_pixelRectW; }
    float GetPixelRectH()    { return g_pixelRectH; }
    bool  GetCenteringState(){ return g_offsetMode; }
    void  SetZoomValue(float zoom)
    {
        // Always apply — the cached g_zoom may be stale if DoRefresh() hasn't run.
        // ApplyZoom updates g_zoom after writing, so back-to-back identical calls are cheap.
        ApplyZoom(zoom);
    }
    void  SetAngleDegrees(int angleDeg)
    {
        // Always apply — g_angle may be stale (only updated by DoRefresh).
        ApplyAngle(angleDeg);
    }
    void  SetCenteredOnPlayer(bool centered)
    {
        if (!AddrValid(reinterpret_cast<void*>(g_camMgrPtr)))
            return;
        // Read live state instead of relying on cached g_offsetMode which may be stale.
        // The Render() function reads the live getter every frame, so piggyback on s_cachedCamMgr.
        bool liveOffsetMode = g_offsetMode;
        if (AddrValid(s_cachedCamMgr)) {
            Il2CppClass* k = il2cpp_object_get_class(s_cachedCamMgr);
            if (k) {
                const MethodInfo* getter = il2cpp_class_get_method_from_name(k, "ANBDPNHJBHG", 0);
                if (!getter) getter = il2cpp_class_get_method_from_name(k, "get_IOABMGFJLLP", 0);
                if (!getter) getter = il2cpp_class_get_method_from_name(k, "IOABMGFJLLP", 0);
                if (getter) {
                    Il2CppObject* res = Resolver::Protection::SafeRuntimeInvoke(getter, s_cachedCamMgr, nullptr);
                    if (res) {
                        void* ub = il2cpp_object_unbox(res);
                        if (ub) liveOffsetMode = *reinterpret_cast<bool*>(ub);
                    }
                }
            }
        }
        const bool currentlyCentered = !liveOffsetMode;
        if (currentlyCentered == centered)
            return;
        Il2CppObject* camMgrObj = reinterpret_cast<Il2CppObject*>(g_camMgrPtr);
        Il2CppClass* k = il2cpp_object_get_class(camMgrObj);
        if (!k)
            return;
        const MethodInfo* fn = il2cpp_class_get_method_from_name(k, "ChangeOffsetMode", 0);
        if (!fn)
            return;
        Resolver::Protection::SafeRuntimeInvoke(fn, camMgrObj, nullptr);
        g_offsetMode = !centered;
    }
}
