#pragma once
#include <cstdint>

struct ImDrawList;

namespace ZDodge::Target {

// Maximum screen-space distance (pixels) from the click to an enemy's projected
// centre for that enemy to be considered a valid pick target.
constexpr float kPickThresholdPx = 80.f;

// Call on each Shift+LMB press (edge only, not hold).
// Projects all live enemies to screen and picks the closest one within
// kPickThresholdPx of (sx, sy).  Clears the existing target if nothing is close.
void ProcessClick(float sx, float sy,
                  float camX, float camY, float angle, float zoom,
                  float cx,   float cy);

// Directly set a target by entity ID (use when the ID is already resolved by
// another system, e.g. the existing AutoAim chord handler in TestTAB).
void SetTarget(int32_t id);

// Clears the current target.
void Clear();

bool    HasTarget();
int32_t GetId();

// Draws the target indicator using the caller's draw list.
// No-op when HasTarget() is false.
void Render(ImDrawList* draw,
            float camX, float camY, float angle, float zoom,
            float cx,   float cy);

} // namespace ZDodge::Target
