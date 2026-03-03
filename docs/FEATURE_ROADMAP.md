# SparkEngine — Feature Roadmap

> Recommended new features based on a comprehensive audit of the current codebase.
> Prioritized for a solo developer building an FPS-focused game engine.

---

## Current State Summary

SparkEngine already includes:

- **Rendering:** DirectX 11 (primary) + Vulkan/OpenGL backends, PBR materials, deferred/forward+/clustered pipelines, shadow mapping (PCF/VSM/CSM/PCSS), SSAO, SSR, volumetric lighting, particle system
- **Physics:** Bullet Physics integration (rigid bodies, constraints, raycasts, collision callbacks)
- **Audio:** XAudio2-based 3D spatial audio with distance attenuation and Doppler
- **Scripting:** AngelScript with hot-reload and full engine bindings
- **ECS:** EnTT-based entity component system with a rich component library
- **Editor:** ImGui-based (partially complete — 8 panels disabled)
- **FPS Gameplay:** Player controller, weapon system (bullet/rocket/grenade), class system, vehicle system, gravity system, HUD (crosshairs, health bars, kill feed, minimap, compass)
- **Terrain:** Heightmap-based LOD terrain
- **Tooling:** Crash handler (minidumps), debug console, profiler, JSON scene serialization, prefab system

---

## Tier 1: High Impact — Fill Critical Gaps

These features are essential for a functional FPS game engine.

### 1. Skeletal Animation System

| | |
|---|---|
| **Description** | Full skeletal animation pipeline: bone hierarchies, skinned mesh rendering, animation blending, IK, and animation state machines. |
| **Why it matters** | An `AnimationController` component exists in `Components.h` but has no implementation. FPS games need animated characters (first-person arms, enemy models, weapon animations). Without this, the engine can only render static meshes. |
| **Key files** | `Components.h` (AnimationController), `GraphicsEngine.cpp`, new `Engine/ECS/AnimationSystem.cpp/h` |
| **Dependencies** | Assimp (already integrated) supports FBX/glTF animation import |
| **Complexity** | **Large** |
| **Sub-tasks** | 1. Bone/skeleton data structures and FBX/glTF import via Assimp<br>2. GPU skinning shader (vertex shader bone transforms)<br>3. Animation clip playback (keyframe interpolation)<br>4. Blending and layered animation (walk+shoot overlay)<br>5. Animation state machine (idle → run → jump transitions)<br>6. IK system (foot placement, aim IK for weapons) |

### 2. AI & Navigation System (NavMesh + Behavior Trees)

| | |
|---|---|
| **Description** | NavMesh generation from level geometry, A* pathfinding, steering behaviors, and a behavior tree framework for NPC AI. |
| **Why it matters** | There is currently zero AI or pathfinding infrastructure. FPS games need enemies that can navigate levels, take cover, flank, and exhibit believable combat behavior. This is the single biggest gameplay gap. |
| **Key files** | New `Engine/AI/` directory: `NavMesh.cpp/h`, `Pathfinding.cpp/h`, `BehaviorTree.cpp/h`, `AISystem.cpp/h` |
| **Dependencies** | Recast/Detour (industry-standard navmesh library, MIT license) |
| **Complexity** | **Large** |
| **Sub-tasks** | 1. Integrate Recast for navmesh generation from static geometry<br>2. Integrate Detour for pathfinding queries<br>3. Steering behaviors (seek, flee, arrive, obstacle avoidance)<br>4. Behavior tree nodes: Selector, Sequence, Decorator, Action<br>5. Pre-built FPS AI behaviors: patrol, chase, take-cover, attack<br>6. Editor visualization for navmesh debugging |

### 3. Networking & Multiplayer Foundation

| | |
|---|---|
| **Description** | Client/server architecture with entity replication, input prediction, lag compensation, and basic matchmaking. |
| **Why it matters** | `ENABLE_NETWORKING` is OFF (disabled due to CURL dependency issues). FPS games are inherently multiplayer. Even a basic authoritative server with client-side prediction would unlock online play. |
| **Key files** | `CMakeLists.txt` (re-enable flag), new `Engine/Networking/`: `NetworkManager.cpp/h`, `ReplicationSystem.cpp/h`, `NetProtocol.h` |
| **Dependencies** | Replace CURL with ENet or Valve's GameNetworkingSockets (purpose-built for games, UDP-based, reliable channels) |
| **Complexity** | **Large** |
| **Sub-tasks** | 1. Choose and integrate networking library (ENet or GameNetworkingSockets)<br>2. Packet serialization protocol for entity state<br>3. Authoritative server game loop<br>4. Client-side prediction and server reconciliation<br>5. Lag compensation (rewind hitboxes for shot validation)<br>6. Basic lobby/matchmaking system |

### 4. Save/Load & Game State Serialization

