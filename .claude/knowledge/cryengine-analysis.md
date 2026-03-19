# CryEngine Feature Analysis for SparkEngine

**Last updated:** 2026-03-19
**Type:** Decision
**Status:** Resolved

## Description

Comprehensive analysis of CryEngine (https://github.com/ValtoGameEngines/CryEngine) to identify features and architectural patterns worth adopting in SparkEngine. Follows the same approach as the TrinityCore analysis — only patterns and ideas are adopted, no code copying.

## Context

SparkEngine had 29 working systems but critical gaps in terrain rendering, advanced AI (cover, formations, group tactics), material effects, sky/water rendering, occlusion culling, and animation features. CryEngine is a AAA game engine with ~800+ source files across 20+ modules.

## CryEngine Module Summary

| Module | Files | Key Features |
|--------|-------|-------------|
| Cry3DEngine | ~200 | Terrain (18 files), SVO GI, Nishita Sky, Water, Occlusion (portals + SW rasterizer), Vegetation, Fog volumes |
| RenderDll | ~100+ | D3D11/D3D12/Vulkan, Deferred shading, HDR, AO, Shadows, Post-processing |
| CryAISystem | ~143 | NavMesh, TacticalPointSystem, Cover, Formation, GroupAI, Perception, CollisionAvoidance |
| CryAnimation | ~121 | Skeletal, PoseModifiers, ParametricSampler, AnimCompression, Ragdoll, FacialAnimation |
| CryPhysics | ~94 | Rigid/Soft/Rope/Vehicle/Water/Articulated physics |
| CryEntitySystem | ~100 | ProximityTriggers, AreaManager, EntityArchetypes, Layers |
| CryDynamicResponseSystem | ~62 | Signal/condition-driven dialogue, Variable tracking, Speaker management |
| CryMovie | ~66 | Cinematic sequencer: camera/light/entity/audio/material/post-FX tracks |
| CryNetwork | ~48+ | Compression, Cryptography, Streams, VOIP, Socket |
| CryAction | Large | Vehicle system, Item system, Inventory, MaterialEffects, GameRules, LevelSystem |

## Features Adopted (16 systems)

### HIGH Priority — Filled Critical Gaps
1. **TerrainRenderer** (.cpp) — Sector-based terrain with heightmap LOD, splatmap blending
2. **TacticalPointSystem** — Query-based tactical position evaluation (cover, vantage, ambush)
3. **CoverSystem** — Cover analysis from geometry, AI cover querying during combat
4. **MaterialEffectSystem** — Maps (interaction, surface) → particles + sounds + decals

### MEDIUM Priority — Significant Improvements
5. **FormationSystem** — Predefined formation shapes, AI slot assignment, leader-based movement
6. **GroupAISystem** — Shared threat knowledge, role assignment, coordinated tactics
7. **CollisionAvoidanceSystem** — ORCA-based velocity obstacle avoidance for crowds
8. **DynamicResponseSystem** — Signal/condition-driven contextual dialogue and actions
9. **EntityArchetypeSystem** — Template-based entity creation, data-driven spawning
10. **ProximityTriggerSystem** — Spatial trigger volumes with enter/exit events

### LOW Priority — Rendering & Animation
11. **SkyAtmosphereSystem** — Preetham analytical sky model with sun/atmosphere scattering
12. **WaterRenderer** — Gerstner wave animation, water height sampling
13. **OcclusionCullingSystem** — CPU software rasterization occlusion testing
14. **PoseModifierStack** — Post-animation pose adjustments (look-at, aim IK, foot placement)
15. **AnimCompression** — Quantized keyframe compression for animation data
16. **BlendSpace2D** — 2D parametric animation blending (speed × direction → anim blend)

## Features NOT Adopted

| Feature | Reason |
|---------|--------|
| Flow Graph / Visual Scripting | Deleted twice as orphaned; AngelScript covers scripting |
| Cinematic Sequencer (CryMovie) | Massive scope, no current caller |
| Procedural Generation | No game module uses it |
| Vehicle System | Deeply coupled to CryPhysics; Bullet3 doesn't support it |
| Schematyc | EnTT ECS serves the same purpose |
| SVO Global Illumination | Extremely complex, requires compute shaders |
| VOIP | Not relevant to SparkEngine's scope |
| Facial Animation | No character assets |

## Notes

- All 16 systems follow SparkEngine anti-bloat rules: <500 line .cpp, <300 line .h, all wired in
- Each system has unit tests in Tests/
- Wired into SparkEngine.cpp Init/Update/Shutdown paths
- Pattern: singletons with GetInstance(), registered in EngineContext where appropriate
