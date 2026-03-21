# Closed/Proprietary Game Engine Analysis

**Last updated:** 2026-03-21
**Type:** Decision
**Status:** Active

## Description

Analysis of 13 closed/proprietary game engines to extract architectural patterns, innovations, and lessons for SparkEngine. Covers Blizzard, Havok, Frostbite, RAGE, Decima, id Tech, Snowdrop, Fox Engine, Naughty Dog, REDengine, Luminous, Creation Engine, and Source 2.

## Context

SparkEngine has already analyzed ~20 open-source engines (CryEngine, TrinityCore, Godot, O3DE, Bevy, Cocos, PCSX2, etc.) yielding 80+ recommendations. This research targets the highest-performing proprietary technology in the industry — engines behind Overwatch, Battlefield, GTA, Horizon, Doom, The Last of Us, The Witcher, and Counter-Strike 2.

## Details

### Per-Engine Analysis

#### 1. Blizzard Engines (WoW, Overwatch, Diablo IV)
- Fully proprietary per-game engines. WoW engine evolved via "Ship of Theseus" across 10 expansions (2004–2024+).
- Overwatch: Custom engine with TED world editor. Tight control over latency, client-side prediction, authoritative server reconciliation.
- Diablo IV: PBR pipeline.
- Shared next-gen engine initiative announced ~2018 to unify technology across games.
- **Relevance**: Validates SparkEngine's networking architecture (area servers, prediction, reconciliation). Per-game specialization vs. generalization is a cautionary lesson.

#### 2. Havok (Microsoft, Middleware)
- SDK middleware: physics, navigation, cloth as separable modules. C/C++ source for licensees.
- Continuous collision detection (default). Industry-leading constraint solvers. Automatic sleeping of inactive bodies (2x perf vs Unity Physics). Visual debugger with performance heatmaps.
- Deterministic cross-platform simulation — identical output across all targets.
- Renamed Havok AI → Havok Navigation (2024). Discontinued: Havok Destruction, Havok Animation Studio.
- 2025: New indie-friendly pricing (one-time per-title fee).
- **Relevance**: Verify CCD enabled for fast objects. Add physics heatmap to DebugDraw. Validate fixed timestep.

#### 3. Frostbite (EA/DICE)
- C++/C#. Modular subsystem design. Originally built for Battlefield large-scale multiplayer.
- **FrameGraph** — render graph of all passes and resources. Stateless render modules with inputs/outputs as resource handles. WorldRenderer reduced from 15K→5K SLOC.
- Real-time ray tracing (Battlefield V, 2018). Dynamic destruction. Networked water simulation.
- Struggled when forced onto non-FPS games (BioWare called it "an F1 race car").
- Rebranded December 2023; moving away from rigid "one engine" mandate.
- **Relevance**: SparkEngine has RenderGraph but it's dead code. Frostbite's success is the strongest validation to integrate it. Extract render passes into stateless modules.

#### 4. RAGE (Rockstar Games)
- Modular from inception — rendering components updatable independently.
- **Predictive asset streaming** — analyzes player velocity, camera direction, historical movement patterns to preload. Deferred shading. Euphoria procedural animation (NaturalMotion).
- RAGE 9 (GTA VI): mesh shaders, full path tracing, virtual texturing return, async compute queues, procedural generation.
- Scales across 4 orders of magnitude of hardware power.
- **Relevance**: SparkEngine's SeamlessAreaManager lacks velocity/direction prediction. HIGH gap for open-world.

#### 5. Decima (Guerrilla Games / Kojima Productions)
- Evolved from Killzone (linear FPS) to Horizon (open-world RPG). Named by Kojima after Dejima island.
- **Tile-based UI rendering** — SDF-based, thousands of primitives in a single draw call.
- Voxel-based real-time clouds. Checkerboard rendering for 4K upscaling.
- **Pico upscaling** (2025-2026) — rivals DLSS in sharpness and temporal stability.
- Jolt physics integration (open-source). Cross-platform: PS4/PS5/Windows/macOS/iOS/Xbox.
- **Relevance**: SDF UI rendering, voxel clouds for WeatherSystem enhancement.

#### 6. id Tech (id Software / Bethesda)
- Evolved C→C++. Vulkan-exclusive since id Tech 7.
- **Virtual texturing (MegaTexture)** — unique textures per surface, 128x128 tile streaming. Feedback buffer (160x120) drives streaming decisions.
- **Clustered forward rendering** (id Tech 6) — 3072 clusters (16x8x24), 256 lights/decals/cubemaps per cluster.
- **"No main thread, no render thread — all jobs"** (id Tech 7, Doom Eternal).
- **Mesh shaders + full path tracing** (id Tech 8, Doom: The Dark Ages 2025). MegaTexture returned.
- **Relevance**: "All jobs" threading is gold standard — validates parallelizing SparkEngine's serial systems. Clustered lighting dimensions (16x8x24) validated. Virtual texturing for large worlds.

