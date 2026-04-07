# Documentation Coverage Audit

**Last updated:** 2026-04-06
**Type:** Observation
**Status:** Active
**Severity:** Low

## Description

125 wiki pages (excluding _Sidebar.md), 246/246 headers with Doxygen comments, complete API coverage. Added 8 new graphics/rendering wiki pages: Post-Processing, Neural Rendering, Material System, Decal System, Particle System, Sky and Atmosphere, Shadow System, Foliage System. Fixed stale counts across README, PROJECT_STATUS, FEATURE_ROADMAP, CHANGELOG, and editor references (57→59 panels, test counts updated to 4290).

---

## Wiki Coverage (125 pages)

### Statistics
- **Total pages**: 125 (excluding _Sidebar.md)
- All engine subsystems have wiki pages
- All graphics subsystems documented (Render Graph, GPU Particles, GPU-Driven Rendering, Volumetric Fog, Global Illumination, Virtual Texturing, Water Rendering, Clustered Lighting, Mesh Shaders, Shader Graph, Post-Processing, Neural Rendering, Material System, Decal System, Particle System, Sky and Atmosphere, Shadow System, Foliage System)
- Broad topic pages: Error Handling Patterns, Hot Reload Overview
- User-facing docs: FAQ, Quick-Start Tutorial, Editor Walkthrough, Configuration Reference, Performance Tips

### New Pages Added (2026-04-06)

| Page | Category | Coverage |
|------|----------|----------|
| `Post-Processing.md` | Graphics | 14-pass pipeline, per-effect settings, performance metrics |
| `Neural-Rendering.md` | Graphics | Inference engine, radiance cache, NTC, neural denoiser/SR |
| `Material-System.md` | Graphics | PBR metallic/roughness, 18 texture slots, advanced properties |
| `Decal-System.md` | Graphics | Deferred decals, surface types, weapon integration |
| `Particle-System.md` | Graphics | CPU particles, emitter shapes, blend modes |
| `Sky-and-Atmosphere.md` | Graphics | Preetham sky model, turbidity, sun direction |
| `Shadow-System.md` | Graphics | Shadow atlas, PCSS, temporal caching |
| `Foliage-System.md` | Graphics | Instanced vegetation, terrain-aware placement |

### New Pages Added (2026-04-03)

| Page | Category | Coverage |
|------|----------|----------|
| `FAQ.md` | Getting Started | Common questions: general, building, editor, gameplay, graphics, physics, audio, modding |
| `Quick-Start-Tutorial.md` | Getting Started | First 10 minutes: launch, console, editor, spawn objects, adjust world, graphics |
| `Editor-Walkthrough.md` | Getting Started | Practical hands-on editor guide: navigation, panels, workflows, play mode, shortcuts |
| `Configuration-Reference.md` | Advanced | Complete settings.ini reference, 150+ console commands, CVar system |
| `Performance-Tips.md` | Advanced | Optimization for rendering, physics, audio, networking, scripting, animation |

### New Pages Added (2026-04-02)

| Page | Category | Coverage |
|------|----------|----------|
| `Tween-System.md` | Engine Subsystems | Handle-based tween system with sequencing, composition, easing |
| `Render-Graph.md` | Graphics | Declarative render pipeline, StandardPipelineBuilder, transient resources |
| `Shader-Graph.md` | Graphics | Node-based material authoring, 35+ node types, HLSL compilation |
| `GPU-Particles.md` | Graphics | D3D11 compute particle system, 1M particles, indirect draw |
| `GPU-Driven-Rendering.md` | Graphics | Compute culling, HiZ occlusion, indirect rendering |
| `Volumetric-Fog.md` | Graphics | Froxel-based fog, Henyey-Greenstein, Beer-Lambert |
| `Global-Illumination.md` | Graphics | DDGI probes + Adaptive Probe Volumes, L2 SH |
| `Virtual-Texturing.md` | Graphics | Feedback-driven page streaming, LRU cache |
| `Water-Rendering.md` | Graphics | Gerstner wave simulation, height queries |
| `Clustered-Lighting.md` | Graphics | 3D frustum grid light culling |
| `Mesh-Shaders.md` | Graphics | Meshlet rendering, amplification shaders, D3D12 SM6.5 |
| `Error-Handling-Patterns.md` | Advanced | Unified error handling reference across subsystems |
| `Hot-Reload-Overview.md` | Advanced | Shader, script, module, material, asset hot-reload |

### Sidebar Updated
- "Graphics Backends" renamed to "Graphics" with 10 new entries
- "Engine Subsystems" gained Tween System
- "Advanced" gained Error Handling Patterns and Hot Reload Overview

---

## API Documentation (Doxygen)

- **246/246 headers** have `@file` + `@brief` (100%)
- UISystem.h Doxygen tags converted from `\` to `@` style to match codebase convention
- **Auto-generated API docs**: `docs/api/` generated (370 pages from 382 headers)

---

## docs/ Directory (current)

```
docs/
├── README.md
├── Doxyfile.txt
├── auto-update.sh
├── generate-api-docs.sh
├── generate-docs.sh
├── generate-flowchart.sh
├── generate-flowchart-content.py
├── sync-wiki.sh
├── plans/
│   └── hardware-acceleration-plan.md
└── specs/
    ├── asset-format.md
    ├── networking-wire-format.md
    └── plugin-abi-guide.md
```

---

## Top Documentation Gaps (remaining)

| Priority | Gap | Impact | Status |
|----------|-----|--------|--------|
| 1 | ~~Networking protocol wire format~~ | ~~Can't build external clients~~ | **Resolved** (wiki page exists) |
| 2 | ~~Asset format specifications~~ | ~~Can't build external tools~~ | **Resolved** (wiki page exists) |
| 3 | ~~Plugin ABI stability & versioning~~ | ~~Breaking module changes~~ | **Resolved** (wiki page exists) |
| 4 | ~~Physics solver tuning guide~~ | ~~Instability in complex scenes~~ | **Resolved** (Solver Tuning section added to Physics.md) |
| 5 | ~~Migration/upgrade guide~~ | ~~No formal version upgrade path~~ | **Resolved** (wiki/Migration-Guide.md created) |

---

## What's Done Right

- 125 wiki pages covering all major subsystems, graphics, and rendering
- 100% header Doxygen coverage (246/246)
- Codebase-Statistics.md with comprehensive metrics
- Codebase-Health.md with system maturity status
- Unified error handling and hot-reload documentation
- README badges for all platforms, compilers, and quality tools
- All analysis data lives in wiki (no orphan docs/ files)
- docs/specs/ contains detailed wire format, asset format, and plugin ABI specs