| | |
|---|---|
| **Description** | Serialize full runtime game state (entities, component data, player progress, inventory) to disk and restore it. |
| **Why it matters** | No save/load system exists. The scene serializer handles level layout but not runtime game state. Players expect to save and resume progress. |
| **Key files** | `SceneManager.cpp/h` (extend), new `SaveSystem.cpp/h`, `Components.h` (add serialize/deserialize methods) |
| **Dependencies** | RapidJSON (already integrated) |
| **Complexity** | **Medium** |
| **Sub-tasks** | 1. Define serializable interface for all gameplay components<br>2. Full ECS world snapshot (all entities + components)<br>3. Save file versioning and migration<br>4. Autosave and quicksave/quickload support<br>5. Save slot management UI |

---

## Tier 2: Quality & Polish

These features elevate the engine's capability and developer experience.

### 5. Complete the Editor (Re-enable Disabled Panels)

| | |
|---|---|
| **Description** | Fix and re-enable the 8 disabled editor source files: AssetDatabase, EngineInterface (IPC), DockingSystem, ConsolePanel, HierarchyPanel, SceneManager, PerformanceProfiler, and related panels. |
| **Why it matters** | These panels represent significant invested work that's currently unusable. A functional editor massively accelerates level design, debugging, and iteration speed. |
| **Key files** | All `.disabled` files in `SparkEditor/Source/`: fix compilation/linking issues, remove `.disabled` extension |
| **Complexity** | **Medium** |
| **Sub-tasks** | 1. Audit each disabled file — identify why it was disabled (missing deps, API changes, crashes)<br>2. Fix AssetDatabase (asset import pipeline, thumbnail generation)<br>3. Fix DockingSystem (ImGui docking branch integration)<br>4. Fix HierarchyPanel (scene graph display + entity selection)<br>5. Fix ConsolePanel (connect to existing console system)<br>6. Fix PerformanceProfiler (frame time graphs, system breakdowns)<br>7. Fix EngineInterface/IPC (editor ↔ engine communication)<br>8. Integration testing for the complete editor workflow |

### 6. Mesh LOD (Level of Detail) System

| | |
|---|---|
| **Description** | Automatic mesh LOD generation and distance-based LOD switching for 3D models. |
| **Why it matters** | Terrain has LOD but meshes don't. FPS levels with many objects (weapons, props, characters) will suffer performance without mesh LOD — essential for open-world or large-map FPS games. |
| **Key files** | `Mesh.cpp/h`, `GraphicsEngine.cpp`, ECS `RenderSystem` |
| **Dependencies** | meshoptimizer (MIT license, industry-standard mesh simplification) |
| **Complexity** | **Medium** |
| **Sub-tasks** | 1. Integrate meshoptimizer for automatic LOD generation at asset import<br>2. LOD storage in mesh asset format (LOD chain per mesh)<br>3. Distance-based LOD selection in render system<br>4. Smooth LOD transitions (crossfade or dithered)<br>5. Per-object LOD bias for important objects (e.g. weapons always highest LOD) |

### 7. Expanded Test Suite

| | |
|---|---|
| **Description** | Increase test coverage from ~13 tests to comprehensive coverage of core engine systems. |
| **Why it matters** | Only MathUtils, ECS World, ObjectPool, and GameMode have tests. Major systems (physics, graphics, audio, scripting, scene management) have zero test coverage. This creates regression risk as the engine grows. |
| **Key files** | `Tests/` directory: add `TestPhysics.cpp`, `TestSceneSerializer.cpp`, `TestAudioEngine.cpp`, `TestInputManager.cpp`, `TestScripting.cpp` |
| **Complexity** | **Medium** |
| **Sub-tasks** | 1. Physics: collision detection, raycast, constraint creation, body types<br>2. Scene: serialize → deserialize round-trip, prefab instantiation<br>3. Audio: source creation, 3D positioning, volume control<br>4. Input: action mapping, key binding, sensitivity<br>5. Scripting: script compilation, function binding, hot-reload<br>6. ECS: complex queries, system ordering, component lifecycle |

### 8. Audio Overhaul — Music System & Sound Groups

| | |
|---|---|
| **Description** | Add a music playback system (crossfading, playlists, dynamic intensity layers), sound categories/mixers (SFX, Voice, Ambient, Music with independent volume), and audio occlusion. |
| **Why it matters** | The current audio system handles basic 2D/3D playback but has no music pipeline, no mixer buses, and no environmental audio effects. FPS games need dynamic music that reacts to combat intensity and sounds that attenuate through walls. |
| **Key files** | `AudioEngine.cpp/h`, new `MusicManager.cpp/h`, `AudioMixer.cpp/h` |
| **Complexity** | **Medium** |
| **Sub-tasks** | 1. Mixer bus system (Master → SFX / Voice / Music / Ambient)<br>2. Music manager with crossfade and playlist support<br>3. Dynamic music layers (low/medium/high combat intensity)<br>4. Audio occlusion via raycasts against level geometry<br>5. Reverb zones (indoor/outdoor, different room sizes) |

