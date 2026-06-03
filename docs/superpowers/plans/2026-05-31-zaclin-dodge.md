# Zaclin Dodge Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a new isolated `Zaclin` dodge mode that assists player movement by sliding unsafe intent around projectiles, enemies, and obstacles while preserving the existing XDodge and Rollout modes.

**Architecture:** The new engine lives under `internal/src/features/movement/zdodge` and owns its own settings, sensors, planner, runtime API, and debug overlay. It reuses only low-level sources: `ProjectileTracking` for projectile prediction, `AutoAim::EnumerateLiveEnemies` for enemy bodies, `TestTAB::IsWalkPositionBlocked` for obstacle checks, `SteerInput` for player intent, and `DodgeRuntime` for native speed-clamped movement.

**Tech Stack:** C++20 DLL code with MSVC/PCH, ImGui/W2S debug drawing, existing IPC feature keys, TypeScript plugin settings, Visual Studio `.vcxproj`/`.filters`, Debug x64 MSBuild verification.

---

## File Structure

- Create `internal/src/features/movement/zdodge/ZDodgeTypes.h`: shared constants, settings, sensor structs, planner structs, and debug snapshot structs.
- Create `internal/src/features/movement/zdodge/ZDodgePlanner.h/.cpp`: pure-ish planning helpers for candidate generation, projectile safety, blocker rejection, slide-vector calculation, and target selection.
- Create `internal/src/features/movement/zdodge/ZDodgeSensors.h/.cpp`: adapts current projectile/enemy/obstacle data into bounded snapshots; all fragile reads stay here.
- Create `internal/src/features/movement/zdodge/ZDodgeDebug.h/.cpp`: overlay drawing for projectiles, paths, enemies, obstacles, candidates, intent vector, slide vector, target, and status text.
- Create `internal/src/features/movement/zdodge/ZDodge.h/.cpp`: public mode API, settings setters/getters, per-frame tick, render settings, and render overlay entry points.
- Modify `internal/src/gui/tabs/TestTAB.h/.cpp`: add `DodgeMode::Zaclin`, switch mode enablement, render settings, and overlay call.
- Modify `internal/src/features/control/FeatureState.cpp`: clamp `autoDodgeMode` to include `Zaclin`.
- Modify `internal/src/features/control/FeatureCommandRegistry.cpp`: add `zaclin*` IPC feature handlers.
- Modify `client/plugins/auto-dodge.ts`: add dropdown value and settings sync for Zaclin.
- Modify `internal/il2cpp-dll-injection.vcxproj` and `internal/il2cpp-dll-injection.vcxproj.filters`: include new source/header files and include path.

---

### Task 1: Scaffold Zaclin Types And Pure Planner Core

**Files:**
- Create: `internal/src/features/movement/zdodge/ZDodgeTypes.h`
- Create: `internal/src/features/movement/zdodge/ZDodgePlanner.h`
- Create: `internal/src/features/movement/zdodge/ZDodgePlanner.cpp`

- [ ] **Step 1: Create shared types**

Add `ZDodgeTypes.h` with bounded arrays and explicit status values:

```cpp
#pragma once

#include <cstdint>

namespace ZDodge {

constexpr int kCandidateDirections = 32;
constexpr int kRingPasses = 5;
constexpr int kMaxCandidates = kCandidateDirections * kRingPasses;
constexpr int kMaxThreats = 128;
constexpr int kMaxPathSamples = 24;
constexpr int kMaxBlockers = 128;
constexpr float kTwoPi = 6.28318530717958647692f;

enum class CandidateRejectReason : uint8_t {
    None,
    Projectile,
    Blocker,
    Sweep,
    OutsideMoveBudget
};

enum class FrameStatus : uint8_t {
    Disabled,
    NoPlayer,
    NoThreats,
    IntentSafe,
    SlideAssist,
    CandidateAssist,
    NoSafeCandidate,
    MovementFailed,
    SensorLimited
};

struct Settings {
    float reactWindowMs = 650.f;
    float maxMoveTiles = 1.25f;
    float playerRadius = 0.25f;
    float projectileRadiusFallback = 0.10f;
    float damageThresholdPct = 0.f;
    float clearanceTiles = 0.08f;
    bool debugOverlay = false;
    bool candidateOverlay = true;
};

struct Vec2 {
    float x = 0.f;
    float y = 0.f;
};

struct Threat {
    int32_t id = 0;
    float radius = 0.10f;
    float damage = 0.f;
    int sampleCount = 0;
    Vec2 samples[kMaxPathSamples]{};
};

struct Blocker {
    enum class Kind : uint8_t { Enemy, Obstacle } kind = Kind::Obstacle;
    int32_t id = 0;
    Vec2 pos{};
    float radius = 0.5f;
};

struct SensorSnapshot {
    Threat threats[kMaxThreats]{};
    int threatCount = 0;
    Blocker blockers[kMaxBlockers]{};
    int blockerCount = 0;
    bool projectileLimited = false;
    bool blockerLimited = false;
};

struct CandidateDebug {
    Vec2 pos{};
    float score = 0.f;
    bool safe = false;
    CandidateRejectReason rejectReason = CandidateRejectReason::None;
};

struct DebugSnapshot {
    FrameStatus status = FrameStatus::Disabled;
    Vec2 player{};
    Vec2 intentDir{};
    Vec2 intendedTarget{};
    Vec2 slideDir{};
    Vec2 selectedTarget{};
    bool hasSelectedTarget = false;
    CandidateDebug candidates[kMaxCandidates]{};
    int candidateCount = 0;
    SensorSnapshot sensors{};
};

struct PlanRequest {
    Settings settings{};
    SensorSnapshot sensors{};
    Vec2 player{};
    Vec2 intentDir{};
    float moveBudget = 0.f;
};

struct PlanResult {
    FrameStatus status = FrameStatus::NoThreats;
    Vec2 target{};
    Vec2 slideDir{};
    bool shouldMove = false;
    CandidateDebug candidates[kMaxCandidates]{};
    int candidateCount = 0;
};

} // namespace ZDodge
```

