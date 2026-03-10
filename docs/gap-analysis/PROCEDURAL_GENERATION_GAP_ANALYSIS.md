# SparkEngine Procedural Generation — Gap Analysis

> **Scope**: `SparkEngine/Source/Engine/Procedural/` (NoiseGenerator, HeightmapGenerator, ProceduralMesh, ObjectPlacer, WaveFunctionCollapse)
> **Date**: 2026-03-10
> **Methodology**: Static source-code audit of all `.h`/`.cpp` files under `Procedural/`.
> Each gap is assigned a severity: **Critical**, **Major**, **Moderate**, **Minor**.

---

## Context

The Procedural Generation subsystem provides noise functions (Perlin, Simplex, Worley, FBM, ridged multifractal, domain warping), heightmap generation with erosion simulation, procedural mesh primitives (plane, box, sphere, cylinder, cone, torus, terrain, rock, tree), rule-based object placement, and Wave Function Collapse for room/dungeon layouts. The API is clean and functional with static utility methods. However, the system is entirely standalone with no integration into the ECS, editor, or rendering pipeline, and all spatial types are Windows-only.

---

## Major Gaps

### GAP-PG01 — Platform-Dependent Data Structures Throughout

**Files**:
- `Procedural/ProceduralGeneration.h` (lines 119–122, `ProceduralVertex`; lines 164, 174–178, `PlacementRule`/`PlacementResult`)

**Impact**: `ProceduralVertex` uses `XMFLOAT3` and `XMFLOAT2` for position/normal/texcoord. `PlacementRule` uses `XMFLOAT2` for scale range. `PlacementResult` uses `XMFLOAT3` for position/rotation/scale. All these types are DirectXMath types available only on Windows. The entire procedural generation system fails to compile on Linux/macOS.

**Evidence**: `#include <DirectXMath.h>` at line 19, guarded by `SPARK_PLATFORM_WINDOWS`. The struct definitions using these types (lines 119–178) are outside the guard, causing compile errors on non-Windows.

**What is needed**: Use platform-agnostic math types (e.g., from `Platform.h` stubs) or define local `struct Vector3 { float x, y, z; }` types within the procedural namespace.

---

### GAP-PG02 — No Integration with ECS

**Files**:
- `Procedural/ProceduralGeneration.h` (full file)

**Impact**: Generated meshes (`ProceduralMeshData`) and placement results (`PlacementResult`) are returned as raw data structs. There is no method to create ECS entities from generated content. `ObjectPlacer::PlaceObjects()` returns placement positions but does not create entities in the World. Game code must manually iterate results, create entities, and attach mesh/transform components.

**Evidence**: No `#include` of any ECS headers. No `World&` parameter in any method. `PlacementResult` contains `objectType` (a string) and transform data but no entity creation.

**What is needed**: Add helper methods: `CreateEntitiesFromPlacements(World&, placements, meshMap)` that creates entities with `Transform`, `MeshRenderer`, and `NameComponent` for each placement result. Consider an `ECS/ProceduralSystem` that wraps the generation pipeline.

---

### GAP-PG03 — No Editor Integration

**Files**:
- `Procedural/ProceduralGeneration.h`
- `SparkEditor/Source/` (no Procedural directory)

**Impact**: The SparkEditor has terrain tools (`SparkEditor/Source/Terrain/`) but no procedural generation panel. Users cannot configure noise parameters, preview heightmaps, tweak placement rules, or run WFC layouts from within the editor. All procedural generation must be done via code.

**Evidence**: `SparkEditor/Source/` contains `Terrain/` but no `Procedural/` directory. The `HeightmapSettings` struct has 11 parameters that would benefit from visual sliders and real-time preview.

**What is needed**: Create an ImGui-based procedural generation panel in the editor with parameter sliders, heightmap preview (as a 2D texture or 3D mesh), placement rule editors, and WFC tile configuration.

---

### GAP-PG04 — No Integration with Rendering Pipeline

**Files**:
- `Procedural/ProceduralGeneration.h` (lines 124–128, `ProceduralMeshData`)

**Impact**: `ProceduralMeshData` contains raw vertex/index arrays but has no method to create GPU buffers (vertex buffer, index buffer) or `MeshRenderer` components. Converting procedural mesh data to renderable meshes requires manual D3D11 buffer creation and shader setup that is not encapsulated anywhere.

**Evidence**: `ProceduralMeshData` has `std::vector<ProceduralVertex>` and `std::vector<uint32_t>` — CPU-side data only. No `CreateGPUMesh()`, `ToVertexBuffer()`, or similar method.