### 9. Decal System

| | |
|---|---|
| **Description** | Projected decals for bullet holes, blood splatter, scorch marks, tire tracks, and environmental detail. |
| **Why it matters** | FPS games rely heavily on decals for visual feedback (bullet impacts, explosion marks). This is a high-visibility feature that is standard in every FPS engine. |
| **Key files** | New `DecalSystem.cpp/h`, `GraphicsEngine.cpp` (add deferred decal rendering pass), new HLSL/GLSL decal shaders |
| **Complexity** | **Medium** |
| **Sub-tasks** | 1. Deferred decal projection (screen-space or OBB projection)<br>2. Decal material support (albedo, normal, roughness)<br>3. Decal pooling and fade-out (max decals, oldest fade first)<br>4. Surface-type mapping (different decals for metal/concrete/wood)<br>5. Integration with weapon system (spawn decal at raycast hit point) |

---

## Tier 3: Advanced / Future

Ambitious features for long-term engine growth.

### 10. Ray Tracing Support (DXR)

| | |
|---|---|
| **Description** | Optional DirectX Raytracing (DXR) for reflections, global illumination, ambient occlusion, and shadows. |
| **Why it matters** | RT is becoming standard in modern engines. The existing RHI abstraction layer makes it feasible to add as a new backend. Would position SparkEngine alongside modern AAA technology. |
| **Key files** | New `Graphics/RHI/D3D12/` backend, RT shader pipeline, `GraphicsEngine.cpp` (feature toggle) |
| **Complexity** | **Large** |

### 11. Procedural Generation Framework

| | |
|---|---|
| **Description** | Tools for procedural level/terrain generation: noise functions (Perlin, Simplex, Worley), rule-based object placement, procedural meshes, and wave function collapse for room layouts. |
| **Why it matters** | Enables infinite replayability for FPS maps, randomized dungeons/arenas, and procedural environments. Leverages the existing terrain system. |
| **Key files** | New `Engine/Procedural/`: `NoiseGenerator.cpp/h`, `ProceduralMesh.cpp/h`, `LevelGenerator.cpp/h` |
| **Complexity** | **Large** |

### 12. Cinematic / Cutscene System

| | |
|---|---|
| **Description** | Timeline-based sequencer for camera paths, entity animations, audio cues, and scripted events — for campaign storytelling. |
| **Why it matters** | FPS campaigns need cutscenes, mission briefings, and scripted sequences. The editor's existing animation infrastructure could serve as a UI foundation. |
| **Key files** | New `Sequencer.cpp/h`, integrate with editor, camera system, AngelScript events |
| **Complexity** | **Large** |

### 13. Cross-Platform Input (SDL2)

| | |
|---|---|
| **Description** | Replace the Win32-specific input system with SDL2 for full cross-platform keyboard, mouse, and gamepad support on Linux and macOS. |
| **Why it matters** | Current input handling is Win32-dependent. True cross-platform gameplay requires platform-agnostic input. SDL2 also provides better gamepad support (hot-plug, rumble, gyro). |
| **Key files** | `InputManager.cpp/h`, `GamepadInput.cpp/h`, `CMakeLists.txt` |
| **Complexity** | **Medium** |

---

## Recommended Implementation Order

| Priority | Feature | Tier | Complexity | Impact |
|:--------:|---------|:----:|:----------:|--------|
| 1 | Skeletal Animation System | 1 | Large | Unlocks animated characters — prerequisite for everything else |
| 2 | Complete the Editor | 2 | Medium | Leverages existing half-built work, accelerates all future development |
| 3 | AI & Navigation (NavMesh) | 1 | Large | Enables single-player gameplay with enemy combatants |
| 4 | Save/Load System | 1 | Medium | Essential player expectation, builds on existing serialization |
| 5 | Decal System | 2 | Medium | High visual impact for moderate effort |
| 6 | Expanded Test Suite | 2 | Medium | Prevents regressions as the engine grows |
| 7 | Mesh LOD System | 2 | Medium | Performance for larger scenes |
| 8 | Audio Overhaul | 2 | Medium | Immersion and polish |
| 9 | Networking Foundation | 1 | Large | Multiplayer (tackle when ready for the commitment) |
| 10 | Decal System | 2 | Medium | Visual polish |
| 11 | Ray Tracing (DXR) | 3 | Large | Next-gen graphics |
| 12 | Procedural Generation | 3 | Large | Replayability |
| 13 | Cinematic System | 3 | Large | Storytelling |
| 14 | Cross-Platform Input | 3 | Medium | Broader platform support |
