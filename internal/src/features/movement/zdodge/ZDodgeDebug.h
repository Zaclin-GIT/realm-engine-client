#pragma once

#include "ZDodgeTypes.h"

namespace ZDodge::Debug {

void Render(const DebugSnapshot& snapshot, const Settings& settings, float camX, float camY, float angle, float zoom, float cx, float cy);

} // namespace ZDodge::Debug