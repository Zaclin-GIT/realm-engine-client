#include "pch-il2cpp.h"
#include "ZaclinDodge.h"

#include "ZaclinDodgeDebug.h"
#include "ZaclinDodgePlanner.h"
#include "ZaclinDodgeSensors.h"

#include "MovementRuntime.h"
#include "SteerInput.h"
#include "gui/tabs/TestTAB.h"

#include <imgui/imgui.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

namespace ZaclinDodge {
namespace {

std::atomic<bool> g_enabled{ false };
std::atomic<float> g_reactWindowMs{ 650.f };
std::atomic<float> g_maxMoveTiles{ 1.25f };
std::atomic<float> g_playerRadius{ 0.25f };
std::atomic<float> g_projectileRadiusFallback{ 0.10f };
std::atomic<float> g_damageThresholdPct{ 0.f };
std::atomic<bool> g_debugOverlay{ false };
std::atomic<bool> g_candidateOverlay{ true };
std::mutex g_debugMutex;
DebugSnapshot g_debug{};

float Clamp(float value, float lo, float hi)
{
    if (!std::isfinite(value)) return lo;
    return std::clamp(value, lo, hi);
}

Settings ReadSettings()
{
    Settings s{};
    s.reactWindowMs = Clamp(g_reactWindowMs.load(std::memory_order_relaxed), 100.f, 2500.f);
    s.maxMoveTiles = Clamp(g_maxMoveTiles.load(std::memory_order_relaxed), 0.2f, 4.f);
    s.playerRadius = Clamp(g_playerRadius.load(std::memory_order_relaxed), 0.05f, 1.f);
    s.projectileRadiusFallback = Clamp(g_projectileRadiusFallback.load(std::memory_order_relaxed), 0.02f, 1.f);
    s.damageThresholdPct = Clamp(g_damageThresholdPct.load(std::memory_order_relaxed), 0.f, 1.f);
    s.debugOverlay = g_debugOverlay.load(std::memory_order_relaxed);
    s.candidateOverlay = g_candidateOverlay.load(std::memory_order_relaxed);
    return s;
}

float MoveBudget(float tilesPerSec, float dt, float maxMoveTiles)
{
    if (!std::isfinite(tilesPerSec) || tilesPerSec < 0.f) tilesPerSec = 0.f;
    if (!std::isfinite(dt) || dt < 0.f) dt = 0.f;
    return std::min(maxMoveTiles, std::max(0.02f, tilesPerSec * dt));
}

void PublishDebug(const DebugSnapshot& snapshot)
{
    std::lock_guard<std::mutex> lock(g_debugMutex);
    g_debug = snapshot;
}

DebugSnapshot ReadDebugSnapshot()
{
    std::lock_guard<std::mutex> lock(g_debugMutex);
    return g_debug;
}

void PublishStatus(FrameStatus status)
{
    DebugSnapshot snapshot = ReadDebugSnapshot();
    snapshot.status = status;
    snapshot.hasSelectedTarget = false;
    snapshot.candidateCount = 0;
    PublishDebug(snapshot);
}

void ApplyDamageThreshold(SensorSnapshot& sensors, int32_t maxHp, float damageThresholdPct)
{
    if (maxHp <= 0 || damageThresholdPct <= 0.f) return;
    const float minDamage = static_cast<float>(maxHp) * damageThresholdPct;
    int kept = 0;
    for (int i = 0; i < std::min(sensors.threatCount, kMaxThreats); ++i) {
        if (sensors.threats[i].damage < minDamage) continue;
        if (kept != i) sensors.threats[kept] = sensors.threats[i];
        ++kept;
    }
    sensors.threatCount = kept;
}

} // namespace

void SetEnabled(bool enabled)
{
    g_enabled.store(enabled, std::memory_order_relaxed);
    if (!enabled) PublishDebug(DebugSnapshot{});
}
bool IsEnabled() { return g_enabled.load(std::memory_order_relaxed); }
void OnEnter() { PublishDebug(DebugSnapshot{}); }

void Tick(void* player, float px, float py, float dt)
{
    if (!IsEnabled()) return;
    if (!player || !std::isfinite(px) || !std::isfinite(py)) { PublishStatus(FrameStatus::NoPlayer); return; }
    if (!DodgeRuntime::EnsureResolved()) { PublishStatus(FrameStatus::MovementFailed); return; }

    SteerInput::Tick();
    const SteerInput::SteerState steer = SteerInput::Get();
    const Settings settings = ReadSettings();

    int32_t hp = 0;
    int32_t maxHp = 0;
    float spd = 0.f;
    float tilesPerSec = 0.f;
    TestTAB::ReadDodgePlayerStats(hp, maxHp, spd, tilesPerSec);
    const float moveBudget = MoveBudget(tilesPerSec, dt, settings.maxMoveTiles);

    PlanRequest req{};
    req.settings = settings;
    req.player = { px, py };
    req.intentDir = steer.active ? Vec2{ steer.dirX, steer.dirY } : Vec2{};
    req.moveBudget = moveBudget;
    req.sensors = Sensors::Build(px, py, settings);
    ApplyDamageThreshold(req.sensors, maxHp, settings.damageThresholdPct);

    const PlanResult plan = Planner::Evaluate(req);

    DebugSnapshot debug{};
    debug.status = plan.status;
    debug.player = req.player;
    debug.intentDir = req.intentDir;
    debug.intendedTarget = { px + req.intentDir.x * moveBudget, py + req.intentDir.y * moveBudget };
    debug.slideDir = plan.slideDir;
    debug.selectedTarget = plan.target;
    debug.hasSelectedTarget = plan.shouldMove;
    debug.sensors = req.sensors;
    debug.candidateCount = std::min(plan.candidateCount, kMaxCandidates);
    for (int i = 0; i < debug.candidateCount; ++i) debug.candidates[i] = plan.candidates[i];

    if (plan.shouldMove) {
        if (!DodgeRuntime::CallMoveTo(player, plan.target.x, plan.target.y))
            debug.status = FrameStatus::MovementFailed;
    }
    PublishDebug(debug);
}

void RenderSettings()
{
    float react = GetReactWindowMs();
    float maxMove = GetMaxMoveTiles();
    float playerRadius = GetPlayerRadius();
    float projRadius = GetProjectileRadiusFallback();
    float dmg = GetDamageThresholdPct();
    bool debug = GetDebugOverlay();
    bool candidates = GetCandidateOverlay();
    if (ImGui::SliderFloat("React window ms##zaclin", &react, 100.f, 2500.f)) SetReactWindowMs(react);
    if (ImGui::SliderFloat("Max assist tiles##zaclin", &maxMove, 0.2f, 4.f)) SetMaxMoveTiles(maxMove);
    if (ImGui::SliderFloat("Player radius##zaclin", &playerRadius, 0.05f, 1.f)) SetPlayerRadius(playerRadius);
    if (ImGui::SliderFloat("Projectile fallback radius##zaclin", &projRadius, 0.02f, 1.f)) SetProjectileRadiusFallback(projRadius);
    if (ImGui::SliderFloat("Damage threshold pct##zaclin", &dmg, 0.f, 1.f)) SetDamageThresholdPct(dmg);
    if (ImGui::Checkbox("Debug overlay##zaclin", &debug)) SetDebugOverlay(debug);
    if (ImGui::Checkbox("Candidate overlay##zaclin", &candidates)) SetCandidateOverlay(candidates);
}

void RenderDebugOverlay(float camX, float camY, float angle, float zoom, float cx, float cy)
{
    if (!GetDebugOverlay()) return;
    const DebugSnapshot snapshot = ReadDebugSnapshot();
    Debug::Render(snapshot, ReadSettings(), camX, camY, angle, zoom, cx, cy);
}

void SetReactWindowMs(float ms) { g_reactWindowMs.store(Clamp(ms, 100.f, 2500.f), std::memory_order_relaxed); }
float GetReactWindowMs() { return g_reactWindowMs.load(std::memory_order_relaxed); }
void SetMaxMoveTiles(float tiles) { g_maxMoveTiles.store(Clamp(tiles, 0.2f, 4.f), std::memory_order_relaxed); }
float GetMaxMoveTiles() { return g_maxMoveTiles.load(std::memory_order_relaxed); }
void SetPlayerRadius(float radius) { g_playerRadius.store(Clamp(radius, 0.05f, 1.f), std::memory_order_relaxed); }
float GetPlayerRadius() { return g_playerRadius.load(std::memory_order_relaxed); }
void SetProjectileRadiusFallback(float radius) { g_projectileRadiusFallback.store(Clamp(radius, 0.02f, 1.f), std::memory_order_relaxed); }
float GetProjectileRadiusFallback() { return g_projectileRadiusFallback.load(std::memory_order_relaxed); }
void SetDamageThresholdPct(float pct) { g_damageThresholdPct.store(Clamp(pct, 0.f, 1.f), std::memory_order_relaxed); }
float GetDamageThresholdPct() { return g_damageThresholdPct.load(std::memory_order_relaxed); }
void SetDebugOverlay(bool enabled) { g_debugOverlay.store(enabled, std::memory_order_relaxed); }
bool GetDebugOverlay() { return g_debugOverlay.load(std::memory_order_relaxed); }
void SetCandidateOverlay(bool enabled) { g_candidateOverlay.store(enabled, std::memory_order_relaxed); }
bool GetCandidateOverlay() { return g_candidateOverlay.load(std::memory_order_relaxed); }

} // namespace ZaclinDodge