- [ ] **Step 2: Add planner interface**

Create `ZDodgePlanner.h`:

```cpp
#pragma once

#include "ZDodgeTypes.h"

namespace ZDodge::Planner {

PlanResult Evaluate(const PlanRequest& req);
bool IsPointSafe(const Vec2& point, const Settings& settings, const SensorSnapshot& sensors);
bool IsSweepSafe(const Vec2& from, const Vec2& to, const Settings& settings, const SensorSnapshot& sensors);
Vec2 ComputeSlideDirection(const Vec2& desiredDir, const Vec2& player, const Settings& settings, const SensorSnapshot& sensors);

} // namespace ZDodge::Planner
```

- [ ] **Step 3: Implement planner math**

Create `ZDodgePlanner.cpp` with PCH first and these helpers:

```cpp
#include "pch-il2cpp.h"
#include "ZDodgePlanner.h"

#include <algorithm>
#include <cmath>

namespace ZDodge::Planner {
namespace {

float Dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
float LenSq(Vec2 v) { return Dot(v, v); }
float Len(Vec2 v) { return std::sqrt(LenSq(v)); }
Vec2 Add(Vec2 a, Vec2 b) { return { a.x + b.x, a.y + b.y }; }
Vec2 Sub(Vec2 a, Vec2 b) { return { a.x - b.x, a.y - b.y }; }
Vec2 Mul(Vec2 v, float s) { return { v.x * s, v.y * s }; }

Vec2 Normalize(Vec2 v)
{
    const float n = Len(v);
    return n > 0.0001f ? Mul(v, 1.f / n) : Vec2{};
}

float ChebDistance(Vec2 a, Vec2 b)
{
    return std::max(std::fabs(a.x - b.x), std::fabs(a.y - b.y));
}

Vec2 ClosestPointOnSegment(Vec2 point, Vec2 a, Vec2 b)
{
    const Vec2 ab = Sub(b, a);
    const float denom = LenSq(ab);
    if (denom < 0.000001f) return a;
    const float t = std::clamp(Dot(Sub(point, a), ab) / denom, 0.f, 1.f);
    return Add(a, Mul(ab, t));
}

bool ThreatHitsPoint(const Threat& threat, Vec2 point, const Settings& settings)
{
    const float half = threat.radius + settings.playerRadius + settings.clearanceTiles;
    for (int i = 0; i < threat.sampleCount; ++i) {
        if (ChebDistance(point, threat.samples[i]) <= half) return true;
        if (i + 1 < threat.sampleCount) {
            const Vec2 closest = ClosestPointOnSegment(point, threat.samples[i], threat.samples[i + 1]);
            if (ChebDistance(point, closest) <= half) return true;
        }
    }
    return false;
}

bool BlockerHitsPoint(const Blocker& blocker, Vec2 point, const Settings& settings)
{
    const float r = blocker.radius + settings.playerRadius + settings.clearanceTiles;
    return LenSq(Sub(point, blocker.pos)) <= r * r;
}

float ThreatClearance(Vec2 point, const Settings& settings, const SensorSnapshot& sensors)
{
    float best = 9999.f;
    for (int i = 0; i < sensors.threatCount; ++i) {
        const Threat& threat = sensors.threats[i];
        const float half = threat.radius + settings.playerRadius;
        for (int sample = 0; sample < threat.sampleCount; ++sample) {
            best = std::min(best, ChebDistance(point, threat.samples[sample]) - half);
        }
    }
    return best == 9999.f ? 10.f : best;
}

void AppendCandidateDebug(PlanResult& out, Vec2 pos, bool safe, CandidateRejectReason reason, float score)
{
    if (out.candidateCount >= kMaxCandidates) return;
    CandidateDebug& c = out.candidates[out.candidateCount++];
    c.pos = pos;
    c.safe = safe;
    c.rejectReason = reason;
    c.score = score;
}

} // namespace

bool IsPointSafe(const Vec2& point, const Settings& settings, const SensorSnapshot& sensors)
{
    for (int i = 0; i < sensors.blockerCount; ++i)
        if (BlockerHitsPoint(sensors.blockers[i], point, settings)) return false;
    for (int i = 0; i < sensors.threatCount; ++i)
        if (ThreatHitsPoint(sensors.threats[i], point, settings)) return false;
    return true;
}

bool IsSweepSafe(const Vec2& from, const Vec2& to, const Settings& settings, const SensorSnapshot& sensors)
{
    constexpr int kSteps = 4;
    for (int i = 1; i <= kSteps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kSteps);
        const Vec2 p = Add(from, Mul(Sub(to, from), t));
        if (!IsPointSafe(p, settings, sensors)) return false;
    }
    return true;
}

Vec2 ComputeSlideDirection(const Vec2& desiredDir, const Vec2& player, const Settings& settings, const SensorSnapshot& sensors)
{
    Vec2 adjusted = desiredDir;
    for (int i = 0; i < sensors.blockerCount; ++i) {
        const Blocker& blocker = sensors.blockers[i];
        const Vec2 away = Normalize(Sub(player, blocker.pos));
        const float inward = Dot(adjusted, Mul(away, -1.f));
        const float r = blocker.radius + settings.playerRadius + settings.clearanceTiles;
        if (LenSq(Sub(player, blocker.pos)) <= r * r * 1.44f && inward > 0.f) {
            adjusted = Add(adjusted, Mul(away, inward));
        }
    }
    return Normalize(adjusted);
}

PlanResult Evaluate(const PlanRequest& req)
{
    PlanResult out{};
    if (req.sensors.threatCount == 0 && req.sensors.blockerCount == 0) {
        out.status = FrameStatus::NoThreats;
        return out;
    }

    const bool hasIntent = LenSq(req.intentDir) > 0.0001f;
    const Vec2 intentDir = Normalize(req.intentDir);
    const Vec2 intended = Add(req.player, Mul(intentDir, hasIntent ? req.moveBudget : 0.f));
    if (IsSweepSafe(req.player, intended, req.settings, req.sensors)) {
        out.status = hasIntent ? FrameStatus::IntentSafe : FrameStatus::NoThreats;
        return out;
    }

    const Vec2 slideDir = ComputeSlideDirection(intentDir, req.player, req.settings, req.sensors);
    out.slideDir = slideDir;
    if (hasIntent && LenSq(slideDir) > 0.0001f) {
        const Vec2 slideTarget = Add(req.player, Mul(slideDir, req.moveBudget));
        if (IsSweepSafe(req.player, slideTarget, req.settings, req.sensors)) {
            out.status = FrameStatus::SlideAssist;
            out.target = slideTarget;
            out.shouldMove = true;
            AppendCandidateDebug(out, slideTarget, true, CandidateRejectReason::None, 1.f);
            return out;
        }
    }

    Vec2 best{};
    float bestScore = -999999.f;
    bool found = false;
    const float baseRadius = std::max(req.settings.playerRadius + req.settings.clearanceTiles, 0.15f);
    for (int ring = 0; ring < kRingPasses; ++ring) {
        const float radius = std::min(req.settings.maxMoveTiles, baseRadius * (1.f + 0.55f * static_cast<float>(ring)));
        for (int i = 0; i < kCandidateDirections; ++i) {
            const float angle = kTwoPi * static_cast<float>(i) / static_cast<float>(kCandidateDirections);
            const Vec2 dir{ std::cos(angle), std::sin(angle) };
            Vec2 candidate = Add(req.player, Mul(dir, std::min(radius, req.moveBudget)));
            CandidateRejectReason reason = CandidateRejectReason::None;
            bool safe = IsPointSafe(candidate, req.settings, req.sensors);
            if (!safe) reason = CandidateRejectReason::Projectile;
            if (safe && !IsSweepSafe(req.player, candidate, req.settings, req.sensors)) {
                safe = false;
                reason = CandidateRejectReason::Sweep;
            }
            float score = -Len(Sub(candidate, req.player));
            if (hasIntent) score += Dot(dir, intentDir) * 3.f;
            score += ThreatClearance(candidate, req.settings, req.sensors) * 0.75f;
            AppendCandidateDebug(out, candidate, safe, reason, score);
            if (safe && score > bestScore) {
                bestScore = score;
                best = candidate;
                found = true;
            }
        }
        if (found) break;
    }

    if (found) {
        out.status = FrameStatus::CandidateAssist;
        out.target = best;
        out.shouldMove = true;
        return out;
    }

    out.status = FrameStatus::NoSafeCandidate;
    return out;
}

} // namespace ZDodge::Planner
```

