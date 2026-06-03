#include "pch-il2cpp.h"
#include "ZDodgePlanner.h"

#include <algorithm>
#include <cmath>

namespace ZDodge::Planner {
namespace {

constexpr float kDefaultFrameMs = 16.667f;
constexpr float kTimingPadMs = 15.f;
constexpr float kArrivalHoldMs = 120.f;
constexpr float kTauTieMs = 10.f;
constexpr int kMaxRolloutSteps = 24;
constexpr float kEscapeEpsilon = 0.01f;

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

float SanitizeFrameMs(float value)
{
    return std::isfinite(value) && value > 0.f ? std::clamp(value, 1.f, 250.f) : kDefaultFrameMs;
}

float SanitizeSetting(float value, float fallback, float lo, float hi)
{
    if (!std::isfinite(value)) return fallback;
    return std::clamp(value, lo, hi);
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

float EffectiveThreatHalf(const Threat& threat, const Settings& settings)
{
    const float hitScale = SanitizeSetting(settings.projectileHitScale, Settings{}.projectileHitScale, 0.f, 3.f);
    const float playerRadius = SanitizeSetting(settings.playerRadius, Settings{}.playerRadius, 0.f, 1.f);
    const float clearance = SanitizeSetting(settings.clearanceTiles, Settings{}.clearanceTiles, 0.f, 1.f);
    return std::max(0.f, threat.radius * hitScale) + playerRadius + clearance;
}

void ThreatBounds(const Threat& threat, Vec2& outMin, Vec2& outMax)
{
    if (threat.hasBounds) {
        outMin = threat.boundsMin;
        outMax = threat.boundsMax;
        return;
    }

    const int n = SampleCount(threat);
    if (n <= 0) {
        outMin = {};
        outMax = {};
        return;
    }

    outMin = threat.samples[0];
    outMax = threat.samples[0];
    for (int i = 1; i < n; ++i) {
        outMin.x = std::min(outMin.x, threat.samples[i].x);
        outMin.y = std::min(outMin.y, threat.samples[i].y);
        outMax.x = std::max(outMax.x, threat.samples[i].x);
        outMax.y = std::max(outMax.y, threat.samples[i].y);
    }
}

bool ThreatBoundsCanHitPoint(const Threat& threat, Vec2 point, float half)
{
    Vec2 boundsMin{};
    Vec2 boundsMax{};
    ThreatBounds(threat, boundsMin, boundsMax);
    return point.x >= boundsMin.x - half && point.x <= boundsMax.x + half &&
        point.y >= boundsMin.y - half && point.y <= boundsMax.y + half;
}

float EffectiveBlockerHalf(const Blocker& blocker, const Settings& settings)
{
    const float playerRadius = SanitizeSetting(settings.playerRadius, Settings{}.playerRadius, 0.f, 1.f);
    const float clearance = SanitizeSetting(settings.clearanceTiles, Settings{}.clearanceTiles, 0.f, 1.f);
    const float enemyAvoidance = blocker.kind == Blocker::Kind::Enemy
        ? SanitizeSetting(settings.enemyAvoidanceRadius, Settings{}.enemyAvoidanceRadius, 0.f, 3.f)
        : 0.f;
    return std::max(0.f, blocker.radius) + playerRadius + clearance + enemyAvoidance;
}

float SampleTimeMs(const Threat& threat, int sample)
{
    const float value = threat.sampleTimesMs[std::clamp(sample, 0, kMaxPathSamples - 1)];
    return std::isfinite(value) && value >= 0.f ? value : 0.f;
}

bool TimeRangeOverlaps(float sampleStartMs, float sampleEndMs, float arrivalMs, float holdMs)
{
    const float lo = std::max(0.f, arrivalMs - kTimingPadMs);
    const float hi = arrivalMs + std::max(holdMs, 0.f) + kTimingPadMs;
    if (sampleStartMs > sampleEndMs) std::swap(sampleStartMs, sampleEndMs);
    return sampleStartMs <= hi && sampleEndMs >= lo;
}

bool ClipSegmentToTimeWindow(float sampleStartMs, float sampleEndMs, float arrivalMs, float holdMs, float& outT0, float& outT1)
{
    const float lo = std::max(0.f, arrivalMs - kTimingPadMs);
    const float hi = arrivalMs + std::max(holdMs, 0.f) + kTimingPadMs;
    if (sampleStartMs > sampleEndMs) std::swap(sampleStartMs, sampleEndMs);
    if (sampleStartMs > hi || sampleEndMs < lo) return false;

    const float denom = sampleEndMs - sampleStartMs;
    if (denom <= 0.0001f) {
        outT0 = 0.f;
        outT1 = 0.f;
        return true;
    }

    outT0 = std::clamp((std::max(sampleStartMs, lo) - sampleStartMs) / denom, 0.f, 1.f);
    outT1 = std::clamp((std::min(sampleEndMs, hi) - sampleStartMs) / denom, 0.f, 1.f);
    return outT0 <= outT1;
}

float ArrivalMs(Vec2 from, Vec2 to, float moveBudget, float frameMs)
{
    const float dist = Len(Sub(to, from));
    if (dist <= 0.0001f || moveBudget <= 0.0001f) return 0.f;
    return (dist / moveBudget) * SanitizeFrameMs(frameMs);
}

Vec2 ClosestPointOnSegment(Vec2 point, Vec2 a, Vec2 b)
{
    const Vec2 ab = Sub(b, a);
    const float denom = LenSq(ab);
    if (denom < 0.000001f) return a;
    const float t = std::clamp(Dot(Sub(point, a), ab) / denom, 0.f, 1.f);
    return Add(a, Mul(ab, t));
}

bool ThreatHitsPointAt(const Threat& threat, Vec2 point, const Settings& settings, float arrivalMs, float holdMs)
{
    const float half = EffectiveThreatHalf(threat, settings);
    if (!ThreatBoundsCanHitPoint(threat, point, half)) return false;
    const int n = SampleCount(threat);
    for (int i = 0; i < n; ++i) {
        const float sampleMs = SampleTimeMs(threat, i);
        if (TimeRangeOverlaps(sampleMs, sampleMs, arrivalMs, holdMs) &&
            ChebDistance(point, threat.samples[i]) <= half) return true;
        if (i + 1 < n) {
            const float nextMs = SampleTimeMs(threat, i + 1);
            float t0 = 0.f;
            float t1 = 0.f;
            if (ClipSegmentToTimeWindow(sampleMs, nextMs, arrivalMs, holdMs, t0, t1)) {
                const Vec2 a = Add(threat.samples[i], Mul(Sub(threat.samples[i + 1], threat.samples[i]), t0));
                const Vec2 b = Add(threat.samples[i], Mul(Sub(threat.samples[i + 1], threat.samples[i]), t1));
                const Vec2 closest = ClosestPointOnSegment(point, a, b);
                if (ChebDistance(point, closest) <= half) return true;
            }
        }
    }
    return false;
}

bool BlockerHitsPoint(const Blocker& blocker, Vec2 point, const Settings& settings)
{
    return ChebDistance(point, blocker.pos) <= EffectiveBlockerHalf(blocker, settings);
}

float BlockerClearance(const Blocker& blocker, Vec2 point, const Settings& settings)
{
    return ChebDistance(point, blocker.pos) - EffectiveBlockerHalf(blocker, settings);
}

bool IsEscapingExistingEnemyOverlap(const Blocker& blocker, Vec2 player, Vec2 point, const Settings& settings)
{
    if (blocker.kind != Blocker::Kind::Enemy) return false;
    if (!BlockerHitsPoint(blocker, player, settings)) return false;
    return ChebDistance(point, blocker.pos) > ChebDistance(player, blocker.pos) + kEscapeEpsilon;
}

CandidateRejectReason RejectReasonAt(const Vec2& point, float arrivalMs, float holdMs, const Settings& settings, const SensorSnapshot& sensors)
{
    for (int i = 0; i < BlockerCount(sensors); ++i)
        if (BlockerHitsPoint(sensors.blockers[i], point, settings)) return CandidateRejectReason::Blocker;
    for (int i = 0; i < ThreatCount(sensors); ++i)
        if (ThreatHitsPointAt(sensors.threats[i], point, settings, arrivalMs, holdMs)) return CandidateRejectReason::Projectile;
    return CandidateRejectReason::None;
}

CandidateRejectReason RejectReasonForMove(Vec2 player, Vec2 point, float arrivalMs, float holdMs, const Settings& settings, const SensorSnapshot& sensors)
{
    for (int i = 0; i < BlockerCount(sensors); ++i) {
        const Blocker& blocker = sensors.blockers[i];
        if (!BlockerHitsPoint(blocker, point, settings)) continue;
        if (IsEscapingExistingEnemyOverlap(blocker, player, point, settings)) continue;
        return CandidateRejectReason::Blocker;
    }
    for (int i = 0; i < ThreatCount(sensors); ++i)
        if (ThreatHitsPointAt(sensors.threats[i], point, settings, arrivalMs, holdMs)) return CandidateRejectReason::Projectile;
    return CandidateRejectReason::None;
}

float ThreatClearanceAt(Vec2 point, float arrivalMs, float holdMs, const Settings& settings, const SensorSnapshot& sensors)
{
    float best = 9999.f;
    for (int i = 0; i < ThreatCount(sensors); ++i) {
        const Threat& threat = sensors.threats[i];
        const float half = EffectiveThreatHalf(threat, settings);
        if (!ThreatBoundsCanHitPoint(threat, point, half)) continue;
        const int n = SampleCount(threat);
        for (int sample = 0; sample < n; ++sample) {
            const float sampleMs = SampleTimeMs(threat, sample);
            if (!TimeRangeOverlaps(sampleMs, sampleMs, arrivalMs, holdMs)) continue;
            best = std::min(best, ChebDistance(point, threat.samples[sample]) - half);
        }
    }
    return best == 9999.f ? 10.f : best;
}

float ClearanceAt(Vec2 point, float arrivalMs, float holdMs, const Settings& settings, const SensorSnapshot& sensors)
{
    float best = 9999.f;
    for (int i = 0; i < BlockerCount(sensors); ++i)
        best = std::min(best, BlockerClearance(sensors.blockers[i], point, settings));
    best = std::min(best, ThreatClearanceAt(point, arrivalMs, holdMs, settings, sensors));
    return best == 9999.f ? 10.f : best;
}

struct ThreatFlow {
    Vec2 dir{};
    float coherence = 0.f;
    bool hasFlow = false;
};

ThreatFlow ComputeThreatFlow(const SensorSnapshot& sensors)
{
    Vec2 sum{};
    float totalWeight = 0.f;
    for (int i = 0; i < ThreatCount(sensors); ++i) {
        const Threat& threat = sensors.threats[i];
        const int n = SampleCount(threat);
        if (n < 2) continue;
        const Vec2 dir = Normalize(Sub(threat.samples[n - 1], threat.samples[0]));
        if (LenSq(dir) <= 0.0001f) continue;
        const float weight = std::max(1.f, threat.damage * 0.01f);
        sum = Add(sum, Mul(dir, weight));
        totalWeight += weight;
    }

    ThreatFlow flow{};
    if (totalWeight <= 0.f) return flow;
    const float magnitude = Len(sum);
    flow.coherence = std::clamp(magnitude / totalWeight, 0.f, 1.f);
    flow.hasFlow = magnitude > 0.0001f;
    flow.dir = flow.hasFlow ? Mul(sum, 1.f / magnitude) : Vec2{};
    return flow;
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

struct HeadingEval {
    Vec2 dir{};
    Vec2 target{};
    float tauMs = 0.f;
    float score = 0.f;
    float clearance = 10.f;
    CandidateRejectReason rejectReason = CandidateRejectReason::None;
};

float PlanningDistanceTiles(float moveBudget, float frameMs, float horizonMs, const Settings& settings)
{
    const float maxMoveTiles = SanitizeNonNegative(settings.maxMoveTiles, Settings{}.maxMoveTiles);
    const float speedTilesPerMs = moveBudget > 0.f ? moveBudget / frameMs : 0.f;
    const float horizonTravel = speedTilesPerMs * horizonMs;
    return std::clamp(std::max(maxMoveTiles, horizonTravel), maxMoveTiles, 6.f);
}

float RolloutStepMs(float horizonMs, const Settings& settings)
{
    const float configured = SanitizeSetting(settings.sampleStepMs, Settings{}.sampleStepMs, 16.f, 100.f);
    const float capped = horizonMs / static_cast<float>(kMaxRolloutSteps - 1);
    return std::max(configured, capped);
}

HeadingEval EvaluateHeading(const PlanRequest& req, Vec2 dir, float moveBudget, float frameMs, float horizonMs, float planningDistance, const ThreatFlow& flow)
{
    HeadingEval eval{};
    eval.dir = dir;
    eval.target = Add(req.player, Mul(dir, planningDistance));
    eval.tauMs = horizonMs;

    const float stepMs = RolloutStepMs(horizonMs, req.settings);
    const float speedTilesPerMs = moveBudget > 0.f ? moveBudget / frameMs : 0.f;
    const bool moving = LenSq(dir) > 0.0001f && speedTilesPerMs > 0.f;
    eval.clearance = ClearanceAt(req.player, 0.f, 0.f, req.settings, req.sensors);
    for (float tMs = moving ? stepMs : 0.f; tMs <= horizonMs + 0.001f; tMs += stepMs) {
        const float travel = moving ? std::min(planningDistance, speedTilesPerMs * tMs) : 0.f;
        const Vec2 pos = Add(req.player, Mul(dir, travel));
        eval.clearance = std::min(eval.clearance, ClearanceAt(pos, tMs, 0.f, req.settings, req.sensors));
        const CandidateRejectReason reason = moving
            ? RejectReasonForMove(req.player, pos, tMs, 0.f, req.settings, req.sensors)
            : RejectReasonAt(pos, tMs, 0.f, req.settings, req.sensors);
        if (reason != CandidateRejectReason::None) {
            eval.tauMs = tMs;
            eval.rejectReason = reason;
            break;
        }
    }

    const bool hasIntent = LenSq(req.intentDir) > 0.0001f;
    const Vec2 intentDir = Normalize(req.intentDir);
    const float intentDot = hasIntent ? Dot(dir, intentDir) : 0.f;
    const float intentWeight = SanitizeSetting(req.settings.intentWeight, Settings{}.intentWeight, 0.f, 10.f);
    const float perpWeight = SanitizeSetting(req.settings.perpWeight, Settings{}.perpWeight, 0.f, 10.f);
    const float clearanceWeight = SanitizeSetting(req.settings.clearanceWeight, Settings{}.clearanceWeight, 0.f, 5.f);
    const float backpedalPenalty = SanitizeSetting(req.settings.backpedalPenalty, Settings{}.backpedalPenalty, 0.f, 10.f);
    const float arrivalMs = moving ? ArrivalMs(req.player, eval.target, moveBudget, frameMs) : 0.f;
    const float targetClearance = ClearanceAt(eval.target, arrivalMs, kArrivalHoldMs, req.settings, req.sensors);
    eval.clearance = std::min(eval.clearance, targetClearance);

    float perpScore = 0.f;
    if (flow.hasFlow && moving) {
        const float parallel = std::fabs(Dot(dir, flow.dir));
        perpScore = (1.f - parallel * 2.f) * flow.coherence;
    }

    eval.score = eval.tauMs * 10.f;
    eval.score += perpScore * perpWeight;
    eval.score += intentDot * intentWeight;
    eval.score += std::clamp(eval.clearance, -2.f, 4.f) * clearanceWeight;
    if (hasIntent && intentDot < 0.f) eval.score += intentDot * backpedalPenalty;
    if (!moving) eval.score -= flow.hasFlow ? perpWeight * flow.coherence : 0.f;
    return eval;
}

} // namespace

bool IsPointSafe(const Vec2& point, const Settings& settings, const SensorSnapshot& sensors)
{
    return RejectReasonAt(point, 0.f, settings.reactWindowMs, settings, sensors) == CandidateRejectReason::None;
}

bool IsSweepSafe(const Vec2& from, const Vec2& to, const Settings& settings, const SensorSnapshot& sensors, float frameMs)
{
    constexpr int kSteps = 4;
    const float safeFrameMs = SanitizeFrameMs(frameMs);
    for (int i = 1; i <= kSteps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kSteps);
        const Vec2 p = Add(from, Mul(Sub(to, from), t));
        if (RejectReasonForMove(from, p, safeFrameMs * t, 0.f, settings, sensors) != CandidateRejectReason::None)
            return false;
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
        const float r = EffectiveBlockerHalf(blocker, settings);
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
    const float frameMs = SanitizeFrameMs(req.frameMs);
    const float horizonMs = SanitizeSetting(req.settings.reactWindowMs, Settings{}.reactWindowMs, 100.f, 2500.f);
    const float planningDistance = PlanningDistanceTiles(moveBudget, frameMs, horizonMs, req.settings);
    if (threatCount == 0 && blockerCount == 0) {
        out.status = FrameStatus::NoThreats;
        return out;
    }

    const bool hasIntent = LenSq(req.intentDir) > 0.0001f;
    const Vec2 intentDir = Normalize(req.intentDir);
    const Vec2 intended = Add(req.player, Mul(intentDir, hasIntent ? moveBudget : 0.f));
    const bool intentSafe = hasIntent
        ? IsSweepSafe(req.player, intended, req.settings, req.sensors, frameMs)
        : RejectReasonAt(req.player, 0.f, horizonMs, req.settings, req.sensors) == CandidateRejectReason::None;
    if (intentSafe) {
        out.status = hasIntent ? FrameStatus::IntentSafe : FrameStatus::NoThreats;
        return out;
    }

    out.slideDir = ComputeSlideDirection(intentDir, req.player, req.settings, req.sensors);
    const ThreatFlow flow = ComputeThreatFlow(req.sensors);
    HeadingEval best{};
    best.tauMs = -1.f;
    float bestScore = -999999.f;
    bool found = false;
    HeadingEval fallback{};
    float fallbackScore = -999999.f;
    bool haveFallback = false;

    const HeadingEval hold = EvaluateHeading(req, Vec2{}, moveBudget, frameMs, horizonMs, planningDistance, flow);
    AppendCandidateDebug(out, req.player, hold.tauMs > frameMs, hold.rejectReason, hold.score);
    best = hold;
    bestScore = hold.score;
    found = hold.tauMs > frameMs;

    for (int i = 0; i < kCandidateDirections; ++i) {
        const float angle = kTwoPi * static_cast<float>(i) / static_cast<float>(kCandidateDirections);
        const Vec2 dir{ std::cos(angle), std::sin(angle) };
        const HeadingEval eval = EvaluateHeading(req, dir, moveBudget, frameMs, horizonMs, planningDistance, flow);
        const bool safeForNow = eval.tauMs > frameMs;
        AppendCandidateDebug(out, eval.target, safeForNow, eval.rejectReason, eval.score);
        if (!haveFallback || eval.score > fallbackScore) {
            fallback = eval;
            fallbackScore = eval.score;
            haveFallback = true;
        }
        if (!safeForNow) continue;
        if (!found || eval.tauMs > best.tauMs + kTauTieMs ||
            (std::fabs(eval.tauMs - best.tauMs) <= kTauTieMs && eval.score > bestScore)) {
            best = eval;
            bestScore = eval.score;
            found = true;
        }
    }

    if (found) {
        if (LenSq(best.dir) <= 0.0001f) {
            out.status = FrameStatus::NoThreats;
            return out;
        }
        out.status = best.tauMs >= horizonMs ? FrameStatus::CandidateAssist : FrameStatus::SlideAssist;
        out.target = best.target;
        out.shouldMove = true;
        return out;
    }

    if (haveFallback) {
        out.status = FrameStatus::SlideAssist;
        out.target = fallback.target;
        out.shouldMove = true;
        return out;
    }

    out.status = FrameStatus::NoSafeCandidate;
    return out;
}

} // namespace ZDodge::Planner
