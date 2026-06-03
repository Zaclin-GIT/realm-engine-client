// Purpose: declares the runtime feature-application surface used by the IPC
// bridge and hook loop.

// Helpful notes:
// - ApplyOverrides pushes current FeatureState values into gameplay systems.
// - PollSocketHotkeyEvent reports edge-triggered socket hotkey presses so IPC can
//   emit a signed hotkey event back to the client.
// - ApplyPluginToggleHotkeys parses the "id=combo;..." spec from the client and
//   CollectPluginToggleHotkeyEvents reports edge-triggered presses per plugin id.

#pragma once

#include <string>
#include <vector>

namespace FeatureRuntime {

void ApplyOverrides();
bool PollSocketHotkeyEvent();

// Plugin toggle hotkeys (owner feature): bind arbitrary key combos to plugin ids.
void ApplyPluginToggleHotkeys(const char* spec);
void CollectPluginToggleHotkeyEvents(std::vector<std::string>& outPluginIds);

} // namespace FeatureRuntime