- [ ] **Step 4: Build planner files only through full project**

Run:

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" .\internal\il2cpp-dll-injection.sln /p:Configuration=Debug /p:Platform=x64 /m
```

Expected before project entries are added: the new files are not compiled yet. If the command succeeds, continue; if unrelated existing files fail, record the errors and continue only after confirming they are unrelated.

- [ ] **Step 5: Commit planner scaffold**

```powershell
git add internal/src/features/movement/zdodge/ZDodgeTypes.h internal/src/features/movement/zdodge/ZDodgePlanner.h internal/src/features/movement/zdodge/ZDodgePlanner.cpp
git commit -m "feat: scaffold zaclin dodge planner"
```

---

### Task 2: Add Sensor Snapshot Adapter

**Files:**
- Create: `internal/src/features/movement/zdodge/ZDodgeSensors.h`
- Create: `internal/src/features/movement/zdodge/ZDodgeSensors.cpp`

- [ ] **Step 1: Add sensor interface**

Create `ZDodgeSensors.h`:

```cpp
#pragma once

#include "ZDodgeTypes.h"

namespace ZDodge::Sensors {

SensorSnapshot Build(float playerX, float playerY, const Settings& settings);

} // namespace ZDodge::Sensors
```

- [ ] **Step 2: Implement projectile, enemy, and obstacle snapshotting**

Create `ZDodgeSensors.cpp`:

```cpp
#include "pch-il2cpp.h"
#include "ZDodgeSensors.h"

#include "AutoAim.h"
#include "ProjectileTracking.h"
#include "WorldTAB.h"
#include "gui/tabs/TestTAB.h"

#include <algorithm>
#include <cmath>
#include <vector>
#include <windows.h>

