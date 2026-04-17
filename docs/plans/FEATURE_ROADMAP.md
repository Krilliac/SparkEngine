# SparkEngine Feature Roadmap

This roadmap reflects the current development priorities for SparkEngine. Features are organized by priority tier based on impact and feasibility.

## Tier 1 -- Active Development

These features are actively being worked on or are near completion.

| Feature | Status | Notes |
|---------|--------|-------|
| **Metal Backend** | In Progress | API layer designed (`MetalDevice.h`), implementation pending. macOS experimental builds use OpenGL or NullRHI. |
| **VR/AR Framework** | Framework Stub | OpenXR-ready interface designed (`VRSystem.h`), needs SDK integration and runtime initialization. |
| **Mobile Platform** | Working Skeleton | Touch input and battery-aware quality scaling functional. Gesture recognition needs expansion. |
| **D3D12 Stabilization** | Experimental | Mesh shaders, DXR, VRS all functional. Moving toward stable status. |
| **Vulkan 1.4 Stabilization** | Experimental | Largest backend by LOC. Dynamic rendering, push descriptors, timeline semaphores. |

## Tier 2 -- Planned

Features planned for future development cycles.

| Feature | Priority | Notes |
|---------|----------|-------|
| **Console Platform Support** | Medium | PlayStation and Xbox platform layers. Requires NDAs and dev kits. |
| **Asset Marketplace** | Medium | Infrastructure for sharing and distributing game assets and plugins. |
| **Full Mobile Parity** | Medium | Complete iOS/Android platform layer with build pipeline. |
| **Advanced Audio DSP** | Low | Reverb zones, EQ, compressor, procedural audio effects. |
| **Niagara-style VFX** | Low | Procedural GPU particle graph system beyond current GPU particles. |

## Tier 3 -- Under Consideration

Features being evaluated but not yet committed to.

| Feature | Notes |
|---------|-------|
| **WebGPU Backend** | Browser-based rendering via WebGPU API. |
| **Additional VR Runtimes** | Beyond OpenXR (native Oculus, SteamVR). |
| **Machine Learning Integration** | **Shipped** (v1.0.0) — Neural texture compression, radiance cache, denoiser, super-resolution. See `ENABLE_NEURAL_RENDERING`. |
| **Deterministic Lockstep Networking** | Alternative to current snapshot-based replication. |


Tier 3 execution is tracked in detail by **Milestone M1**: `docs/plans/tier3-polish-maturity-milestone.md` (7 scoped epics, acceptance tests, perf budgets, owner/estimate, docs gates).

## Completed (v1.0.0)

Major features shipped in the initial release:

- 6 RHI backends (D3D11, D3D12, Vulkan, OpenGL, Metal header, NullRHI)
- Global Illumination (DDGI, Adaptive Probe Volumes, Hybrid RT)
- GPU-Driven Rendering (compute culling, HiZ occlusion, indirect draws)
- Mesh Shader Pipeline (meshlet clustering, amplification/mesh shaders)
- Virtual Texturing (feedback-driven page streaming)
- DXR 1.1 Ray Tracing (reflections, shadows, AO, GI, denoising)
- Shader Graph (35+ nodes, HLSL generation)
- Visual Scripting (60 node types, compiles to AngelScript)
- HeroEngine-inspired MMO networking (AreaServers, WorldServer, seamless migration)
- 59-panel ImGui editor with collaborative multi-user editing
- Jolt Physics (vehicles, ragdoll, cloth, destruction)
- 10 game module templates (FPS, MMO, RPG, ARPG, RTS, Racing, Platformer, OpenWorld, VisualScript, Base)
- Accessibility (colorblind modes, subtitles, reduced motion, one-handed input)
- 4290 unit tests across 338 files, 5 sanitizer CI jobs, code coverage

---

*Last updated: 2026-04-06*
