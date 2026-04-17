# HLOD and World Partition

Hierarchical Level of Detail (HLOD) generation and world partition grid for efficient rendering and streaming of large open worlds.

**Source:** `SparkEngine/Source/Engine/HLOD/HLODSystem.h`

## Overview

The HLOD system addresses two core problems in large-world rendering: draw call reduction at distance and memory management through spatial streaming. It combines hierarchical LOD generation (merging nearby entities into simplified proxy meshes) with a uniform world partition grid that loads and unloads cells based on viewer distance.

At build time, `HLODBuilder` groups entities into spatial clusters using grid-based binning. Each cluster gets multiple LOD representations: LOD0 (original meshes), LOD1 (merged geometry with reduced draw calls), LOD2 (decimated proxy mesh), and LOD3 (billboard imposters). The system selects the appropriate LOD level based on the cluster's screen-space size.

At runtime, `WorldPartitionGrid` divides the world into uniform cells. As the viewer moves, cells within the load radius are streamed in and cells beyond the unload radius are released. The hysteresis between load and unload radii prevents thrashing at cell boundaries.

## Architecture

```
HLODSystem (singleton)
  |
  +-- HLODBuilder (build-time)
  |     +-- EntityEntry[] --> BuildClusters() --> HLODCluster[]
  |     +-- GenerateProxy(cluster, targetTris) --> HLODProxy
  |     +-- GenerateImposter(cluster) --> HLODProxy (billboard)
  |
  +-- WorldPartitionGrid (runtime)
  |     +-- WorldBounds + cellSize --> uniform grid
  |     +-- AssignEntity(id, pos) --> place in cell
  |     +-- UpdateStreaming(viewerPos, loadR, unloadR)
  |           --> load nearby cells, unload distant cells
  |
  +-- LOD Selection
        LOD0 (full detail) --> screen size > 10%
        LOD1 (merged)      --> screen size > 3%
        LOD2 (proxy)        --> screen size > 1%
        LOD3 (imposter)     --> screen size < 1%
```

## Key Classes

| Class | Description |
|-------|-------------|
| `HLODSystem` | Singleton managing HLOD generation and world-partition streaming |
| `HLODBuilder` | Build-time tool for generating clusters and proxy meshes |
| `HLODCluster` | Group of spatially-close entities with per-LOD triangle counts |
| `HLODProxy` | Simplified mesh data representing a cluster at a specific LOD |
| `WorldPartitionGrid` | Uniform grid that partitions the world into streamable cells |
| `WorldPartitionCell` | Single cell with entities, load state, and priority |
| `HLODBuildSettings` | Configuration for cluster size, triangle targets, and thresholds |

## Usage

### Building HLOD Clusters

```cpp
auto& hlod = Spark::HLOD::HLODSystem::GetInstance();
hlod.Initialize();

// Populate entity data
Spark::HLOD::HLODBuilder& builder = hlod.GetBuilder();
std::vector<Spark::HLOD::HLODBuilder::EntityEntry> entities = {
    {1, {100, 0, 200}, 5.0f, 1000},
    {2, {110, 0, 205}, 3.0f, 800},
    {3, {500, 0, 500}, 10.0f, 2000},
};

Spark::HLOD::HLODBuildSettings settings;
settings.clusterSize = 128.0f;
settings.lod1TargetTriangles = 5000;
settings.lod2TargetTriangles = 500;
builder.BuildClusters(entities, settings);
hlod.BuildHLOD(settings);
```

### World Partition Streaming

```cpp
auto& grid = hlod.GetGrid();
grid.Initialize({{-2048, 0, -2048}, {2048, 512, 2048}}, 256.0f);

// Assign entities to cells
grid.AssignEntity(1, {100, 0, 200});
grid.AssignEntity(2, {110, 0, 205});

// Per-frame: update streaming based on camera position
hlod.Update(cameraPosition);

// Get loaded cells for rendering
for (const auto* cell : grid.GetLoadedCells())
{
    // Render entities in cell
}
```

## API Reference

### HLODSystem

| Method | Description |
|--------|-------------|
| `Initialize()` | Set up with default radii (load=1000, unload=1500, view=2000) |
| `Update(viewerPos)` | Advance streaming and LOD selection |
| `BuildHLOD(settings)` | Generate HLOD clusters from builder data |
| `GetVisibleClusters(pos, dist)` | Query clusters within view distance |
| `SetLoadRadius(r)` | Set cell streaming load radius |
| `SetUnloadRadius(r)` | Set cell streaming unload radius |

### WorldPartitionGrid

| Method | Description |
|--------|-------------|
| `Initialize(bounds, cellSize)` | Create the grid over world bounds |
| `AssignEntity(id, pos)` | Place an entity in the appropriate cell |
| `RemoveEntity(id)` | Remove an entity from its cell |
| `UpdateStreaming(pos, loadR, unloadR)` | Load/unload cells by distance |
| `GetLoadedCells()` | Get all currently loaded cells |
| `GetCellAt(pos)` | Look up the cell at a world position |

### HLODBuilder

| Method | Description |
|--------|-------------|
| `BuildClusters(entities, settings)` | Group entities into spatial clusters |
| `GenerateProxy(cluster, targetTris)` | Create a simplified proxy mesh |
| `GenerateImposter(cluster)` | Create a billboard proxy |

## Configuration

| Setting | Default | Description |
|---------|---------|-------------|
| `clusterSize` | 128.0 | Grid cell size for HLOD clustering (world units) |
| `lod1TargetTriangles` | 5000 | Triangle budget for LOD1 merged meshes |
| `lod2TargetTriangles` | 500 | Triangle budget for LOD2 proxy meshes |
| `imposterResolution` | 256 | Texture resolution for billboard imposters |
| `imposterDirections` | 8 | Number of capture directions for imposters |
| `screenSizeThresholdLOD1` | 0.10 | Screen fraction threshold for LOD1 |
| `screenSizeThresholdLOD2` | 0.03 | Screen fraction threshold for LOD2 |
| `screenSizeThresholdLOD3` | 0.01 | Screen fraction threshold for LOD3 |
| Load radius | 1000.0 | Distance to begin streaming cells |
| Unload radius | 1500.0 | Distance to release streamed cells |

## Related Systems

- [Seamless Area Manager](Seamless-Area-Streaming) -- Higher-level area streaming that works alongside world partition
- [World Origin System](World-Origin-System) -- Floating-point origin rebasing for large worlds
- [Render Graph](../graphics/Render-Graph.md) -- Manages render passes including HLOD proxy rendering