#### 7. Snowdrop (Ubisoft Massive)
- C++, development started 2009. Node-based scripting linking all subsystems.
- **Strongly data-driven** — reduces programming needed for features.
- Tiled light culling. Procedural destruction. Graphics API abstraction (DirectX/Vulkan/console SDKs).
- Fast iteration: new builds in minutes. Small teams creating ambitious AAA.
- Used in: The Division, Avatar: Frontiers of Pandora, Star Wars Outlaws.
- **Relevance**: Data-driven entity/material definitions would reduce SparkEngine iteration time.

#### 8. Fox Engine (Konami / Kojima Productions)
- Cross-platform, cross-generational. Deferred rendering with PBR via photo/laser capture.
- **Multi-threaded physics solvers** distributed across cores for consistent 60fps.
- **Environmental simulation** — sandstorms reduce visibility, muffle sounds, alter friction.
- Photorealistic: GDC 2013 demo indistinguishable from real photos.
- Discontinued after Kojima's departure (2015). Konami switched to UE5.
- **Relevance**: Weather→gameplay integration (SparkEngine's WeatherSystem doesn't affect physics/AI). Multi-threaded physics validates threading need.

#### 9. Naughty Dog Engine
- Custom ECS-like architecture. PlayStation-exclusive optimization.
- **Fiber-based parallelism** — cooperative multitasking, lower overhead than thread pools.
- Frame-centric engine design. Tile-based deferred lighting.
- **GPU-driven particle effects** spawning from world geometry.
- Baked ambient lighting with runtime light optimization for PS4 30fps target.
- **Relevance**: Fiber-based jobs for future JobSystem evolution. GPU particle spawning from geometry.

#### 10. REDengine (CD Projekt Red)
- 4 versions. Flexible renderer (deferred or forward+). Havok physics (v1-2).
- **Clipmap terrain streaming** — 6 clipmaps: elevation, control, color streamed; vertical error, normal, shadow generated at runtime.
- **Film LUT color grading** instead of tone mapping (Cyberpunk 2077).
- Hardware RT with GI, diffuse illumination, AO.
- Cyberpunk 2077 was final REDengine game — switched to UE5.
- **Cautionary**: Modular design caused unexpected inter-module interactions. Integration testing is essential.
- **Relevance**: Clipmap terrain for large worlds. Film LUT as PostProcessing option.

#### 11. Luminous Engine (Square Enix)
- All-in-one dev environment (inspired by Unreal/CryEngine). DirectX 11/12.
- **Ray-bundle GI baking** for fast global illumination.
- GPU-accelerated hair rendering. CG-quality blending with real-time. 100K poly characters, 5M poly/frame.
- Only 2 games shipped (FFXV, Forspoken) before discontinuation.
- **Cautionary**: Massive investment for minimal output. Validates incremental evolution over ground-up rewrites.

#### 12. Creation Engine (Bethesda)
- Forked from Gamebryo. CE2 (Starfield): "largest overhaul since Oblivion." CE3 (TES VI): announced Feb 2026.
- **Modding-first** — Creation Kit is gold standard for moddability.
- Procedural lip-syncing. Quake netcode integration (Fallout 76).
- CE3: scalable performance, advanced data loading, internal stability.
- **Cautionary**: Physics tied to framerate in early versions. Famous bug.
- **Relevance**: Modding architecture reference. Fixed timestep validation.

#### 13. Source 2 (Valve)
- 64-bit, Vulkan/D3D11. Custom Rubikon physics (replaced Havok). Panorama UI (HTML5/CSS/JS-like).
- **Sub-tick networking** (CS2) — input sampling decoupled from server tick rate.
- Unified lighting system. Rebuilt Hammer editor. WebM video (open format).
- VR support (Half-Life: Alyx).
- **Relevance**: Sub-tick networking for competitive FPS. Panorama-style UI for flexibility.

### Cross-Engine Pattern Signals (3+ engines)

