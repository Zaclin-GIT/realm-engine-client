#include "pch-il2cpp.h"
#include "ZaclinDodgeSensors.h"

#include "AutoAim.h"
#include "ProjectileTracking.h"
#include "gui/tabs/WorldTAB.h"
#include "gui/tabs/TestTAB.h"

#include <algorithm>
#include <cmath>
#include <vector>
#include <windows.h>

namespace ZaclinDodge::Sensors {
namespace {

constexpr float kThreatCullTiles = 14.f;
constexpr float kEnemyRadius = 0.5f;
constexpr float kObstacleRadius = 0.5f;
constexpr int kObstacleGridRadius = 6;
constexpr float kObstacleStep = 1.f;

bool IsFinite(float v)
{
    return std::isfinite(v);
}

bool IsFinitePoint(float x, float y)
{
    return IsFinite(x) && IsFinite(y);
}

float SafeRadius(float value, float fallback)
{
    if (!IsFinite(value) || value <= 0.f) return fallback;
    return std::clamp(value, 0.02f, 5.f);
}

float SafeReactWindowMs(float value)
{
    if (!IsFinite(value) || value <= 0.f) return Settings{}.reactWindowMs;
    return std::clamp(value, 100.f, 2500.f);
}

float DistSq(float ax, float ay, float bx, float by)
{
    const float dx = ax - bx;
    const float dy = ay - by;
    return dx * dx + dy * dy;
}

void AddBlocker(SensorSnapshot& out, Blocker::Kind kind, int32_t id, float x, float y, float radius)
{
    if (!IsFinitePoint(x, y)) return;
    if (out.blockerCount >= kMaxBlockers) {
        out.blockerLimited = true;
        return;
    }
    Blocker& b = out.blockers[out.blockerCount++];
    b.kind = kind;
    b.id = id;
    b.pos = { x, y };
    b.radius = SafeRadius(radius, kObstacleRadius);
}

struct EnemyCtx {
    SensorSnapshot* out;
    float playerX;
    float playerY;
    float cullSq;
};

void OnEnemy(float x, float y, int32_t id, void* user)
{
    auto* ctx = static_cast<EnemyCtx*>(user);
    if (!ctx || !ctx->out || !IsFinitePoint(x, y)) return;
    if (DistSq(x, y, ctx->playerX, ctx->playerY) > ctx->cullSq) return;
    AddBlocker(*ctx->out, Blocker::Kind::Enemy, id, x, y, kEnemyRadius);
}

bool TryPredictProjectile(const WorldProjectile& projectile, float tMs, float& outX, float& outY)
{
    outX = 0.f;
    outY = 0.f;
    __try {
        ProjectileTracking::ComputePosAt(projectile, tMs, outX, outY);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return IsFinitePoint(outX, outY);
}

} // namespace

SensorSnapshot Build(float playerX, float playerY, const Settings& settings)
{
    SensorSnapshot out{};

    std::vector<WorldProjectile> projectiles;
    ProjectileTracking::CopyActiveForDraw(projectiles);
    const float cullSq = kThreatCullTiles * kThreatCullTiles;
    const float fallbackRadius = SafeRadius(settings.projectileRadiusFallback, Settings{}.projectileRadiusFallback);
    const float reactWindowMs = SafeReactWindowMs(settings.reactWindowMs);
    const uint64_t nowMs = GetTickCount64();
    for (const WorldProjectile& p : projectiles) {
        if (!p.valid) continue;
        if (!IsFinitePoint(p.x, p.y)) continue;
        if (DistSq(p.x, p.y, playerX, playerY) > cullSq) continue;
        if (out.threatCount >= kMaxThreats) {
            out.projectileLimited = true;
            break;
        }

        Threat& threat = out.threats[out.threatCount];
        threat.id = static_cast<int32_t>(out.threatCount + 1);
        threat.radius = fallbackRadius;
        float readRadius = 0.f;
        if (ProjectileTracking::TryReadProjRadiusFromInstance(p.ptr, readRadius) && readRadius > 0.f && readRadius < 5.f)
            threat.radius = SafeRadius(readRadius, fallbackRadius);
        threat.damage = static_cast<float>(std::max(p.damage, 0));

        const float elapsed = static_cast<float>(nowMs > p.spawnTick ? nowMs - p.spawnTick : 0u);
        const float stepMs = std::max(16.f, reactWindowMs / static_cast<float>(kMaxPathSamples - 1));
        for (int i = 0; i < kMaxPathSamples; ++i) {
            const float tMs = elapsed + stepMs * static_cast<float>(i);
            if (p.lifetime > 0.f && tMs > p.lifetime) break;
            float x = p.x;
            float y = p.y;
            if (i != 0 && !TryPredictProjectile(p, tMs, x, y)) break;
            if (!IsFinitePoint(x, y)) break;
            threat.samples[threat.sampleCount++] = { x, y };
            if (stepMs * static_cast<float>(i) >= reactWindowMs) break;
        }
        if (threat.sampleCount > 0) ++out.threatCount;
    }

    EnemyCtx enemyCtx{ &out, playerX, playerY, cullSq };
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

} // namespace ZaclinDodge::Sensors
