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

} // namespace ZaclinDodge::Sensors
