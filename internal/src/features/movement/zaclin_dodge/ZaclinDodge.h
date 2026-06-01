#pragma once

namespace ZaclinDodge {

void SetEnabled(bool enabled);
bool IsEnabled();
void OnEnter();
void Tick(void* player, float px, float py, float dt);
void RenderSettings();
void RenderDebugOverlay(float camX, float camY, float angle, float zoom, float cx, float cy);

void SetReactWindowMs(float ms);
float GetReactWindowMs();
void SetMaxMoveTiles(float tiles);
float GetMaxMoveTiles();
void SetPlayerRadius(float radius);
float GetPlayerRadius();
void SetProjectileRadiusFallback(float radius);
float GetProjectileRadiusFallback();
void SetDamageThresholdPct(float pct);
float GetDamageThresholdPct();
void SetDebugOverlay(bool enabled);
bool GetDebugOverlay();
void SetCandidateOverlay(bool enabled);
bool GetCandidateOverlay();

} // namespace ZaclinDodge