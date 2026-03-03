# SparkEngine — Feature Roadmap

> Prioritized feature plan for a solo developer building an FPS-focused game engine.

---

## Current State

SparkEngine ships with:

- **Rendering:** DirectX 11 (primary) + Vulkan/OpenGL backends (experimental), PBR materials, deferred/forward+/clustered pipelines, shadow mapping (PCF/VSM/CSM/PCSS), SSAO, SSR, volumetric lighting, multi-pass bloom, tone mapping (Reinhard/ACES/Uncharted 2), FXAA, IBL lighting, particle system
- **Physics:** Bullet Physics (rigid bodies, constraints, raycasting, overlap tests, collision callbacks)
- **Audio:** XAudio2 3D spatial audio with distance attenuation and Doppler
- **Scripting:** AngelScript with hot-reload and full engine bindings
- **ECS:** EnTT-based entity component system
- **Editor:** ImGui-based (partially complete, dead code cleaned)
- **Gameplay:** Player controller, weapon system (bullet/rocket/grenade), class system, vehicles, gravity, HUD (crosshairs, health bars, kill feed, minimap, compass)
- **Terrain:** Heightmap-based LOD terrain
- **Mesh LOD:** Distance-based mesh level of detail
- **Decals:** Projected decal system
- **AI:** NavMesh pathfinding with binary `.snav` loading
- **Save/Load:** Game state serialization via RapidJSON
- **Asset Pipeline:** OBJ mesh, TGA texture, WAV audio loading
- **Tooling:** Crash handler (minidumps), debug console (200+ commands), profiler, JSON scene serialization, prefab system

---

## Tier 1 — High Impact: Fill Critical Gaps

### 1. Skeletal Animation Pipeline

**Why:** An `AnimationController` component exists but has no implementation. FPS games need animated characters (first-person arms, enemies, weapon animations). Without this, the engine renders static meshes only.

| | |
|---|---|
| Complexity | Large |
| Key files | `Components.h`, `GraphicsEngine.cpp`, new `AnimationSystem.cpp/h` |
| Dependencies | Assimp (already integrated) for FBX/glTF animation import |

Sub-tasks:
1. Bone/skeleton data structures + FBX/glTF import via Assimp
2. GPU skinning shader (vertex shader bone transforms)
3. Animation clip playback with keyframe interpolation
4. Blending and layered animation (walk + shoot overlay)
5. Animation state machine (idle, run, jump transitions)
6. IK system (foot placement, aim IK for weapons)

### 2. AI & Navigation (NavMesh + Behavior Trees)

**Why:** NavMesh loading works, but there's no pathfinding query system or AI behavior framework. FPS games need enemies that navigate levels, take cover, and fight.

| | |
|---|---|
| Complexity | Large |
| Key files | `Engine/AI/NavMesh.cpp`, new `Pathfinding.cpp`, `BehaviorTree.cpp`, `AISystem.cpp` |
| Dependencies | Recast/Detour (MIT, industry-standard navmesh) |

Sub-tasks:
1. Integrate Recast for navmesh generation from static geometry
2. Integrate Detour for pathfinding queries
3. Steering behaviors (seek, flee, arrive, obstacle avoidance)
4. Behavior tree nodes: Selector, Sequence, Decorator, Action
5. Pre-built FPS behaviors: patrol, chase, take-cover, attack
6. Editor visualization for navmesh debugging

### 3. Networking & Multiplayer Foundation

**Why:** `ENABLE_NETWORKING` is OFF due to CURL issues. FPS games are inherently multiplayer. An authoritative server with client-side prediction would unlock online play.

| | |
|---|---|
| Complexity | Large |
| Key files | `CMakeLists.txt`, new `Engine/Networking/` |
| Dependencies | Replace CURL with ENet or Valve's GameNetworkingSockets (UDP-based, purpose-built for games) |

