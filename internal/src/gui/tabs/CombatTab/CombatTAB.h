#pragma once

namespace CombatTAB {

/// Max distance from local player (tiles) for wizard auto-ability aim point.
constexpr float kWizardSpellMaxRangeTiles = 13.f;

void Render();
// Called every frame from dPresent (outside menu visibility gate).
void Tick(bool menuVisible);

// World-space ground target auto-ability uses (before native Y invert).
void RefreshAutoAbilityAimVisualCache();
bool GetAutoAbilityAimVisual(float& outWorldX, float& outWorldY);

// Bot-client shared-memory → mirror dashboard toggles / sliders.
void SetAutoAbility(bool enabled);
void SetAbilityMpPct(float pctZeroTo100);
void SetWizardAbilityTargetMode(int mode); // 0 = auto-aim target, 1 = cluster

// Muzzle / weapon-range debug overlay (DebugTAB draws when true).
bool MuzzleWeaponRangeDebugOverlayEnabled();

} // namespace CombatTAB
