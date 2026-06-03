# Zaclin Dodge Design

Date: 2026-05-31

## Purpose

The current dodge feature is too large, difficult to reason about, and unreliable in-game. It sometimes moves the player, but it is not consistently good at avoiding projectiles and has accumulated many interacting systems, toggles, and bug-prone behaviors.

This design introduces a new selectable dodge mode named `ZDodge`, inspired by `learnings/MicroDodgeCalculator.cs` and the tracker examples in `learnings/ProjectileTracker.cs`, `learnings/EnemyTracker.cs`, and `learnings/WorldObjectTracker.cs`. The goal is a small first version that dodges projectiles, avoids obstacles and enemy bodies, gives strong debug visibility, and remains scalable without inheriting the current XDodge/DangerPlanner complexity.

## Scope

Version 1 creates a new dodge mode beside the existing modes. It does not replace XDodge or Rollout yet. The new mode should be easy to compare in-game and can become the default only after it proves better.

Included in v1:

- A new isolated engine under `internal/src/features/movement/zdodge`.
- Projectile survival using a MicroDodge-style candidate sampler.
- Hard blocking for obstacles and enemies.
- Complete debug visibility for tracked projectiles, projectile paths, tracked enemies, tracked obstacles, sampled candidates, and current movement target.
- Intent-preserving movement: follow the player's movement when safe, and redirect only the unsafe part of that movement around projectiles, enemies, and obstacles.
- Configurable projectile damage filtering, defaulting to all enemy projectiles as dangerous.

Excluded from v1:

- Replacing XDodge/Rollout.
- Strategic orbit/follow pathing.
- A* or BFS grid planning.
- Prediction-error learning and per-type bullet model correction.
- Raw position writes or movement that bypasses the game's native move path.

## Architecture

`ZDodge` owns its own state, candidate sampler, sensor snapshots, debug snapshots, settings, and overlay. It must not depend on XDodge or DangerPlanner planning internals.

Allowed shared dependencies are small, low-level systems that already represent the safest available source of truth:

- `ProjectileTracking` for active projectile snapshots and projectile prediction.
- `DodgeRuntime` for speed-clamped native movement calls.
- `SteerInput` or existing player-intent reads to build the intended movement vector.
- Existing W2S/drawing helpers for debug overlay rendering.
- Small pure geometry helpers when they match the axis-aligned projectile collision model.

Proposed files:

- `ZDodge.h/.cpp`: public mode API, enable/disable, tick, settings, and overlay entry points.
- `ZDodgeTypes.h`: shared data structs for threats, blockers, candidates, settings, and debug snapshots.
- `ZDodgeSensors.h/.cpp`: collects projectile, enemy, and obstacle snapshots with isolated pointer handling.
- `ZDodgePlanner.h/.cpp`: candidate generation, safety checks, scoring, fallback selection, and movement target calculation.
- `ZDodgeDebug.h/.cpp`: draws tracked objects, projectile paths, candidate state, and chosen target.

Each module has one purpose: gather world state, evaluate safe points, issue movement, or draw debug state.

## Runtime Behavior

Each game-update tick, `ZDodge::Tick(player, px, py, dt)` runs only when the new mode is enabled.

1. Read player state: position, effective movement speed, delta time, HP/defense when available, and WASD state.
2. Refresh sensor/debug snapshots every tick so the overlay remains current.
3. Compute the player's intended next position from WASD input and movement speed. If no WASD input is active, the intended position is the current position.
4. If no relevant threats or blockers exist, let normal player movement continue and do not issue an extra dodge move.
5. If the intended movement path is safe for the configured reaction window, do not override it.
6. If the intended movement path is unsafe, compute an assist target that preserves as much of the player's requested movement as possible while sliding around projectiles, enemies, and obstacles.
7. Reject assist candidates that collide with projectile paths, obstacles, or enemy bodies.
8. Sweep-check from current position to candidate so the movement step does not cross through danger.
9. Score safe candidates by alignment with player intent, projectile clearance, short distance, and stability.
10. Clamp the chosen target to the per-frame movement budget and call the native movement path.

The default feel should be assistive rather than overriding. The engine should not wander around looking for a better position while the player's intended movement is safe. When the intended move is unsafe, the dodge correction should feel like a zero-friction slide along nearby circular blockers: preserve tangential movement, remove the component that drives into danger, and only choose a different direction when sliding cannot produce a safe step.

## Sensors

Projectile data should use the existing C++ projectile tracking first, despite the current dodge implementation not being trustworthy overall. The useful low-level pieces are already wired into spawn hooks and prediction helpers, and reacquiring all projectile state through a brand-new C++ scene scan would add risk to v1.

`ZDodgeSensors` should adapt existing projectile records into a compact threat snapshot:

- Current position.
- Predicted path samples over the reaction window.
- Projectile radius, using a configurable fallback when unavailable.
- Optional damage metadata for threshold filtering.
- Stable ID or slot identity for debug display.

Enemy and obstacle tracking should be isolated from XDodge/DangerPlanner. The C# examples establish the desired pattern: cache expensive pool/object discovery, refresh cheap per-frame or throttled state, and clear on realm/reconnect. In C++, any pointer chasing must be SEH-guarded and kept inside `ZDodgeSensors`.

For v1, enemies are hard blockers. Obstacles are hard blockers. If a blocker source fails for a frame, that source is skipped for that frame and marked in debug state, while projectile safety continues to work.

## Planner

