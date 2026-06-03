#pragma once

namespace ZDodge {

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
void SetProjectileHitScale(float scale);
float GetProjectileHitScale();
void SetProjectileRadiusFallback(float radius);
float GetProjectileRadiusFallback();
void SetClearanceTiles(float tiles);
float GetClearanceTiles();
void SetSampleStepMs(float ms);
float GetSampleStepMs();
void SetPerpWeight(float weight);
float GetPerpWeight();
void SetIntentWeight(float weight);
float GetIntentWeight();
void SetClearanceWeight(float weight);
float GetClearanceWeight();
void SetBackpedalPenalty(float weight);
float GetBackpedalPenalty();
void SetEnemyAvoidanceRadius(float radius);
float GetEnemyAvoidanceRadius();
void SetDamageThresholdPct(float pct);
float GetDamageThresholdPct();
void SetDebugOverlay(bool enabled);
bool GetDebugOverlay();
void SetCandidateOverlay(bool enabled);
bool GetCandidateOverlay();

} // namespace ZDodge