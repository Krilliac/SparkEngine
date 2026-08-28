# SparkEngine Feature Roadmap

> **Release boundary:** This roadmap is an implementation-planning document, not
> a release record. No versioned SparkEngine release has been published. The only
> declared profile is the blocked and uncertified `stable-v1` Windows 11 x64/MSVC
> v143 + D3D11/Windows NullRHI + C++ module slice; other breadth remains outside it.

This roadmap reflects the current development priorities for SparkEngine. Features are organized by priority tier based on impact and feasibility.

## Tier 1 -- Active Development

These features are actively being worked on or are near completion.

| Feature | Status | Notes |
|---------|--------|-------|
| **Metal Backend** | Experimental | Objective-C++ device/interop/readback paths exist, but macOS is outside `stable-v1` and has no release certification. |
| **VR/AR Framework** | Framework Stub | OpenXR-ready interface designed (`VRSystem.h`), needs SDK integration and runtime initialization. |
| **Mobile Platform** | Working Skeleton | Touch input and battery-aware quality scaling functional. Gesture recognition needs expansion. |
| **D3D12 Stabilization** | Experimental | D3D12, mesh-shader, DXR, and VRS implementation paths exist; parity and release evidence remain incomplete. |
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
| **Machine Learning Integration** | Unreleased implementation inventory — neural texture compression, radiance cache, denoiser, and super-resolution paths exist behind `ENABLE_NEURAL_RENDERING`; outside `stable-v1`. |
| **Deterministic Lockstep Networking** | Alternative to current snapshot-based replication. |


Tier 3 execution is tracked in detail by **Milestone M1**: `docs/plans/tier3-polish-maturity-milestone.md` (7 scoped epics, acceptance tests, perf budgets, owner/estimate, docs gates).

## Historical Implementation Baseline (Unreleased)

Earlier roadmap revisions marked the following source areas as implemented. They
were not shipped as v1.0.0 and do not establish stable-v1 support or certification:

- 6 RHI implementation paths (D3D11, D3D12, Vulkan, OpenGL, experimental Metal Objective-C++ device/interop/readback paths, NullRHI)
- Global Illumination (DDGI, Adaptive Probe Volumes, Hybrid RT)
- GPU-Driven Rendering (compute culling, HiZ occlusion, indirect draws)
- Mesh Shader Pipeline (meshlet clustering, amplification/mesh shaders)
- Virtual Texturing (feedback-driven page streaming)
- DXR 1.1 Ray Tracing (reflections, shadows, AO, GI, denoising)
- Shader Graph (35+ nodes, HLSL generation)
- Visual Scripting (64 node palette entries across 9 categories, compiles to AngelScript)
- HeroEngine-inspired MMO networking (AreaServers, WorldServer, seamless migration)
- 65 `*Panel.h` editor-class inventory; registration, operation coverage, and collaborative editing remain separately gated
- Jolt Physics (vehicles, ragdoll, cloth, destruction)
- 11 in-tree game-module directories with differing prototype maturity
- Accessibility (colorblind modes, subtitles, reduced motion, one-handed input)
- Broad test and CI infrastructure; current counts and gate states come from generated evidence rather than this roadmap snapshot

---

*Last updated: 2026-08-28*