Sub-tasks:
1. Integrate networking library (ENet or GameNetworkingSockets)
2. Packet serialization protocol for entity state
3. Authoritative server game loop
4. Client-side prediction and server reconciliation
5. Lag compensation (rewind hitboxes for shot validation)
6. Basic lobby/matchmaking system

### 4. Expand Save/Load to Full Game State

**Why:** The save system handles level layout but doesn't capture full runtime game state (player progress, inventory, entity state). Players expect to save and resume.

| | |
|---|---|
| Complexity | Medium |
| Key files | `SceneManager.cpp/h`, `SaveSystem.cpp/h`, `Components.h` |
| Dependencies | RapidJSON (already integrated) |

Sub-tasks:
1. Define serializable interface for all gameplay components
2. Full ECS world snapshot (all entities + components)
3. Save file versioning and migration
4. Autosave and quicksave/quickload
5. Save slot management UI

---

## Tier 2 — Quality & Polish

### 5. Complete the Editor

**Why:** Dead code was cleaned up, but many panels still have TODOs. A functional editor accelerates level design and iteration.

| | |
|---|---|
| Complexity | Medium |
| Sub-tasks | Fix docking system (tab colors, save/reset), editor theme (live customization, JSON export), asset browser (import logic), editor UI (recovery dialog, layout export) |

### 6. Expanded Test Suite

**Why:** Only 11 test files covering MathUtils, ECS World, ObjectPool, and GameMode. Major systems (physics, graphics, audio, scripting) have zero coverage. Regressions are a growing risk.

| | |
|---|---|
| Complexity | Medium |
| Sub-tasks | Physics tests, scene serialization round-trip, audio tests, input mapping tests, scripting tests, ECS complex query tests |

### 7. Audio Overhaul — Music & Sound Groups

**Why:** The audio system handles basic 2D/3D playback but lacks a music pipeline, mixer buses, and environmental effects.

| | |
|---|---|
| Complexity | Medium |
| Sub-tasks | Mixer bus system (Master/SFX/Voice/Music/Ambient), music manager with crossfade and playlists, dynamic music layers, audio occlusion via raycasts, reverb zones |

---

## Tier 3 — Advanced / Future

### 8. Ray Tracing (DXR)

Optional DirectX Raytracing for reflections, GI, AO, and shadows. Requires building a D3D12 backend. The existing RHI abstraction makes this feasible as a new backend.

### 9. Procedural Generation Framework

Noise functions (Perlin, Simplex, Worley), rule-based object placement, procedural meshes, wave function collapse for room layouts. Enables randomized FPS arenas and environments.

### 10. Cinematic / Cutscene System

Timeline-based sequencer for camera paths, entity animations, audio cues, and scripted events. Needed for campaign storytelling.

### 11. Cross-Platform Input (SDL2)

Replace Win32-specific input with SDL2 for Linux/macOS support. Also provides better gamepad handling (hot-plug, rumble, gyro).

---

## Recommended Order

| # | Feature | Tier | Complexity | Rationale |
|:---:|---|:---:|:---:|---|
| 1 | Skeletal Animation | 1 | Large | Prerequisite for animated characters |
| 2 | Complete the Editor | 2 | Medium | Leverages existing half-built work |
| 3 | AI & Navigation | 1 | Large | Enables single-player enemy combatants |
| 4 | Expand Save/Load | 1 | Medium | Builds on existing serialization |
| 5 | Expanded Test Suite | 2 | Medium | Prevents regressions as engine grows |
| 6 | Audio Overhaul | 2 | Medium | Immersion and polish |
| 7 | Networking | 1 | Large | Multiplayer (large commitment) |
| 8 | Ray Tracing | 3 | Large | Next-gen graphics |
| 9 | Procedural Generation | 3 | Large | Replayability |
| 10 | Cinematic System | 3 | Large | Storytelling |
| 11 | Cross-Platform Input | 3 | Medium | Broader platform support |
