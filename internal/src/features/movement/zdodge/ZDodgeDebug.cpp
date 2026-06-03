#include "pch-il2cpp.h"
#include "ZDodgeDebug.h"

#include "W2S.h"

#include <imgui/imgui.h>
#include <algorithm>
#include <cmath>

namespace ZDodge::Debug {
namespace {

constexpr float kDangerNearTiles = 2.f;
constexpr float kDangerMidTiles = 6.f;
constexpr float kDangerFarTiles = 10.f;

bool ToScreen(Vec2 p, float camX, float camY, float angle, float zoom, float cx, float cy, ImVec2& out)
{
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(camX) || !std::isfinite(camY) ||
        !std::isfinite(angle) || !std::isfinite(zoom) || !std::isfinite(cx) || !std::isfinite(cy) || zoom <= 0.f)
        return false;
    float sx = 0.f, sy = 0.f;
    if (!W2S(p.x, p.y, sx, sy, camX, camY, angle, zoom, cx, cy)) return false;
    if (!std::isfinite(sx) || !std::isfinite(sy)) return false;
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

void DrawRect(ImDrawList* draw, Vec2 p, float halfSizePx, ImU32 color, float camX, float camY, float angle, float zoom, float cx, float cy)
{
    ImVec2 screen;
    if (!ToScreen(p, camX, camY, angle, zoom, cx, cy, screen)) return;
    draw->AddRect(
        ImVec2(screen.x - halfSizePx, screen.y - halfSizePx),
        ImVec2(screen.x + halfSizePx, screen.y + halfSizePx),
        color,
        0.f,
        0,
        1.f);
}

float Dist(Vec2 a, Vec2 b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

ImU32 PathColorByPlayerDistance(Vec2 a, Vec2 b, Vec2 player)
{
    const Vec2 mid{ (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f };
    const float dist = Dist(mid, player);

    int r = 0;
    int g = 255;
    if (dist <= kDangerNearTiles) {
        r = 255;
        g = 0;
    } else if (dist <= kDangerMidTiles) {
        const float t = (dist - kDangerNearTiles) / (kDangerMidTiles - kDangerNearTiles);
        r = 255;
        g = static_cast<int>(std::clamp(t, 0.f, 1.f) * 255.f);
    } else if (dist <= kDangerFarTiles) {
        const float t = (dist - kDangerMidTiles) / (kDangerFarTiles - kDangerMidTiles);
        r = static_cast<int>((1.f - std::clamp(t, 0.f, 1.f)) * 255.f);
        g = 255;
    }
    return IM_COL32(r, g, 0, 255);
}

int ClosestSampleIndex(const Vec2* samples, int sampleCount, Vec2 current)
{
    int bestIdx = 0;
    float bestDistSq = 3.402823466e+38f;
    for (int i = 0; i < sampleCount; ++i) {
        const float dx = samples[i].x - current.x;
        const float dy = samples[i].y - current.y;
        const float distSq = dx * dx + dy * dy;
        if (distSq < bestDistSq) {
            bestDistSq = distSq;
            bestIdx = i;
        }
    }
    return bestIdx;
}

void DrawThreatOverlay(ImDrawList* draw, const Threat& threat, Vec2 player,
    float camX, float camY, float angle, float zoom, float cx, float cy)
{
    const int sampleCount = std::clamp(threat.sampleCount, 0, kMaxPathSamples);
    if (sampleCount < 1) return;

    const ImU32 projectileColor = IM_COL32(255, 255, 0, 255);
    const Vec2 current = threat.samples[0];
    DrawRect(draw, current, 5.f, projectileColor, camX, camY, angle, zoom, cx, cy);

    if (sampleCount < 2) return;
    const int startIdx = ClosestSampleIndex(threat.samples, sampleCount, current);
    for (int s = startIdx; s + 1 < sampleCount; ++s) {
        const ImU32 pathColor = PathColorByPlayerDistance(threat.samples[s], threat.samples[s + 1], player);
        DrawLine(draw, threat.samples[s], threat.samples[s + 1], pathColor, camX, camY, angle, zoom, cx, cy, 1.f);
    }
}

const char* StatusName(FrameStatus status)
{
    switch (status) {
    case FrameStatus::Disabled: return "Disabled";
    case FrameStatus::NoPlayer: return "No player";
    case FrameStatus::NoThreats: return "No threats";
    case FrameStatus::IntentSafe: return "Intent safe";
    case FrameStatus::SlideAssist: return "Slide assist";
    case FrameStatus::CandidateAssist: return "Candidate assist";
    case FrameStatus::NoSafeCandidate: return "No safe candidate";
    case FrameStatus::MovementFailed: return "Movement failed";
    case FrameStatus::SensorLimited: return "Sensor limited";
    default: return "Unknown";
    }
}

} // namespace

void Render(const DebugSnapshot& snapshot, const Settings& settings, float camX, float camY, float angle, float zoom, float cx, float cy)
{
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    if (!draw) return;
    const ImU32 enemyColor = IM_COL32(255, 60, 200, 210);
    const ImU32 obstacleColor = IM_COL32(90, 160, 255, 190);
    const ImU32 safeColor = IM_COL32(80, 255, 140, 190);
    const ImU32 rejectColor = IM_COL32(255, 80, 80, 120);
    const ImU32 intentColor = IM_COL32(255, 255, 255, 230);
    const ImU32 slideColor = IM_COL32(80, 255, 255, 230);
    const ImU32 targetColor = IM_COL32(80, 255, 80, 255);
    const ImU32 textColor = IM_COL32(245, 245, 245, 230);

    const SensorSnapshot& sensors = snapshot.sensors;
    const int threatCount = std::clamp(sensors.threatCount, 0, kMaxThreats);
    for (int i = 0; i < threatCount; ++i) {
        DrawThreatOverlay(draw, sensors.threats[i], snapshot.player, camX, camY, angle, zoom, cx, cy);
    }

    const int blockerCount = std::clamp(sensors.blockerCount, 0, kMaxBlockers);
    for (int i = 0; i < blockerCount; ++i) {
        const Blocker& blocker = sensors.blockers[i];
        DrawCircle(draw, blocker.pos, blocker.kind == Blocker::Kind::Enemy ? 8.f : 5.f,
            blocker.kind == Blocker::Kind::Enemy ? enemyColor : obstacleColor,
            camX, camY, angle, zoom, cx, cy);
    }

    if (settings.candidateOverlay) {
        const int candidateCount = std::clamp(snapshot.candidateCount, 0, kMaxCandidates);
        for (int i = 0; i < candidateCount; ++i) {
            const CandidateDebug& c = snapshot.candidates[i];
            DrawCircle(draw, c.pos, 2.5f, c.safe ? safeColor : rejectColor, camX, camY, angle, zoom, cx, cy);
        }
    }

    if (snapshot.status != FrameStatus::Disabled && snapshot.status != FrameStatus::NoPlayer) {
        DrawLine(draw, snapshot.player, snapshot.intendedTarget, intentColor, camX, camY, angle, zoom, cx, cy, 2.f);
        DrawLine(draw, snapshot.player, { snapshot.player.x + snapshot.slideDir.x, snapshot.player.y + snapshot.slideDir.y }, slideColor, camX, camY, angle, zoom, cx, cy, 2.f);
    }
    if (snapshot.hasSelectedTarget) DrawCircle(draw, snapshot.selectedTarget, 7.f, targetColor, camX, camY, angle, zoom, cx, cy);

    ImVec2 playerScreen;
    if (ToScreen(snapshot.player, camX, camY, angle, zoom, cx, cy, playerScreen))
        draw->AddText(ImVec2(playerScreen.x + 10.f, playerScreen.y - 18.f), textColor, StatusName(snapshot.status));
}

} // namespace ZDodge::Debug