namespace ZDodge::Sensors {
namespace {

constexpr float kThreatCullTiles = 14.f;
constexpr float kEnemyRadius = 0.5f;
constexpr float kObstacleRadius = 0.5f;
constexpr int kObstacleGridRadius = 6;
constexpr float kObstacleStep = 1.f;

float DistSq(float ax, float ay, float bx, float by)
{
    const float dx = ax - bx;
    const float dy = ay - by;
    return dx * dx + dy * dy;
}

void AddBlocker(SensorSnapshot& out, Blocker::Kind kind, int32_t id, float x, float y, float radius)
{
    if (out.blockerCount >= kMaxBlockers) {
        out.blockerLimited = true;
        return;
    }
    Blocker& b = out.blockers[out.blockerCount++];
    b.kind = kind;
    b.id = id;
    b.pos = { x, y };
    b.radius = radius;
}

struct EnemyCtx {
    SensorSnapshot* out;
};

void OnEnemy(float x, float y, int32_t id, void* user)
{
    auto* ctx = static_cast<EnemyCtx*>(user);
    AddBlocker(*ctx->out, Blocker::Kind::Enemy, id, x, y, kEnemyRadius);
}

} // namespace

SensorSnapshot Build(float playerX, float playerY, const Settings& settings)
{
    SensorSnapshot out{};

    std::vector<WorldProjectile> projectiles;
    ProjectileTracking::CopyActiveForDraw(projectiles);
    const float cullSq = kThreatCullTiles * kThreatCullTiles;
    const uint64_t nowMs = GetTickCount64();
    for (const WorldProjectile& p : projectiles) {
        if (!p.valid) continue;
        if (out.threatCount >= kMaxThreats) {
            out.projectileLimited = true;
            break;
        }
        if (DistSq(p.x, p.y, playerX, playerY) > cullSq) continue;

        Threat& threat = out.threats[out.threatCount];
        threat.id = static_cast<int32_t>(out.threatCount + 1);
        threat.radius = settings.projectileRadiusFallback;
        float readRadius = 0.f;
        if (ProjectileTracking::TryReadProjRadiusFromInstance(p.ptr, readRadius) && readRadius > 0.f && readRadius < 5.f)
            threat.radius = readRadius;
        threat.damage = static_cast<float>(std::max(p.damage, 0));

        const float elapsed = static_cast<float>(nowMs > p.spawnTick ? nowMs - p.spawnTick : 0u);
        const float stepMs = std::max(16.f, settings.reactWindowMs / static_cast<float>(kMaxPathSamples - 1));
        for (int i = 0; i < kMaxPathSamples; ++i) {
            const float tMs = elapsed + stepMs * static_cast<float>(i);
            if (p.lifetime > 0.f && tMs > p.lifetime) break;
            float x = p.x;
            float y = p.y;
            if (i != 0) ProjectileTracking::ComputePosAtSafe(p, tMs, x, y);
            if (!std::isfinite(x) || !std::isfinite(y)) break;
            threat.samples[threat.sampleCount++] = { x, y };
            if (stepMs * static_cast<float>(i) >= settings.reactWindowMs) break;
        }
        if (threat.sampleCount > 0) ++out.threatCount;
    }

    EnemyCtx enemyCtx{ &out };
    AutoAim::EnumerateLiveEnemies(OnEnemy, &enemyCtx);

    for (int gy = -kObstacleGridRadius; gy <= kObstacleGridRadius; ++gy) {
        for (int gx = -kObstacleGridRadius; gx <= kObstacleGridRadius; ++gx) {
            if (gx == 0 && gy == 0) continue;
            const float x = playerX + static_cast<float>(gx) * kObstacleStep;
            const float y = playerY + static_cast<float>(gy) * kObstacleStep;
            if (TestTAB::IsWalkPositionBlocked(x, y))
                AddBlocker(out, Blocker::Kind::Obstacle, 100000 + gy * 100 + gx, x, y, kObstacleRadius);
        }
    }

    return out;
}

} // namespace ZDodge::Sensors
```

- [ ] **Step 3: Verify symbols used by sensors**

Run:

```powershell
rg "struct WorldProjectile|ptr|damage|spawnTick|lifetime|valid" internal/src -n
```

Expected: `WorldProjectile` exposes `ptr`, `x`, `y`, `damage`, `spawnTick`, `lifetime`, and `valid` in `internal/src/gui/tabs/WorldTAB.h`.

- [ ] **Step 4: Commit sensors**

```powershell
git add internal/src/features/movement/zdodge/ZDodgeSensors.h internal/src/features/movement/zdodge/ZDodgeSensors.cpp
git commit -m "feat: add zaclin dodge sensors"
```

---

### Task 3: Add Runtime API And Tick Loop

**Files:**
- Create: `internal/src/features/movement/zdodge/ZDodge.h`
- Create: `internal/src/features/movement/zdodge/ZDodge.cpp`

- [ ] **Step 1: Add public API**

Create `ZDodge.h`:

```cpp
#pragma once

namespace ZDodge {

void SetEnabled(bool enabled);
bool IsEnabled();
void OnEnter();
void Tick(void* player, float px, float py, float dt);
void RenderSettings();
void RenderDebugOverlay(float camX, float camY, float angle, float zoom, float cx, float cy);

void SetReactWindowMs(float ms);
float GetReactWindowMs();
void SetMaxMoveTiles(float tiles);
float GetMaxMoveTiles();
void SetPlayerRadius(float radius);
float GetPlayerRadius();
void SetProjectileRadiusFallback(float radius);
float GetProjectileRadiusFallback();
void SetDamageThresholdPct(float pct);
float GetDamageThresholdPct();
void SetDebugOverlay(bool enabled);
bool GetDebugOverlay();
void SetCandidateOverlay(bool enabled);
bool GetCandidateOverlay();

} // namespace ZDodge
```

- [ ] **Step 2: Implement runtime using sensors/planner/movement**

Create `ZDodge.cpp`:

```cpp
#include "pch-il2cpp.h"
#include "ZDodge.h"

#include "ZDodgeDebug.h"
#include "ZDodgePlanner.h"
#include "ZDodgeSensors.h"

#include "MovementRuntime.h"
#include "SteerInput.h"
#include "gui/tabs/TestTAB.h"

