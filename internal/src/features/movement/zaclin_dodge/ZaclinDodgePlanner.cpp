#include "pch-il2cpp.h"
#include "ZaclinDodgePlanner.h"

#include <algorithm>
#include <cmath>

namespace ZaclinDodge::Planner {
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

float SanitizeNonNegative(float value, float fallback = 0.f)
{
    return std::isfinite(value) && value >= 0.f ? value : fallback;
}

int ThreatCount(const SensorSnapshot& sensors)
{
    return std::clamp(sensors.threatCount, 0, kMaxThreats);
}

int BlockerCount(const SensorSnapshot& sensors)
{
    return std::clamp(sensors.blockerCount, 0, kMaxBlockers);
}

int SampleCount(const Threat& threat)
{
    return std::clamp(threat.sampleCount, 0, kMaxPathSamples);
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
    const int n = SampleCount(threat);
    for (int i = 0; i < n; ++i) {
        if (ChebDistance(point, threat.samples[i]) <= half) return true;
        if (i + 1 < n) {
            const Vec2 closest = ClosestPointOnSegment(point, threat.samples[i], threat.samples[i + 1]);
            if (ChebDistance(point, closest) <= half) return true;
        }
    }
    return false;
}

bool BlockerHitsPoint(const Blocker& blocker, Vec2 point, const Settings& settings)
{
    const float r = blocker.radius + settings.playerRadius + settings.clearanceTiles;
    return ChebDistance(point, blocker.pos) <= r;
}

float ThreatClearance(Vec2 point, const Settings& settings, const SensorSnapshot& sensors)
{
    float best = 9999.f;
    for (int i = 0; i < ThreatCount(sensors); ++i) {
        const Threat& threat = sensors.threats[i];
        const float half = threat.radius + settings.playerRadius + settings.clearanceTiles;
        const int n = SampleCount(threat);
        for (int sample = 0; sample < n; ++sample) {
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
    for (int i = 0; i < BlockerCount(sensors); ++i)
        if (BlockerHitsPoint(sensors.blockers[i], point, settings)) return false;
    for (int i = 0; i < ThreatCount(sensors); ++i)
        if (ThreatHitsPoint(sensors.threats[i], point, settings)) return false;
    return true;
}

CandidateRejectReason RejectReasonForPoint(const Vec2& point, const Settings& settings, const SensorSnapshot& sensors)
{
    for (int i = 0; i < BlockerCount(sensors); ++i)
        if (BlockerHitsPoint(sensors.blockers[i], point, settings)) return CandidateRejectReason::Blocker;
    for (int i = 0; i < ThreatCount(sensors); ++i)
        if (ThreatHitsPoint(sensors.threats[i], point, settings)) return CandidateRejectReason::Projectile;
    return CandidateRejectReason::None;
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
    for (int i = 0; i < BlockerCount(sensors); ++i) {
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
    const int threatCount = ThreatCount(req.sensors);
    const int blockerCount = BlockerCount(req.sensors);
    const float maxMoveTiles = SanitizeNonNegative(req.settings.maxMoveTiles, Settings{}.maxMoveTiles);
    const float moveBudget = std::clamp(SanitizeNonNegative(req.moveBudget), 0.f, maxMoveTiles);
    if (threatCount == 0 && blockerCount == 0) {
        out.status = FrameStatus::NoThreats;
        return out;
    }

    const bool hasIntent = LenSq(req.intentDir) > 0.0001f;
    const Vec2 intentDir = Normalize(req.intentDir);
    const Vec2 intended = Add(req.player, Mul(intentDir, hasIntent ? moveBudget : 0.f));
    if (IsSweepSafe(req.player, intended, req.settings, req.sensors)) {
        out.status = hasIntent ? FrameStatus::IntentSafe : FrameStatus::NoThreats;
        return out;
    }

    const Vec2 slideDir = ComputeSlideDirection(intentDir, req.player, req.settings, req.sensors);
    out.slideDir = slideDir;
    if (hasIntent && LenSq(slideDir) > 0.0001f) {
        const Vec2 slideTarget = Add(req.player, Mul(slideDir, moveBudget));
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
        const float radius = std::min(maxMoveTiles, baseRadius * (1.f + 0.55f * static_cast<float>(ring)));
        for (int i = 0; i < kCandidateDirections; ++i) {
            const float angle = kTwoPi * static_cast<float>(i) / static_cast<float>(kCandidateDirections);
            const Vec2 dir{ std::cos(angle), std::sin(angle) };
            Vec2 candidate = Add(req.player, Mul(dir, std::min(radius, moveBudget)));
            CandidateRejectReason reason = RejectReasonForPoint(candidate, req.settings, req.sensors);
            bool safe = reason == CandidateRejectReason::None;
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

} // namespace ZaclinDodge::Planner
