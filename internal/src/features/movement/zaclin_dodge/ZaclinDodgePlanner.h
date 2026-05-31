#pragma once

#include "ZaclinDodgeTypes.h"

namespace ZaclinDodge::Planner {

PlanResult Evaluate(const PlanRequest& req);
bool IsPointSafe(const Vec2& point, const Settings& settings, const SensorSnapshot& sensors);
bool IsSweepSafe(const Vec2& from, const Vec2& to, const Settings& settings, const SensorSnapshot& sensors);
Vec2 ComputeSlideDirection(const Vec2& desiredDir, const Vec2& player, const Settings& settings, const SensorSnapshot& sensors);

} // namespace ZaclinDodge::Planner
