#include "pch-il2cpp.h"
#include "ZDodgeSensors.h"

#include "AoeTracking.h"
#include "AutoAim.h"
#include "ProjectileTracking.h"
#include "gui/tabs/WorldTAB.h"
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

float ProjectileRadius(const WorldProjectile& projectile, float fallback)
{
    if (IsFinite(projectile.projHalfSize) && projectile.projHalfSize > 0.005f && projectile.projHalfSize < 2.f)
        return SafeRadius(projectile.projHalfSize, fallback);
    return fallback;
}

float DistSq(float ax, float ay, float bx, float by)
{
    const float dx = ax - bx;
    const float dy = ay - by;
    return dx * dx + dy * dy;
}

void AddThreatSample(Threat& threat, float x, float y, float tMs)
{
    if (threat.sampleCount >= kMaxPathSamples || !IsFinitePoint(x, y)) return;
    threat.sampleTimesMs[threat.sampleCount] = std::max(0.f, tMs);
    threat.samples[threat.sampleCount++] = { x, y };
    if (!threat.hasBounds) {
        threat.boundsMin = { x, y };
        threat.boundsMax = { x, y };
        threat.hasBounds = true;
    } else {
        threat.boundsMin.x = std::min(threat.boundsMin.x, x);
        threat.boundsMin.y = std::min(threat.boundsMin.y, y);
        threat.boundsMax.x = std::max(threat.boundsMax.x, x);
        threat.boundsMax.y = std::max(threat.boundsMax.y, y);
    }
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
    std::vector<int32_t>* enemyIds;
    float playerX;
    float playerY;
    float cullSq;
};

void OnEnemy(float x, float y, int32_t id, void* user)
{
    auto* ctx = static_cast<EnemyCtx*>(user);
    if (!ctx || !ctx->out || !IsFinitePoint(x, y)) return;
    if (ctx->enemyIds && id != 0) ctx->enemyIds->push_back(id);
    if (DistSq(x, y, ctx->playerX, ctx->playerY) > ctx->cullSq) return;
    AddBlocker(*ctx->out, Blocker::Kind::Enemy, id, x, y, kEnemyRadius);
}

bool IsEnemyOwnedProjectile(const WorldProjectile& projectile, const std::vector<int32_t>& enemyIds)
{
    for (const int32_t enemyId : enemyIds) {
        if (projectile.attackerObjId == enemyId) return true;
        if (static_cast<int32_t>(projectile.ownerObjId) == enemyId) return true;
    }
    return false;
}

bool TryPredictProjectile(const WorldProjectile& projectile, float tMs, float& outX, float& outY)
{
    float x = 0.f, y = 0.f;
    __try {
        ProjectileTracking::ComputePosAt(projectile, tMs, x, y);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    // (0, 0) is the failure sentinel left by GetPositionAtTime when the positionAt
    // call fails (null fn ptr, bad ptr, etc.). No real in-dungeon projectile is ever
    // at the world origin, so reject it explicitly — otherwise the caller adds
    // world-origin samples that W2S maps to the upper-left of the screen.
    if (x == 0.f && y == 0.f) return false;
    if (!IsFinitePoint(x, y)) return false;
    outX = x;
    outY = y;
    return true;
}

bool TryAddCachedProjectilePath(Threat& threat, const WorldProjectile& projectile, float reactWindowMs)
{
    const uint64_t nowMs = GetTickCount64();
    const float elapsedMs = static_cast<float>(nowMs > projectile.spawnTick ? nowMs - projectile.spawnTick : 0u);
    return AddProjectilePathThreat(threat, projectile, reactWindowMs, elapsedMs);
}

} // namespace

SensorSnapshot Build(float playerX, float playerY, const Settings& settings)
{
    SensorSnapshot out{};

    if (!ProjectileTracking::IsInstalled()) {
        out.projectileSourceUnavailable = true;
        return out;
    }

    const float cullSq = kThreatCullTiles * kThreatCullTiles;
    const float fallbackRadius = SafeRadius(settings.projectileRadiusFallback, Settings{}.projectileRadiusFallback);
    const float reactWindowMs = SafeReactWindowMs(settings.reactWindowMs);
    const uint64_t nowMs = GetTickCount64();
    const int32_t localId = ProjectileTracking::GetLocalPlayerObjectId();

    std::vector<int32_t> enemyIds;
    enemyIds.reserve(kMaxBlockers);
    EnemyCtx enemyCtx{ &out, &enemyIds, playerX, playerY, cullSq };
    AutoAim::EnumerateLiveEnemies(OnEnemy, &enemyCtx);

    std::vector<WorldProjectile> projectiles;
    ProjectileTracking::CopyActiveForDraw(projectiles);
    for (const WorldProjectile& p : projectiles) {
        if (!p.valid) continue;
        if (localId != 0 && p.attackerObjId == localId) continue;
        if (localId != 0 && static_cast<int32_t>(p.ownerObjId) == localId) continue;
        if (!p.canHitPlayer && !IsEnemyOwnedProjectile(p, enemyIds)) continue;
        if (!IsFinitePoint(p.x, p.y)) continue;
        if (DistSq(p.x, p.y, playerX, playerY) > cullSq) continue;
        if (out.threatCount >= kMaxThreats) {
            out.projectileLimited = true;
            break;
        }

        Threat& threat = out.threats[out.threatCount];
        threat.id = static_cast<int32_t>(out.threatCount + 1);
        threat.radius = ProjectileRadius(p, fallbackRadius);
        threat.damage = static_cast<float>(std::max(p.damage, 0));

        if (!TryAddCachedProjectilePath(threat, p, reactWindowMs)) {
            const float elapsed = static_cast<float>(nowMs > p.spawnTick ? nowMs - p.spawnTick : 0u);
            const float stepMs = std::max(16.f, reactWindowMs / static_cast<float>(kMaxPathSamples - 1));
            for (int i = 0; i < kMaxPathSamples; ++i) {
                const float futureMs = stepMs * static_cast<float>(i);
                const float tMs = elapsed + futureMs;
                if (p.lifetime > 0.f && tMs > p.lifetime) break;
                float x = p.x;
                float y = p.y;
                if (i != 0 && !TryPredictProjectile(p, tMs, x, y)) break;
                if (!IsFinitePoint(x, y)) break;
                AddThreatSample(threat, x, y, futureMs);
                if (stepMs * static_cast<float>(i) >= reactWindowMs) break;
            }
        }
        if (threat.sampleCount > 0) ++out.threatCount;
    }

    AoeTracking::EnsureInstalled();
    std::vector<WorldAoe> aoes;
    AoeTracking::CopyActiveForDraw(aoes);
    AddAoeThreats(out, aoes.data(), static_cast<int>(aoes.size()), playerX, playerY, settings, nowMs);

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
