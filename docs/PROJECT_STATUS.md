# SparkEngine — Project Status

**Last Updated:** 2026-03-03
**Codebase:** 172 engine source files, ~80 editor source files, 11 test files, 23 shaders

---

## System Status

| System | Status | Notes |
|---|---|---|
| Graphics / Rendering (D3D11) | **Working** | Full deferred/forward+ pipeline |
| Post-Processing | **Working** | Bloom (multi-pass), tone mapping (Reinhard/ACES/Uncharted 2), color grading, FXAA 3.11 |
| Shadow Mapping | **Working** | PCF, VSM, CSM, PCSS |
| Material System (PBR) | **Working** | Physically-based rendering with metallic/roughness workflow |
| IBL Lighting | **Working** | Irradiance map, prefiltered environment, BRDF LUT |
| Physics (Bullet) | **Working** | Rigid bodies, collision shapes, constraints, raycasting, overlap tests |
| Particle System | **Working** | GPU-accelerated particle effects |
| Decal System | **Working** | Projected decals for bullet impacts, scorch marks |
| Mesh LOD | **Working** | Distance-based level-of-detail switching |
| Audio (XAudio2) | **Working** | 3D spatial audio, distance attenuation, Doppler |
| ECS (EnTT) | **Working** | Component-based architecture with rich component library |
| Input Manager | **Working** | Keyboard, mouse, gamepad with release events |
| Camera System | **Working** | First-person with smooth mouselook |
| Scene Manager | **Working** | JSON serialization, prefab system |
| Player Controller | **Working** | FPS movement, jump, crouch, zoom |
| Weapon System | **Working** | Bullet, rocket, grenade projectiles |
| Animation System | **Working** | Skeletal animation support |
| AI / NavMesh | **Working** | Binary `.snav` loading, pathfinding |
| Save / Load | **Working** | Game state persistence |
| Asset Pipeline | **Working** | OBJ mesh, TGA texture, WAV audio loading with fallback placeholders |
| Shader Cross-Compilation | **Partial** | Basic HLSL-to-GLSL translation; SPIR-V needs DXC/glslang |
| Editor (ImGui) | **Partial** | Dead code removed; docking, theme, asset import still have TODOs |
| Networking | **Disabled** | CURL dependency issues; needs ENet or GameNetworkingSockets replacement |
| DXR / Ray Tracing | **Stub** | Requires D3D12 backend which doesn't exist yet |
| Vulkan Backend | **Untested** | RHI abstraction in place; needs real SPIR-V compilation |
| OpenGL Backend | **Untested** | Needs GLAD loader + glslang integration |
| Test Coverage | **Minimal** | 11 test files covering MathUtils, ECS World, ObjectPool, GameMode |

---

## Recent Changes

### Systems Activated

- **Physics System** — Complete Bullet Physics rewrite: `btDiscreteDynamicsWorld` initialization, `btRigidBody` creation with all collision shape types, force/impulse/torque, constraints, raycasting, overlap tests.
- **Post-Processing** — Full 1,495-line implementation: multi-pass bloom (brightness extract, 5-level downsample, Gaussian blur, upsample composite), three tone mapping operators, color grading, FXAA 3.11 with inline HLSL compilation.
- **IBL Lighting** — `GenerateIrradianceMap()` (32x32 HDR cubemap), `GeneratePrefilterMap()` (128x128, 5 mip levels), `GenerateBRDFLUT()` (256x256, 1024 samples/texel).
- **Asset Pipeline** — Real file loading: OBJ parser (vertex/normal/UV + bounding box), TGA loader (24/32-bit), WAV loader (PCM/RIFF). Falls back to procedural placeholders when files are missing.
- **Input System** — `WasGamepadButtonReleased()` method added; `ActionTrigger::Released` wired for gamepad buttons.
- **NavMesh** — Binary `.snav` deserialization (vertices, triangles, adjacency).

### Cleanup

- 8 disabled editor files deleted (4,319 lines removed)
- Crash handler placeholder URL cleared
- MaterialSystem placeholder metric replaced
- DXR `Initialize()` correctly reports unavailable
- Network manager player name parsing implemented
- Duplicate editor stubs removed, `@file` comments corrected

### Infrastructure (This Pass)

- All 15 git submodules updated to latest upstream commits
- AngelScript submodule path mismatch fixed in `.gitmodules`
- Dependabot configured for weekly GitHub Actions and submodule updates
- README and documentation overhauled

---

## Dependencies (Current Versions)

All managed as git submodules. Updated to latest upstream as of 2026-03-03.

| Library | Version | Status |
|---|---|---|
| Dear ImGui | ~v1.92.6 (HEAD) | Latest |
| ImGuizmo | HEAD (past 1.83 tag) | Latest |
| imnodes | v0.5 | Latest |
| EnTT | ~v3.16.0 (HEAD) | Latest |
| DirectXTK | ~oct2025 (HEAD) | Latest |
| Bullet Physics | ~3.25 (HEAD) | Latest |
| Assimp | ~v6.0.4 (HEAD) | Latest |
| miniaudio | ~0.11.24 (HEAD) | Latest |
| curl | ~8.19.0-rc (HEAD) | Latest |
| GLM | ~1.0.3 (HEAD) | Latest |
| miniz | ~3.1.1 (HEAD) | Latest |
| RapidJSON | v1.1.0 (HEAD) | Only release; active master |
| spdlog | ~v1.17.0 (HEAD) | Latest |
| stb | HEAD (no tags) | Latest |
| AngelScript Mirror | HEAD | Archived upstream; official repo moved to [anjo76/angelscript](https://github.com/anjo76/angelscript) |

---

## Known Issues

- **AngelScript submodule** uses the archived `codecat/angelscript-mirror`. The official AngelScript project (v2.38.0) has moved to [anjo76/angelscript](https://github.com/anjo76/angelscript). Migration recommended.
- **GLAD** (OpenGL loader) is not included as a submodule. OpenGL backend requires manual GLAD download.
- **Networking** is disabled due to CURL dependency issues on Windows. Replace with ENet or GameNetworkingSockets for UDP-based game networking.
- **10 large header-only editor systems** need build verification on all platforms.

---

## Remaining Work

### Editor TODOs

- DockingSystem: tab colors, panel refresh/save/reset
- EditorTheme: live customization UI, JSON export/import
- EditorUI: recovery dialog, layout import/export
- AssetBrowserPanel: asset import logic

### Infrastructure

- Migrate AngelScript submodule to official repo
- Add GLAD as a submodule for OpenGL support
- Expand test coverage beyond current 11 test files
- Add Linux CI job to build workflow
