#pragma once

#include <cstdint>

namespace ZDodge {

constexpr int kCandidateDirections = 24;
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
    float reactWindowMs = 1200.f;
    float maxMoveTiles = 0.55f;
    float playerRadius = 0.05f;
    float projectileHitScale = 0.9f;
    float projectileRadiusFallback = 0.02f;
    float damageThresholdPct = 0.f;
    float clearanceTiles = 0.03f;
    float sampleStepMs = 40.f;
    float perpWeight = 6.0f;
    float intentWeight = 2.5f;
    float clearanceWeight = 1.5f;
    float backpedalPenalty = 3.0f;
    float enemyAvoidanceRadius = 2.0f;
    bool debugOverlay = true;
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
    float sampleTimesMs[kMaxPathSamples]{};
    Vec2 boundsMin{};
    Vec2 boundsMax{};
    bool hasBounds = false;
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
    bool projectileSourceUnavailable = false;
    bool projectileLimited = false;
    bool aoeLimited = false;
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
    float frameMs = 16.667f;
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