**What is needed**: Add a `ProceduralMeshData::CreateRenderMesh(GraphicsEngine&)` method or a utility in the Graphics subsystem that accepts `ProceduralMeshData` and returns a renderable mesh handle.

---

## Moderate Gaps

### GAP-PG05 — WFC Implementation Status Unclear

**Files**:
- `Procedural/ProceduralGeneration.h` (lines 213–241, `WaveFunctionCollapse`)

**Impact**: The Wave Function Collapse class has a clean public API (`AddTile`, `SetGridSize`, `Collapse`, `GetResult`) with internal methods (`FindMinEntropyCell`, `Propagate`, `AreSocketsCompatible`). However, WFC is a complex algorithm and it is unclear from the header alone whether the implementation is complete and correct. The `Collapse()` method returns `bool` for success/contradiction but there is no documentation on failure modes, constraint definitions, or socket format conventions.

**Evidence**: No test file specifically for WFC (the test file `TestNoiseGenerator.cpp` covers noise, not WFC). Socket compatibility uses string matching (`AreSocketsCompatible(string, string)`) but the matching convention (exact match? prefix? complementary pairs?) is undocumented.

**What is needed**: Add documentation for the socket format convention. Write unit tests for WFC with known-good tile sets. Document edge cases (grid too small, conflicting constraints, no valid solution).

---

### GAP-PG06 — No Streaming or Chunked Generation

**Files**:
- `Procedural/ProceduralGeneration.h` (full file)

**Impact**: All generation methods produce the entire result at once. `HeightmapGenerator::Generate()` creates the full heightmap in memory. For large open worlds, this requires generating and storing the entire world before rendering begins. There is no chunked or streaming generation that produces terrain/objects incrementally as the player moves.

**What is needed**: Add a `ChunkedHeightmapGenerator` that generates terrain in fixed-size chunks on demand. Track generated chunks and generate neighbors as the camera approaches chunk boundaries. Combine with LOD for distant chunks.

---

### GAP-PG07 — No Biome System

**Files**:
- `Procedural/ProceduralGeneration.h` (lines 155–171, `PlacementRule`)

**Impact**: `PlacementRule` defines constraints per object type (density, height range, slope range), but there is no biome system that groups placement rules, terrain textures, and noise parameters into coherent environments. Creating a world with forests, deserts, and mountains requires manually defining separate rule sets and blending between them.

**What is needed**: Add a `Biome` struct that groups: noise parameters (frequency, amplitude), terrain texture weights, a set of `PlacementRule`s, and a biome mask noise function. Add `BiomeMixer` that blends between biomes based on temperature/moisture noise maps.

---

### GAP-PG08 — Generated Meshes Have No LOD Integration

**Files**:
- `Procedural/ProceduralGeneration.h` (lines 130–149, `ProceduralMesh` methods)

**Impact**: Procedural meshes are generated at a single level of detail. `CreateSphere(radius, 16, 16)` always produces 16x16 subdivisions. For distant objects, these meshes are over-detailed. The engine has a `MeshLOD` system (tested in `TestMeshLOD.cpp`) but procedural meshes do not integrate with it.

**What is needed**: Add LOD parameters or a `GenerateLODs(meshData, levels)` utility that produces progressively simplified meshes. Register the LOD chain with the engine's `MeshLOD` system.

---

## Minor Gaps

### GAP-PG09 — L-System Tree Generation Described as "Very Simplified"

**Files**:
- `Procedural/ProceduralGeneration.h` (line 148, `CreateTree`)

**Impact**: The `CreateTree()` method is documented as "very simplified L-system." It takes height, branch count, and seed but does not expose L-system grammar rules, iteration depth, branch angle, or leaf generation. The result is likely a basic branching cylinder rather than a realistic tree.

**What is needed**: Expose L-system parameters (axiom, rules, iterations, angle) for configurable tree generation. Add leaf mesh generation. Consider integrating SpeedTree-style billboard LODs for distant trees.

---

### GAP-PG10 — No Undo for Procedural Modifications

**Files**: All procedural files

**Impact**: Running a procedural generation pass (heightmap generation, object placement) produces results that cannot be undone within the editor. If the result is unsatisfactory, the user must re-run with different parameters.

**What is needed**: Integrate with the editor's undo system (if one exists in `SparkEditor/Source/CommandHistory.h`). Snapshot the pre-generation state and allow undo back to it.

---