| Pattern | Count | Engines | SparkEngine Status |
|---------|-------|---------|-------------------|
| FrameGraph / Render Graph | 4 | Frostbite, id Tech, Decima, Snowdrop | Has but unused |
| Job-based parallelism | 4 | id Tech 7, Naughty Dog, Fox Engine, Frostbite | Has JobSystem, unused |
| PBR | 13 | All | Done |
| Deferred + Forward hybrid | 5 | id Tech, REDengine, Frostbite, ND, Source 2 | Done |
| Predictive asset streaming | 4 | RAGE, Decima, REDengine, id Tech | Missing prediction |
| Fixed timestep physics | 3+ | Havok, Fox Engine, Creation Engine (lesson) | Missing |
| Clustered/tiled light culling | 4 | id Tech, Snowdrop, ND, Frostbite | Done |
| Data-driven design | 4 | Snowdrop, Frostbite, Source 2, Creation Engine | Mostly code-driven |
| Virtual texturing | 3 | id Tech, RAGE, REDengine | Missing |
| Weather → gameplay | 3 | Fox Engine, RAGE, Snowdrop | Weather doesn't affect gameplay |
| GPU-driven rendering | 3 | id Tech 8, Naughty Dog, Decima | Not implemented |

## Summary

### HIGH Priority (5 recommendations)

1. **Integrate existing RenderGraph** — Dead code. Frostbite proved FrameGraph reduces WorldRenderer complexity by 67%. Extract GraphicsEngine's render passes into stateless modules.
2. **Parallelize ECS via JobSystem** — id Tech 7 "all jobs, no main thread" is gold standard. 9 total engines (open + closed) validate this. Wire Physics, Animation, AI into parallel jobs.
3. **Fixed timestep physics** — Havok defaults to it. Creation Engine's framerate-tied physics is an industry cautionary tale. SparkEngine's variable deltaTime is a known bug.
4. **Predictive asset streaming** — RAGE's velocity/direction/history-based prefetching. Extend SeamlessAreaManager to predict what to load before the player gets there.
5. **Sub-tick networking** — Source 2's CS2 innovation. Decouple input sampling from server tick rate for competitive FPS responsiveness.

### MEDIUM Priority (5 recommendations)

6. **Virtual texturing / texture streaming** — id Tech MegaTexture, RAGE streaming, REDengine clipmaps. Essential for large open worlds with unique surfaces.
7. **Weather → gameplay integration** — Fox Engine sandstorms affect visibility, sound, friction. Wire WeatherSystem into physics, AI perception, audio.
8. **Data-driven entity/material definitions** — Snowdrop/Frostbite approach reduces iteration time significantly.
9. **Clipmap terrain streaming** — REDengine's 6-clipmap approach. Enhance TerrainSystem for large worlds.
10. **Physics debugging heatmaps** — Havok's Visual Debugger performance visualization. Extend DebugDraw.

### LOW Priority (5 recommendations)

11. **SDF-based UI rendering** — Decima's batched UI primitives
12. **Film LUT color grading** — REDengine's alternative to tone mapping
13. **Fiber-based job system** — Naughty Dog's cooperative multitasking (future evolution)
14. **GPU-driven particles from geometry** — Naughty Dog TLOU2 approach
15. **Procedural lip-sync** — Creation Engine 2's automated dialogue animation

### Overlap with Prior Recommendations

- **Job system parallelism**: Now validated by 9 engines total (5 open + 4 closed) — strongest signal across all analyses
- **RenderGraph integration**: Frostbite invented FrameGraph — strongest single-engine validation
- **Fixed timestep**: Validated by Havok + Creation Engine cautionary tale
- **Clustered lighting**: Already implemented, validated by 4 more engines (8 total)
- **Predictive streaming**: New insight from RAGE, reinforces SeamlessAreaManager gaps from prior analysis

### Cautionary Tales

- **Luminous Engine**: Massive investment, only 2 games shipped. Don't over-invest in engine vs. shipping games.
- **Frostbite forced adoption**: Failed at non-FPS genres (BioWare). Keep SparkEngine genre-flexible.
- **REDengine inter-module bugs**: Modular design needs integration tests. SparkEngine should add cross-subsystem tests.
- **Fox Engine abandonment**: Even a great engine dies if key people leave. Document architecture well.
- **Creation Engine physics**: Tying physics to framerate is a famous bug. Fixed timestep is non-negotiable.

## Notes

- Sources: Wikipedia, GDC Vault, official engine pages, tech press, developer interviews, community analysis
- Most closed engines reveal architecture through GDC talks, not source code — details are approximate
- Several studios (CDPR, Konami) abandoned proprietary engines for UE5, suggesting the "build vs. buy" calculus is shifting
- SparkEngine's position: already has many of the right systems (RenderGraph, JobSystem, ClusteredLighting, ECS) but several are unintegrated dead code — the #1 priority is wiring in what exists, not building new things
