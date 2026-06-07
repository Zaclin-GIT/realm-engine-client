#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace PlayerCollider {

using UpdateLogFn = void (*)(float before, float after, const void* properties, const char* reason);

struct ObjectPropertiesTarget {
	const char* label;
	uint32_t offset;
};

inline bool ApplyMultiplier(float& collisionRadiusMultiplier, const void* properties, const char* reason, UpdateLogFn logFn)
{
	const float before = collisionRadiusMultiplier;
	if (!std::isfinite(before) || before == 0.0f) return false;

	collisionRadiusMultiplier = 0.0f;
	if (logFn) logFn(before, collisionRadiusMultiplier, properties, reason);
	return true;
}

bool ApplyEntityMultiplier(void* entityPtr,
	uint32_t primaryObjectPropertiesOffset,
	uint32_t secondaryObjectPropertiesOffset,
	uint32_t collisionMultiplierOffset,
	const char* reason,
	UpdateLogFn logFn);

bool ApplyEntityMultiplierTargets(void* entityPtr,
	const ObjectPropertiesTarget* targets,
	size_t targetCount,
	uint32_t collisionMultiplierOffset,
	const char* reason,
	UpdateLogFn logFn);

// Master toggle. While disabled, Tick() restores any collider it previously
// zeroed and otherwise leaves the game's collisionRadiusMultiplier untouched.
// Driven by the autododge mode swap (see TestTAB::ApplyDodgeModeWithEnter).
void SetEnabled(bool enabled);
bool IsEnabled();

void Tick(void* player);
void ResetScene();
void ResetStateForTest();

} // namespace PlayerCollider