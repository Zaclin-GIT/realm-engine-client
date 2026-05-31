#pragma once

namespace CameraTAB {
    void Render();
    void ForceRefresh();    // trigger a DoRefresh() from external callers (e.g. TestTAB)

    // Last-known camera values (updated on each DoRefresh / auto-refresh tick).
    float GetAngle();       // camera rotation in degrees (from Transform.eulerAngles.z)
    float GetZoom();        // orthographicSize (smaller = more zoomed in)
    void* GetCamMgrPtr();   // CameraManager*

    // Camera.pixelRect — the exact Unity game viewport in screen pixels (bottom-left origin).
    // Use these in W2S to get an accurate viewport centre that excludes the game's UI panel.
    // cx = GetPixelRectX() + GetPixelRectW()/2
    // cy = screenH - (GetPixelRectY() + GetPixelRectH()/2)
    float GetPixelRectX();
    float GetPixelRectY();
    float GetPixelRectW();
    float GetPixelRectH();

    // Centering state (IOABMGFJLLP) — live, re-read every frame in Render().
    // true = camera NOT centred on player; false = following player (default).
    bool  GetCenteringState();
    void  SetZoomValue(float zoom);
    void  SetAngleDegrees(int angleDeg);
    void  SetCenteredOnPlayer(bool centered);
}
