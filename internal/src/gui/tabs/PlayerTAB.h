#pragma once

namespace PlayerTAB {
    // Called every frame from dPresent (menu visibility gating handled internally).
    void Tick(bool menuVisible);
    void Render();
}