The planner is inspired by `MicroDodgeCalculator`, but adapted to the DLL's available data and collision rules. Projectile safety uses the axis-aligned/Chebyshev hit convention documented in the current C++ dodge helpers rather than the C# reference's circular distance check.

Core algorithm:

- Use fixed-size reusable buffers.
- Sample 32 fixed directions by default, with the candidate-count constant kept isolated so it can be raised later if in-game testing shows directional gaps.
- Run several radius passes from small correction to max correction distance.
- Treat player intent as the preferred velocity vector. Candidate scoring rewards alignment with that vector so dodge assistance follows the player's movement whenever possible.
- When an intended step intersects a circular blocker approximation, compute a slide direction by removing the inward component of the velocity and preserving the tangent component, like frictionless 2D circles sliding past each other.
- Evaluate candidate point safety against projected projectile paths over the reaction window.
- Reject candidates inside obstacle or enemy blocker radius.
- Sweep-check movement from current position to candidate.
- Prefer the safe candidate that best preserves player intent, then projectile clearance, then short distance.
- If no fully safe candidate exists, allow a least-unsafe fallback only when it improves survival and is not blocked.

The planner should expose enough debug data to explain every frame: threat count, blocker count, intended movement vector, slide vector, candidate positions, candidate safe/rejected state, candidate score, selected target, and no-move reason.

## Settings, UI, And IPC

The new mode should be exposed as `Zaclin` in the existing dodge mode selector, beside `Off`, `RE-Plus/XDodge`, and `RE-Sim/Rollout`.

Initial settings:

- `zaclinReactWindowMs`: prediction window for projectile checks.
- `zaclinMaxMoveTiles`: maximum correction search radius.
- `zaclinPlayerRadius`: player safety radius for projectile checks.
- `zaclinProjectileRadiusFallback`: fallback projectile radius.
- `zaclinDamageThresholdPct`: default `0`, meaning all enemy projectiles count.
- `zaclinDebugOverlay`: show tracked projectiles, paths, enemies, obstacles, candidates, and selected target.
- `zaclinCandidateOverlay`: optional separate toggle if full candidate drawing is too noisy.

Integration points:

- Add a new `TestTAB::DodgeMode` enum value.
- Update `ApplyDodgeModeWithEnter` so exactly one dodge engine is enabled at a time.
- Update `FeatureState::SetAutoDodgeMode` clamping for the new mode.
- Add `zaclin*` feature keys to `FeatureCommandRegistry` rather than reusing `xdodge*` keys.
- Add a dropdown value and settings to `client/plugins/auto-dodge.ts`.
- Add new source/header entries to the Visual Studio project and filters.

Existing XDodge and Rollout keys stay tied to their existing modes.

## Debug Overlay

V1 debug visibility is required, not optional polish. The overlay should show:

- Tracked projectiles.
- Projectile predicted paths.
- Tracked enemies.
- Tracked obstacles.
- Candidate points, with safe/rejected state.
- The player's intended movement vector and any slide-adjusted movement vector.
- The selected movement target.
- Frame-level status such as intent-safe, slide-assist, safe-idle, no-threats, no-safe-candidate, sensor-failed, or movement-failed.

Draw counts must be capped so crowded screens do not cause large FPS drops.

## Error Handling

The new mode should fail idle rather than risky.

- If projectile snapshots are unavailable, do not move and show/log that projectile data is unavailable.
- If enemy tracking fails, skip enemy blockers for that frame and mark the failure in debug state.
- If obstacle tracking fails, skip obstacle blockers for that frame and mark the failure in debug state.
- If native movement resolution fails, do not raw-write position. Log a throttled failure and stay idle.
- Any IL2CPP pointer chasing belongs in sensors and must be guarded.

## Performance Guardrails

- Reuse fixed-size buffers for threats, blockers, candidates, and debug data.
- Cap projectile, enemy, obstacle, path, and candidate counts.
- Throttle enemy and obstacle refreshes when full scans are needed.
- Run full candidate scoring only when the intended movement path or current idle position is unsafe.
- Keep debug drawing capped and toggleable.

## Verification Plan

Static/pure checks where possible:

- Candidate generation produces expected counts and radii.
- Safe intended movement returns no dodge override.
- Unsafe current position selects a safe nearby candidate when one exists.
- Unsafe intended movement preserves tangential player movement when a slide-safe path exists.
- Candidate rejection works for projectile paths, enemies, and obstacles.
- Sweep safety rejects moves that cross through projectile danger.

Build verification:

- Run the Debug x64 MSBuild command for `internal/il2cpp-dll-injection.sln`.

In-game validation:

- With WASD held and the intended path safe, the new mode does not interfere.
- With WASD held and the intended path unsafe, the new mode slides the movement around danger instead of taking complete control.
- With no threats, it stays idle.
- With a straight projectile crossing the player, it moves to a nearby safe candidate.
- It does not choose targets inside enemies or obstacles.
- Debug overlay matches observed projectiles, paths, enemies, obstacles, candidates, and chosen target.
- Existing XDodge and Rollout remain selectable and isolated.

## Open Implementation Notes

- The current C++ dodge implementation should not be copied as a design model. Reuse only narrow low-level helpers where they are safer than rebuilding the data source.
- Projectile collision math uses the axis-aligned/Chebyshev projectile check already documented by the C++ dodge helpers. The C# reference's circular distance logic is useful for structure, but not copied for hit detection.
- Enemy and obstacle scanning may need to start with existing world/entity sources if direct C++ pool scanning proves fragile. The `ZDodgeSensors` interface should hide that choice from the planner.