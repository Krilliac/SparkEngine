# Documentation Coverage Audit

**Last updated:** 2026-04-02
**Type:** Observation
**Status:** Active
**Severity:** Low

## Description

83 wiki pages (excluding _Sidebar.md), 245/246 headers with Doxygen comments, near-complete API coverage. Added 13 new wiki pages covering advanced graphics subsystems, tween system, error handling patterns, and hot-reload overview.

---

## Wiki Coverage (83 pages)

### Statistics
- **Total pages**: 83 (excluding _Sidebar.md)
- All engine subsystems have wiki pages (including Tween System, previously missing)
- All major graphics subsystems documented (Render Graph, GPU Particles, GPU-Driven Rendering, Volumetric Fog, Global Illumination, Virtual Texturing, Water Rendering, Clustered Lighting, Mesh Shaders, Shader Graph)
- Broad topic pages added: Error Handling Patterns, Hot Reload Overview

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

- **245/246 headers** have `@file` + `@brief` (99.6%)
- **Missing**: UISystem.h (1 file, zero Doxygen)
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
| 4 | **Physics solver tuning guide** | Instability in complex scenes | Partially covered in Physics.md |
| 5 | **Migration/upgrade guide** | No formal version upgrade path | Not yet documented |

---

## What's Done Right

- 83 wiki pages covering all major subsystems and advanced graphics
- 99.6% header Doxygen coverage
- Codebase-Statistics.md with comprehensive metrics
- Codebase-Health.md with system maturity status
- Unified error handling and hot-reload documentation
- README badges for all platforms, compilers, and quality tools
- All analysis data lives in wiki (no orphan docs/ files)
- docs/specs/ contains detailed wire format, asset format, and plugin ABI specs