#include <imgui/imgui.h>
#include <algorithm>
#include <atomic>
#include <cmath>

namespace ZDodge {
namespace {

std::atomic<bool> g_enabled{ false };
std::atomic<float> g_reactWindowMs{ 650.f };
std::atomic<float> g_maxMoveTiles{ 1.25f };
std::atomic<float> g_playerRadius{ 0.25f };
std::atomic<float> g_projectileRadiusFallback{ 0.10f };
std::atomic<float> g_damageThresholdPct{ 0.f };
std::atomic<bool> g_debugOverlay{ false };
std::atomic<bool> g_candidateOverlay{ true };
DebugSnapshot g_debug{};

float Clamp(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
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

} // namespace

void SetEnabled(bool enabled) { g_enabled.store(enabled, std::memory_order_relaxed); }
bool IsEnabled() { return g_enabled.load(std::memory_order_relaxed); }
void OnEnter() { g_debug = DebugSnapshot{}; }

void Tick(void* player, float px, float py, float dt)
{
    if (!IsEnabled()) return;
    if (!player) { g_debug.status = FrameStatus::NoPlayer; return; }
    if (!DodgeRuntime::EnsureResolved()) { g_debug.status = FrameStatus::MovementFailed; return; }

    SteerInput::Tick();
    const SteerInput::SteerState steer = SteerInput::Get();
    const Settings settings = ReadSettings();
    int32_t hp = 0;
    int32_t maxHp = 0;
    float spd = 0.f;
    float tilesPerSec = 0.f;
    TestTAB::ReadDodgePlayerStats(hp, maxHp, spd, tilesPerSec);
    const float moveBudget = std::min(settings.maxMoveTiles, std::max(0.02f, tilesPerSec * dt));

    PlanRequest req{};
    req.settings = settings;
    req.player = { px, py };
    req.intentDir = steer.active ? Vec2{ steer.dirX, steer.dirY } : Vec2{};
    req.moveBudget = moveBudget;
    req.sensors = Sensors::Build(px, py, settings);

    const PlanResult plan = Planner::Evaluate(req);

    g_debug.status = plan.status;
    g_debug.player = req.player;
    g_debug.intentDir = req.intentDir;
    g_debug.intendedTarget = { px + req.intentDir.x * moveBudget, py + req.intentDir.y * moveBudget };
    g_debug.slideDir = plan.slideDir;
    g_debug.selectedTarget = plan.target;
    g_debug.hasSelectedTarget = plan.shouldMove;
    g_debug.sensors = req.sensors;
    g_debug.candidateCount = std::min(plan.candidateCount, kMaxCandidates);
    for (int i = 0; i < g_debug.candidateCount; ++i) g_debug.candidates[i] = plan.candidates[i];

    if (plan.shouldMove) {
        if (!DodgeRuntime::CallMoveTo(player, plan.target.x, plan.target.y))
            g_debug.status = FrameStatus::MovementFailed;
    }
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
    Debug::Render(g_debug, ReadSettings(), camX, camY, angle, zoom, cx, cy);
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

} // namespace ZDodge
```

- [ ] **Step 3: Commit runtime API**

```powershell
git add internal/src/features/movement/zdodge/ZDodge.h internal/src/features/movement/zdodge/ZDodge.cpp
git commit -m "feat: add zaclin dodge runtime"
```

---

### Task 4: Add Debug Overlay Renderer

**Files:**
- Create: `internal/src/features/movement/zdodge/ZDodgeDebug.h`
- Create: `internal/src/features/movement/zdodge/ZDodgeDebug.cpp`

- [ ] **Step 1: Add debug interface**

Create `ZDodgeDebug.h`:

```cpp
#pragma once

#include "ZDodgeTypes.h"

namespace ZDodge::Debug {

void Render(const DebugSnapshot& snapshot, const Settings& settings, float camX, float camY, float angle, float zoom, float cx, float cy);

} // namespace ZDodge::Debug
```

- [ ] **Step 2: Implement overlay drawing**

Create `ZDodgeDebug.cpp`:

```cpp
#include "pch-il2cpp.h"
#include "ZDodgeDebug.h"

#include "W2S.h"

#include <imgui/imgui.h>
#include <algorithm>

namespace ZDodge::Debug {
namespace {

bool ToScreen(Vec2 p, float camX, float camY, float angle, float zoom, float cx, float cy, ImVec2& out)
{
    float sx = 0.f, sy = 0.f;
    if (!W2S(p.x, p.y, sx, sy, camX, camY, angle, zoom, cx, cy)) return false;
    out = ImVec2(sx, sy);
    return true;
}

void DrawCircle(ImDrawList* draw, Vec2 p, float radiusPx, ImU32 color, float camX, float camY, float angle, float zoom, float cx, float cy)
{
    ImVec2 screen;
    if (ToScreen(p, camX, camY, angle, zoom, cx, cy, screen)) draw->AddCircle(screen, radiusPx, color, 16, 1.5f);
}

void DrawLine(ImDrawList* draw, Vec2 a, Vec2 b, ImU32 color, float camX, float camY, float angle, float zoom, float cx, float cy, float thickness = 1.5f)
{
    ImVec2 sa, sb;
    if (ToScreen(a, camX, camY, angle, zoom, cx, cy, sa) && ToScreen(b, camX, camY, angle, zoom, cx, cy, sb))
        draw->AddLine(sa, sb, color, thickness);
}

} // namespace

void Render(const DebugSnapshot& snapshot, const Settings& settings, float camX, float camY, float angle, float zoom, float cx, float cy)
{
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const ImU32 projectileColor = IM_COL32(255, 80, 80, 220);
    const ImU32 pathColor = IM_COL32(255, 180, 80, 160);
    const ImU32 enemyColor = IM_COL32(255, 60, 200, 210);
    const ImU32 obstacleColor = IM_COL32(90, 160, 255, 190);
    const ImU32 safeColor = IM_COL32(80, 255, 140, 190);
    const ImU32 rejectColor = IM_COL32(255, 80, 80, 120);
    const ImU32 intentColor = IM_COL32(255, 255, 255, 230);
    const ImU32 slideColor = IM_COL32(80, 255, 255, 230);
    const ImU32 targetColor = IM_COL32(80, 255, 80, 255);

    const SensorSnapshot& sensors = snapshot.sensors;
    for (int i = 0; i < sensors.threatCount; ++i) {
        const Threat& threat = sensors.threats[i];
        for (int s = 0; s < threat.sampleCount; ++s) {
            DrawCircle(draw, threat.samples[s], 3.f, projectileColor, camX, camY, angle, zoom, cx, cy);
            if (s + 1 < threat.sampleCount)
                DrawLine(draw, threat.samples[s], threat.samples[s + 1], pathColor, camX, camY, angle, zoom, cx, cy, 1.f);
        }
    }

    for (int i = 0; i < sensors.blockerCount; ++i) {
        const Blocker& blocker = sensors.blockers[i];
        DrawCircle(draw, blocker.pos, blocker.kind == Blocker::Kind::Enemy ? 8.f : 5.f,
            blocker.kind == Blocker::Kind::Enemy ? enemyColor : obstacleColor,
            camX, camY, angle, zoom, cx, cy);
    }

    if (settings.candidateOverlay) {
        for (int i = 0; i < snapshot.candidateCount; ++i) {
            const CandidateDebug& c = snapshot.candidates[i];
            DrawCircle(draw, c.pos, 2.5f, c.safe ? safeColor : rejectColor, camX, camY, angle, zoom, cx, cy);
        }
    }

    DrawLine(draw, snapshot.player, snapshot.intendedTarget, intentColor, camX, camY, angle, zoom, cx, cy, 2.f);
    DrawLine(draw, snapshot.player, { snapshot.player.x + snapshot.slideDir.x, snapshot.player.y + snapshot.slideDir.y }, slideColor, camX, camY, angle, zoom, cx, cy, 2.f);
    if (snapshot.hasSelectedTarget) DrawCircle(draw, snapshot.selectedTarget, 7.f, targetColor, camX, camY, angle, zoom, cx, cy);
}

} // namespace ZDodge::Debug
```

- [ ] **Step 3: Verify W2S signature**

Run:

```powershell
rg "inline bool W2S|W2S\(" internal/src/game internal/src -n
```

Expected: `internal/src/game/math/W2S.h` exposes the global inline `W2S(...)` helper used by `ToScreen`.

- [ ] **Step 4: Commit debug overlay**

```powershell
git add internal/src/features/movement/zdodge/ZDodgeDebug.h internal/src/features/movement/zdodge/ZDodgeDebug.cpp
git commit -m "feat: add zaclin dodge debug overlay"
```

---

### Task 5: Wire Mode Switching, Tick, Overlay, And IPC

**Files:**
- Modify: `internal/src/gui/tabs/TestTAB.h`
- Modify: `internal/src/gui/tabs/TestTAB.cpp`
- Modify: `internal/src/features/control/FeatureState.cpp`
- Modify: `internal/src/features/control/FeatureCommandRegistry.cpp`
- Modify: `internal/src/features/control/FeatureCommandRegistry.cpp`

- [ ] **Step 1: Add enum value**

In `TestTAB.h`, change the enum to:

```cpp
enum class DodgeMode : int {
    Off     = 0,
    XDodge  = 1,  // Spacetime BFS ported from XRebuild/XDriver. Movement via NativeMoveTo.
    Rollout = 2,  // Forward input-simulation + uniform-grid broad-phase (RolloutDodge).
    Zaclin  = 3,  // Intent-preserving slide-assist dodge (zdodge).
};
```

- [ ] **Step 2: Include ZDodge in TestTAB.cpp**

Near existing dodge includes, add:

```cpp
#include "ZDodge.h"
```

- [ ] **Step 3: Update ApplyDodgeModeWithEnter**

In `TestTAB.cpp`, update the mode body to include Zaclin and install the existing update hook:

```cpp
    XDodge::SetEnabled(nextMode == DodgeMode::XDodge);
    RolloutDodge::SetEnabled(nextMode == DodgeMode::Rollout);
    ZDodge::SetEnabled(nextMode == DodgeMode::Zaclin);
    if (nextMode == DodgeMode::XDodge) {
        XDodge::OnEnter();
        DangerPlanner::TryInstall();
    } else if (nextMode == DodgeMode::Rollout) {
        RolloutDodge::OnEnter();
        DangerPlanner::TryInstall();
    } else if (nextMode == DodgeMode::Zaclin) {
        ZDodge::OnEnter();
        DangerPlanner::TryInstall();
    }
```

- [ ] **Step 4: Update render mode list and settings**

In `RenderMovementSection`, change mode labels and Zaclin settings branch:

```cpp
    const char* modeLabels[] = { "Off", "RE-Plus", "RE-Sim", "Zaclin" };
```

and:

```cpp
    if (g_dodgeMode == DodgeMode::XDodge) {
        ImGui::Spacing();
        XDodge::RenderSettings();
    } else if (g_dodgeMode == DodgeMode::Rollout) {
        ImGui::Spacing();
        RolloutDodge::RenderSettings();
    } else if (g_dodgeMode == DodgeMode::Zaclin) {
        ImGui::Spacing();
        ZDodge::RenderSettings();
    }
```

- [ ] **Step 5: Update overlay call**

Where `XDodge::RenderDebugPath` and `RolloutDodge::RenderDebugPath` are called in `TestTAB::Tick`, add:

```cpp
            ZDodge::RenderDebugOverlay(camX, camY, angleRad, zoom, cx, cy);
```

- [ ] **Step 6: Update AppEngine tick dispatch**

In `DangerPlanner.cpp`, include `ZDodge.h` and update the dispatch near `RolloutDodge::Tick` / `XDodge::Tick`:

```cpp
        const bool rolloutOn = RolloutDodge::IsEnabled();
        const bool zaclinOn = ZDodge::IsEnabled();
        if (zaclinOn)      ZDodge::Tick(p, px, py, dt);
        else if (rolloutOn) RolloutDodge::Tick(p, px, py, dt);
        else               XDodge::Tick(p, px, py, dt);
```

- [ ] **Step 7: Update FeatureState clamp**

In `FeatureState.cpp`, change the auto dodge clamp to include Zaclin:

```cpp
void    SetAutoDodgeMode(int mode)    { s_featDodgeMode.store(ClampInt(mode, 0, static_cast<int>(TestTAB::DodgeMode::Zaclin)), std::memory_order_relaxed); }
```

- [ ] **Step 8: Add IPC feature handlers**

In `FeatureCommandRegistry.cpp`, include `ZDodge.h` and add a new function before `ApplyRolloutFeature`:

```cpp
    bool ApplyZaclinFeature(const FeatureCommand& f)
    {
        static const FeatureHandler h[] = {
            FH_FLOAT("zaclinReactWindowMs", ZDodge::SetReactWindowMs),
            FH_FLOAT("zaclinMaxMoveTiles", ZDodge::SetMaxMoveTiles),
            FH_FLOAT("zaclinPlayerRadius", ZDodge::SetPlayerRadius),
            FH_FLOAT("zaclinProjectileRadiusFallback", ZDodge::SetProjectileRadiusFallback),
            FH_FLOAT("zaclinDamageThresholdPct", ZDodge::SetDamageThresholdPct),
            FH_INT_BOOL("zaclinDebugOverlay", ZDodge::SetDebugOverlay),
            FH_INT_BOOL("zaclinCandidateOverlay", ZDodge::SetCandidateOverlay)
        };
        return ApplyFeatureTable(f, h, sizeof(h) / sizeof(h[0]));
    }
```

Then update `FeatureCommandRegistry::Apply`:

```cpp
        if (ApplyCoreFeature(feature)) return true;
        if (ApplyXDodgeFeature(feature)) return true;
        if (ApplyZaclinFeature(feature)) return true;
        if (ApplyRolloutFeature(feature)) return true;
```

- [ ] **Step 9: Commit C++ wiring**

```powershell
git add internal/src/gui/tabs/TestTAB.h internal/src/gui/tabs/TestTAB.cpp internal/src/features/movement/dodge/DangerPlanner.cpp internal/src/features/control/FeatureState.cpp internal/src/features/control/FeatureCommandRegistry.cpp
git commit -m "feat: wire zaclin dodge mode"
```

---

### Task 6: Add Client Plugin Settings

**Files:**
- Modify: `client/plugins/auto-dodge.ts`

- [ ] **Step 1: Add mode value**

Change the mode comments and values:

```ts
// Off=0, XDodge=1, Rollout=2, Zaclin=3.
const DODGE_VALUES = ['off', 'xdodge', 'rollout', 'zaclin'] as const;
```

Add the option:

```ts
      { label: 'Zaclin', value: 'zaclin' },
```

- [ ] **Step 2: Add Zaclin settings**

After the common hitbox settings, add:

```ts
  // ── Zaclin settings ───────────────────────────────────────────────────────
  ctx.registerSetting('zaclinReactWindowMs', {
    label: '[Zaclin] React window (ms)',
    type: 'range', value: 650, min: 100, max: 2500, step: 25,
  }, (v: number) => sendDllFeature('zaclinReactWindowMs', v));
  ctx.registerSetting('zaclinMaxMoveTiles', {
    label: '[Zaclin] Max assist distance (tiles)',
    type: 'range', value: 1.25, min: 0.2, max: 4.0, step: 0.05,
  }, (v: number) => sendDllFeature('zaclinMaxMoveTiles', v));
  ctx.registerSetting('zaclinPlayerRadius', {
    label: '[Zaclin] Player radius', advanced: true,
    type: 'range', value: 0.25, min: 0.05, max: 1.0, step: 0.01,
  }, (v: number) => sendDllFeature('zaclinPlayerRadius', v));
  ctx.registerSetting('zaclinProjectileRadiusFallback', {
    label: '[Zaclin] Projectile fallback radius', advanced: true,
    type: 'range', value: 0.10, min: 0.02, max: 1.0, step: 0.01,
  }, (v: number) => sendDllFeature('zaclinProjectileRadiusFallback', v));
  ctx.registerSetting('zaclinDamageThresholdPct', {
    label: '[Zaclin] Damage threshold pct', advanced: true,
    type: 'range', value: 0, min: 0, max: 1, step: 0.01,
  }, (v: number) => sendDllFeature('zaclinDamageThresholdPct', v));
  ctx.registerSetting('zaclinDebugOverlay', onOff('[Zaclin] Debug overlay', 'off'),
    (v: string) => sendDllFeature('zaclinDebugOverlay', v === 'on' ? 1 : 0));
  ctx.registerSetting('zaclinCandidateOverlay', onOff('[Zaclin] Candidate points', 'on'),
    (v: string) => sendDllFeature('zaclinCandidateOverlay', v === 'on' ? 1 : 0));
```

- [ ] **Step 3: Sync settings on enable/connect**

In `syncModeSettings`, add:

```ts
    sendDllFeature('zaclinReactWindowMs', ctx.getSetting<number>('zaclinReactWindowMs'));
    sendDllFeature('zaclinMaxMoveTiles', ctx.getSetting<number>('zaclinMaxMoveTiles'));
    sendDllFeature('zaclinPlayerRadius', ctx.getSetting<number>('zaclinPlayerRadius'));
    sendDllFeature('zaclinProjectileRadiusFallback', ctx.getSetting<number>('zaclinProjectileRadiusFallback'));
    sendDllFeature('zaclinDamageThresholdPct', ctx.getSetting<number>('zaclinDamageThresholdPct'));
    for (const k of ['zaclinDebugOverlay', 'zaclinCandidateOverlay'])
      sendDllFeature(k, ctx.getSetting<string>(k) === 'on' ? 1 : 0);
```

- [ ] **Step 4: Run TypeScript check**

Run:

```powershell
Set-Location client
pnpm exec tsc --noEmit
```

Expected: exit 0. If the repo uses a different script, run `pnpm run` and use the local typecheck/build script that includes plugins.

- [ ] **Step 5: Commit plugin settings**

```powershell
git add client/plugins/auto-dodge.ts
git commit -m "feat: add zaclin dodge plugin settings"
```

---

### Task 7: Add Project Entries And Include Path

**Files:**
- Modify: `internal/il2cpp-dll-injection.vcxproj`
- Modify: `internal/il2cpp-dll-injection.vcxproj.filters`

- [ ] **Step 1: Add ClCompile entries**

In `internal/il2cpp-dll-injection.vcxproj`, add these near the existing movement dodge source entries:

```xml
    <ClCompile Include="src\features\movement\zdodge\ZDodge.cpp" />
    <ClCompile Include="src\features\movement\zdodge\ZDodgePlanner.cpp" />
    <ClCompile Include="src\features\movement\zdodge\ZDodgeSensors.cpp" />
    <ClCompile Include="src\features\movement\zdodge\ZDodgeDebug.cpp" />
```

- [ ] **Step 2: Add ClInclude entries**

Add these near existing movement dodge headers:

```xml
    <ClInclude Include="src\features\movement\zdodge\ZDodge.h" />
    <ClInclude Include="src\features\movement\zdodge\ZDodgeTypes.h" />
    <ClInclude Include="src\features\movement\zdodge\ZDodgePlanner.h" />
    <ClInclude Include="src\features\movement\zdodge\ZDodgeSensors.h" />
    <ClInclude Include="src\features\movement\zdodge\ZDodgeDebug.h" />
```

- [ ] **Step 3: Add include directory**

In each `AdditionalIncludeDirectories` property, add:

```xml
$(ProjectDir)src\features\movement\zdodge;
```

beside the existing `$(ProjectDir)src\features\movement\dodge;` entry.

- [ ] **Step 4: Mirror entries in filters**

In `internal/il2cpp-dll-injection.vcxproj.filters`, add matching `ClCompile` and `ClInclude` entries. Use the same filter style as nearby movement files; if no nested filter exists, omit a `<Filter>` child just like existing flat entries.

- [ ] **Step 5: Build Debug x64**

Run:

```powershell
Set-Location A:\Projects\realm-engine-client
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" .\internal\il2cpp-dll-injection.sln /p:Configuration=Debug /p:Platform=x64 /m
```

Expected: `Build succeeded. 0 Warning(s) 0 Error(s)`. Fix compile errors in the task that introduced them before committing.

- [ ] **Step 6: Commit project integration**

```powershell
git add internal/il2cpp-dll-injection.vcxproj internal/il2cpp-dll-injection.vcxproj.filters
git commit -m "build: include zaclin dodge sources"
```

---

### Task 8: Final Verification And In-Game Checklist

**Files:**
- Verify: all files touched by Tasks 1-7

- [ ] **Step 1: Run whitespace check**

```powershell
git diff --check HEAD~7..HEAD
```

Expected: no output.

- [ ] **Step 2: Run full DLL build**

```powershell
Set-Location A:\Projects\realm-engine-client
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" .\internal\il2cpp-dll-injection.sln /p:Configuration=Debug /p:Platform=x64 /m
```

Expected: `Build succeeded. 0 Warning(s) 0 Error(s)`.

- [ ] **Step 3: Run client typecheck/build**

```powershell
Set-Location A:\Projects\realm-engine-client\client
pnpm exec tsc --noEmit
```

Expected: exit 0.

- [ ] **Step 4: Manual in-game validation**

Use the Auto Dodge plugin and Movement tab:

- Select `Zaclin` mode and confirm XDodge/Rollout are disabled.
- Enable `[Zaclin] Debug overlay` and confirm projectiles, projectile paths, enemies, obstacles, candidates, intent vector, slide vector, and selected target draw.
- With no threats, confirm no movement is issued.
- Hold WASD on a safe path and confirm no assist movement fights the input.
- Hold WASD into a projectile path and confirm the movement slides around the danger instead of taking complete control.
- Stand idle in a projectile path and confirm it picks a nearby safe candidate.
- Confirm it does not select candidates inside visible enemies or blocked map positions.

- [ ] **Step 5: Commit verification notes if docs changed**

If implementation adds a validation note, save it under `learnings/` and commit it:

```powershell
git add learnings/<new-validation-note>.md
git commit -m "docs: record zaclin dodge validation notes"
```

If no validation note is added, skip this commit.
