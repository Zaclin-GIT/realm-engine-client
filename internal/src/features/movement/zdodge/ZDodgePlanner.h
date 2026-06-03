#pragma once

#include "ZDodgeTypes.h"

namespace ZDodge::Planner {

PlanResult Evaluate(const PlanRequest& req);
bool IsPointSafe(const Vec2& point, const Settings& settings, const SensorSnapshot& sensors);
bool IsSweepSafe(const Vec2& from, const Vec2& to, const Settings& settings, const SensorSnapshot& sensors, float frameMs = 16.667f);
Vec2 ComputeSlideDirection(const Vec2& desiredDir, const Vec2& player, const Settings& settings, const SensorSnapshot& sensors);

} // namespace ZDodge::Planner
