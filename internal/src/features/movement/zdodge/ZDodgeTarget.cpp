#include "pch-il2cpp.h"
#include "ZDodgeTarget.h"

#include "gui/tabs/WorldTAB.h"
#include "W2S.h"

#include <imgui/imgui.h>
#include <atomic>
#include <cmath>

namespace ZDodge::Target {
namespace {

std::atomic<int32_t> g_targetId{ 0 };   // 0 = no target

// True if the entity is a targetable enemy (not the local player).
// Uses the class name set by WorldTAB: PMMFLLAIPGN covers all character mobs;
// OCOND_IS_ENEMY catches ObjectProperties-flagged enemies (bosses, breakable walls, etc.).
static bool IsTargetable(const WorldEntity& e)
{
    if (e.isLocal) return false;
    if (strcmp(e.typeName, "FKALGHJIADI") == 0) return false;
    return strcmp(e.typeName, "PMMFLLAIPGN") == 0 || (e.objConds & OCOND_IS_ENEMY);
}

// Draws a diamond (rotated square) outline + fill at screen position (sx, sy).
// `r` = half-diagonal in pixels.  alpha = 0–255.
static void DrawDiamond(ImDrawList* draw, float sx, float sy, float r, ImU32 fillCol, ImU32 rimCol)
{
    const ImVec2 top   { sx,     sy - r };
    const ImVec2 right { sx + r, sy     };
    const ImVec2 bot   { sx,     sy + r };
    const ImVec2 left  { sx - r, sy     };
    draw->AddQuadFilled(top, right, bot, left, fillCol);
    draw->AddQuad      (top, right, bot, left, rimCol, 2.f);
}

} // namespace

void ProcessClick(float sx, float sy,
                  float camX, float camY, float angle, float zoom,
                  float cx,   float cy)
{
    const auto& entities = WorldTAB::GetEntities();
    int32_t bestId    = 0;
    float   bestDistSq = kPickThresholdPx * kPickThresholdPx;

    for (const WorldEntity& e : entities) {
        if (!IsTargetable(e)) continue;

        float esx = 0.f, esy = 0.f;
        if (!W2S(e.x, e.y, esx, esy, camX, camY, angle, zoom, cx, cy)) continue;

        const float dx = esx - sx;
        const float dy = esy - sy;
        const float distSq = dx * dx + dy * dy;
        if (distSq < bestDistSq) {
            bestDistSq = distSq;
            bestId     = e.objectId;
        }
    }

    // Toggle: clicking the same enemy again clears the target.
    const int32_t current = g_targetId.load(std::memory_order_relaxed);
    g_targetId.store((bestId != 0 && bestId == current) ? 0 : bestId,
                     std::memory_order_relaxed);
}

void    SetTarget(int32_t id) { g_targetId.store(id, std::memory_order_relaxed); }
void    Clear()               { g_targetId.store(0,  std::memory_order_relaxed); }
bool    HasTarget()           { return g_targetId.load(std::memory_order_relaxed) != 0; }
int32_t GetId()               { return g_targetId.load(std::memory_order_relaxed); }

void Render(ImDrawList* draw,
            float camX, float camY, float angle, float zoom,
            float cx,   float cy)
{
    if (!draw) return;
    const int32_t id = g_targetId.load(std::memory_order_relaxed);
    if (id == 0) return;

    // Try a live position read first; fall back to the cached entity position.
    float wx = 0.f, wy = 0.f;
    if (!WorldTAB::GetEntityLivePos(id, wx, wy)) {
        // Entity may have despawned — check cached list.
        bool found = false;
        for (const WorldEntity& e : WorldTAB::GetEntities()) {
            if (e.objectId == id) { wx = e.x; wy = e.y; found = true; break; }
        }
        if (!found) { Clear(); return; }
    }

    float sx = 0.f, sy = 0.f;
    if (!W2S(wx, wy, sx, sy, camX, camY, angle, zoom, cx, cy)) return;

    // Pulse: inner diamond oscillates in size; outer rim is fixed.
    const float t     = static_cast<float>(GetTickCount64() % 900) / 900.f;
    const float pulse = 0.5f + 0.5f * sinf(t * 6.2831853f);   // 0 → 1 over ~0.9 s
    const float innerR = 11.f + pulse * 4.f;
    constexpr float kOuterR = 18.f;

    // Outer fixed rim — high contrast white/black pair for all backgrounds.
    DrawDiamond(draw, sx, sy, kOuterR,
                IM_COL32(0, 0, 0, 0),           // transparent fill on outer
                IM_COL32(0, 0, 0, 210));         // black shadow rim

    // Inner pulsing diamond — yellow-orange fill, white rim.
    DrawDiamond(draw, sx, sy, innerR,
                IM_COL32(255, 200, 0, 130),      // warm fill
                IM_COL32(255, 255, 255, 230));   // white rim

    // Centre dot for precise anchor.
    draw->AddCircleFilled(ImVec2(sx, sy), 3.f, IM_COL32(255, 255, 255, 230));
}

} // namespace ZDodge::Target
