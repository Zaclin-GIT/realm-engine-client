#pragma once

#include "ZaclinDodgeTypes.h"

namespace ZaclinDodge::Sensors {

SensorSnapshot Build(float playerX, float playerY, const Settings& settings);

} // namespace ZaclinDodge::Sensors
