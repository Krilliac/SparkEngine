# Terrain and Procedural Generation

SparkEngine includes heightmap terrain rendering and a procedural generation toolkit for creating game content at runtime.

**Source:** `SparkEngine/Source/Engine/ECS/Systems/TerrainSystem.h`, `SparkEngine/Source/Graphics/TerrainRenderer.h`, `SparkEngine/Source/Engine/ECS/Components/TerrainComponents.h`

## Heightmap Terrain

Terrain and procedural generation are compiled as part of the engine; they do not have separate CMake toggles.

The terrain system renders large outdoor environments using heightmaps:

- **Quadtree LOD** — Automatic level-of-detail based on camera distance
- **Texture Splatting** — Multi-texture blending based on height, slope, or paint masks
- **Heightfield Collision** — [Jolt Physics](../subsystems/Physics.md) `Heightfield` shape for terrain collisions
- **Chunk-based** — Terrain is divided into chunks for efficient culling and streaming

## Noise Functions

The procedural generation system provides multiple noise algorithms:

| Function | Description |
|----------|-------------|
| **Perlin** | Classic gradient noise, smooth and continuous |
| **Simplex** | Faster alternative to Perlin with fewer artifacts |
| **Worley** | Cell/Voronoi noise for organic patterns |
| **FBM** | Fractal Brownian Motion — layered octaves for natural detail |
| **Ridged Multifractal** | Ridge-like noise for mountain ranges |
| **Domain Warping** | Distorts input coordinates for flowing, organic shapes |

## Erosion Simulation

Noise-generated terrain can be refined with erosion:

### Thermal Erosion

Simulates material sliding downhill when slope exceeds a threshold:
- Talus angle parameter
- Iteration count
- Material transport rate

### Hydraulic Erosion

Simulates water-based erosion with sediment transport:
- Rain amount and evaporation rate
- Sediment capacity
- Erosion and deposition rates
- Droplet lifetime and inertia

## Procedural Mesh Generation

Generate primitive meshes at runtime:

| Shape | Description |
|-------|-------------|
| Plane | Flat grid with configurable subdivisions |
| Box | Cube with per-face normals |
| Sphere | UV sphere or icosphere |
| Cylinder | Cylinder with configurable segments |
| Cone | Cone with base cap |
| Torus | Donut shape with configurable radii |
| Terrain | Heightmap-based terrain mesh |
| Rock | Randomized rock shapes using noise displacement |
| Tree | Simple L-system or billboard tree geometry |

## Rule-Based Object Placement

Scatter objects across terrain using configurable rules:
- Height range constraints
- Slope angle constraints
- Density and spacing parameters
- Random rotation and scale variation
- Exclusion zones

## Wave Function Collapse (WFC)

Procedural room and dungeon layout generation using the WFC algorithm:

- Define tile types with adjacency rules
- Generate valid layouts that satisfy all constraints
- Configurable grid size and tile sets
- Useful for dungeon crawlers, roguelikes, and level generation

## Heightmap Import/Export Formats

The terrain system supports multiple heightmap formats for interoperability with external tools (World Machine, Gaea, L3DT):

| Format | Extension | Bit Depth | Notes |
|--------|-----------|-----------|-------|
| Raw Binary | `.raw`, `.r16` | 8-bit or 16-bit unsigned | No header; dimensions must be specified manually |
| PNG Grayscale | `.png` | 8-bit or 16-bit | Lossless; widely supported by image editors |
| OpenEXR | `.exr` | 32-bit float | Full floating-point precision; ideal for import from Gaea/World Machine |
| Terrain Archive | `.sparkterrain` | 32-bit float | SparkEngine native format with embedded metadata (dimensions, scale, splatmap references) |

### Import API

```cpp
TerrainImporter importer;
importer.SetDimensions(1025, 1025);          // Required for headerless formats
importer.SetHeightRange(0.0f, 500.0f);       // Remap to world-space height
importer.SetFlipVertical(true);              // Some tools export upside-down
auto heightmap = importer.LoadFromFile("Heightmaps/terrain01.r16");
```

### Export API

```cpp
TerrainExporter exporter;
exporter.SetFormat(HeightmapFormat::OpenEXR);
exporter.SetNormalize(true);                 // Remap values to 0..1 range
exporter.Export(terrain, "Exported/terrain01.exr");
```

## Terrain Chunk Streaming

Terrain is divided into square chunks (default 64x64 vertices each). Chunks are loaded and unloaded based on camera distance to support large worlds without exhausting memory.

### Streaming Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `ChunkSize` | 64 | Vertices per chunk side (must be power-of-two + 1) |
| `LoadRadius` | 8 chunks | Distance in chunks at which terrain data is loaded into memory |
| `UnloadRadius` | 12 chunks | Distance at which chunk data is evicted from memory |
| `StreamingBudgetPerFrame` | 2 | Maximum chunks loaded or unloaded per frame to avoid hitches |
| `PriorityMode` | `DistanceBased` | Chunks closest to camera load first; alternatives: `DirectionBased` (favor view direction) |

### Streaming Lifecycle

1. **Request** -- Camera movement triggers a scan of chunks entering the `LoadRadius`.
2. **Load** -- Heightmap data is read from disk (or generated procedurally) on a background thread.
3. **Build** -- Vertex/index buffers and collision shapes are created on the main thread.
4. **Active** -- Chunk is rendered and participates in physics queries.
5. **Evict** -- When the chunk leaves `UnloadRadius`, GPU resources are released and data is optionally cached to a memory-mapped file.

## Texture Splatmap System

Splatmaps control how terrain textures blend across the surface. Each splatmap is an RGBA texture where each channel stores the weight for one terrain layer.

### Weight Map Layout

| Channel | Default Layer |
|---------|--------------|
| R | Grass |
| G | Rock |
| B | Dirt |
| A | Sand |

Additional splatmaps can be stacked to support more than four layers. Each splatmap adds four more layers (splatmap 2 covers layers 5--8, etc.). The engine supports up to **16 terrain layers** (4 splatmaps).

### Weight Normalization

Weights across all channels at every texel are normalized to sum to 1.0. The terrain shader performs this normalization at sample time:

```hlsl
float4 weights = splatmapTexture.Sample(splatSampler, uv);
weights /= dot(weights, float4(1, 1, 1, 1));   // Normalize
```

### Painting and Auto-Generation

- **Manual painting** -- The [SparkEditor](SparkEditor.md) terrain brush tool writes directly to splatmap texels with configurable brush size, falloff, and opacity.
- **Auto-splatting** -- Rules based on height and slope automatically assign layers:
  - Below height threshold -> Sand
  - Above slope threshold -> Rock
  - High elevation -> Snow (if configured)
  - Default -> Grass

## Terrain Collision Detail Levels

The Jolt Physics heightfield shape can use a lower-resolution heightmap than the visual mesh to save CPU:

| Collision LOD | Resolution Ratio | Use Case |
|---------------|-----------------|----------|
| Full | 1:1 with visual | Player collision, precise projectile raycasts |
| Half | 1:2 | Distant AI navigation, vehicle physics |
| Quarter | 1:4 | Broad-phase queries, very distant chunks |

Set per-chunk collision LOD via:

```cpp
terrain.SetCollisionLOD(chunkIndex, TerrainCollisionLOD::Half);
```

The collision LOD can also be driven automatically by distance from the camera using `terrain.SetAutoCollisionLOD(true)`.

## Quadtree LOD Switching Distances

The terrain quadtree subdivides chunks into smaller patches at closer distances. Each LOD level halves the triangle count.

| LOD Level | Triangles (64x64 chunk) | Default Distance Threshold |
|-----------|------------------------|---------------------------|
| 0 (highest) | 8192 | 0 -- 100 m |
| 1 | 2048 | 100 -- 250 m |
| 2 | 512 | 250 -- 600 m |
| 3 | 128 | 600 -- 1500 m |
| 4 (lowest) | 32 | 1500 m+ |

Customize thresholds with:

```cpp
terrain.SetLODDistance(0, 150.0f);   // Override LOD 0 -> 1 boundary
terrain.SetLODBias(1.5f);           // Global multiplier on all thresholds
```

**Crack prevention:** Adjacent patches at different LOD levels use T-junction stitching strips to prevent visible cracks along chunk boundaries.

## Procedural Mesh Vertex Format

All procedurally generated meshes share a standard vertex format:

```cpp
struct ProceduralVertex
{
    float3 Position;   // Object-space position
    float3 Normal;     // Normalized surface normal
    float4 Tangent;    // Tangent vector (xyz) + bitangent sign (w)
    float2 TexCoord0;  // Primary UV coordinates
    float2 TexCoord1;  // Secondary UV (lightmap or detail)
    float4 Color;      // Per-vertex color (RGBA, default white)
};
```

Total stride: **64 bytes** per vertex. The vertex layout is compatible with the standard PBR material shader and supports normal mapping via the tangent/bitangent basis.

## Noise Parameter Tuning Guide

Selecting noise parameters has a large impact on terrain character. Here is a recommended starting point and tuning guide:

| Parameter | Recommended Range | Effect |
|-----------|-------------------|--------|
| `Octaves` | 4 -- 8 | More octaves add fine detail; diminishing returns above 8 |
| `Frequency` | 0.001 -- 0.01 | Lower = broader features; higher = more jagged terrain |
| `Lacunarity` | 1.8 -- 2.5 | Controls frequency multiplier between octaves; 2.0 is standard |
| `Persistence` | 0.4 -- 0.6 | Controls amplitude decay between octaves; higher = more rough detail |
| `Amplitude` | 50 -- 500 | World-space height range of the noise output |
| `Seed` | any uint32 | Deterministic; same seed always produces same output |

### Domain Warping Tips

- Apply FBM noise as the warp offset for the most natural results.
- Use a warp strength of 50--200 world units for gentle flowing hills; above 400 creates alien-looking landscapes.
- Chain two warp passes for more complex distortion (warp the warp coordinates themselves).

## Erosion Algorithm Step-by-Step

### Hydraulic Erosion (Droplet-Based)

The engine implements the particle-based hydraulic erosion algorithm:

1. **Spawn droplet** at a random position on the heightmap with initial water volume `W` and zero sediment `S`.
2. **Compute gradient** using bilinear interpolation of the four neighboring height samples. This determines the downhill direction.
3. **Update velocity** by blending the previous direction with the gradient direction, weighted by the `Inertia` parameter (0 = pure gradient, 1 = pure inertia).
4. **Move droplet** one step in the computed direction.
5. **Compute height difference** `deltaH` between old and new positions.
6. **If moving uphill** (`deltaH > 0`): deposit min(`deltaH`, `S`) sediment at the old position to fill the pit.
7. **If moving downhill** (`deltaH < 0`): compute sediment capacity `C = max(-deltaH, MinSlope) * Speed * W * SedimentCapacity`. If carrying more than capacity, deposit `(S - C) * DepositionRate`; otherwise erode `(C - S) * ErosionRate` from a radius around the old position.
8. **Evaporate** water: `W *= (1 - EvaporationRate)`.
9. **Repeat** from step 2 until water is exhausted or `MaxLifetime` steps reached.
10. **Iterate** for `NumDroplets` (typically 50,000--200,000 for a 1025x1025 map).

### Thermal Erosion

1. For each cell, compute slope to all 8 neighbors.
2. If any slope exceeds `TalusAngle`, transfer `TransportRate * (slope - TalusAngle)` height to that neighbor.
3. Repeat for `Iterations` passes (typically 20--50).

## WFC Tile Definition Format

Tiles for Wave Function Collapse are defined in JSON:

```json
{
    "tileset": "dungeon_basic",
    "tileSize": [4, 4, 4],
    "tiles": [
        {
            "id": "corridor_ns",
            "mesh": "Assets/Tiles/corridor_ns.fbx",
            "weight": 1.0,
            "sockets": {
                "+x": "wall",
                "-x": "wall",
                "+z": "open_door",
                "-z": "open_door",
                "+y": "floor",
                "-y": "ceiling"
            },
            "tags": ["corridor"],
            "rotatable": true,
            "rotations": [0, 90, 180, 270]
        }
    ],
    "adjacency": {
        "open_door": ["open_door"],
        "wall": ["wall"],
        "floor": ["floor"],
        "ceiling": ["ceiling"]
    }
}
```

**Socket matching:** Two tiles can be placed adjacent if and only if their facing sockets appear together in the `adjacency` table. The `rotatable` flag allows the solver to try all listed rotations, multiplying the effective tile count.

## Object Scattering Density Maps

The rule-based object placement system supports density maps -- grayscale textures that modulate spawn probability per texel:

- **White (1.0):** Full density, maximum spawn probability.
- **Black (0.0):** No spawning in this area.
- **Intermediate values** scale the configured base density linearly.

Multiple density maps can be layered (one per object type: trees, rocks, grass). The scattering system evaluates density maps in world-space UV matching the terrain splatmap coordinates.

### Scatter Configuration

```json
{
    "objectType": "pine_tree",
    "densityMap": "Textures/Terrain/tree_density.png",
    "baseDensity": 0.15,
    "minSpacing": 3.0,
    "maxSlope": 35.0,
    "heightRange": [10.0, 450.0],
    "scaleRange": [0.8, 1.4],
    "rotationRange": [0, 360],
    "alignToNormal": true,
    "sinkAmount": 0.1
}
```

## Terrain Holes and Cave Support

Terrain chunks support per-vertex hole flags that make portions of the terrain invisible and non-collidable. This enables:

- **Cave entrances** -- Punch holes in the terrain surface and place cave meshes underneath.
- **Tunnels** -- Chain hole regions with tunnel geometry.
- **Pits and chasms** -- Bottomless pits for gameplay hazards.

### Hole API

```cpp
terrain.SetHole(chunkX, chunkZ, localX, localZ, true);   // Punch a hole
terrain.SetHole(chunkX, chunkZ, localX, localZ, false);  // Fill a hole
terrain.SetHoleRect(chunkX, chunkZ, rect, true);          // Rectangular hole region
bool isHole = terrain.IsHole(chunkX, chunkZ, localX, localZ);
```

Holes are stored as a bitfield per chunk (1 bit per quad) and are serialized alongside the heightmap data. The collision shape is rebuilt when holes change.

## Terrain Serialization Format

The `.sparkterrain` native format stores all terrain data in a single binary archive:

| Section | Content |
|---------|---------|
| Header (64 bytes) | Magic number, version, dimensions, chunk size, height range, layer count |
| Heightmap | 32-bit float per vertex, row-major, chunk-major ordering |
| Splatmaps | RGBA8 per texel, one block per splatmap |
| Hole Mask | 1 bit per quad, packed into uint32 blocks |
| Layer Table | Array of layer descriptors (diffuse, normal, roughness texture paths, UV scale) |
| Object Scatter Data | Per-layer scatter configuration and instance transforms |
| Metadata | JSON blob with editor annotations, biome assignments, author info |

The format uses LZ4 block compression on heightmap and splatmap sections for fast streaming. A checksum (CRC-32) per section allows incremental validation.

## Biome System Integration

The terrain can be subdivided into biomes that drive splatmap auto-generation, object scattering rules, and weather/audio ambience:

```cpp
enum class BiomeType
{
    Grassland,
    Desert,
    Forest,
    Tundra,
    Swamp,
    Mountain,
    Custom
};
```

Biome boundaries are defined by a **biome map** -- a low-resolution texture where each unique color maps to a `BiomeType`. Biome transitions use a configurable blend radius (in world units) to smoothly cross-fade splatmap weights and scatter densities at boundaries.

### Biome Configuration

```json
{
    "biome": "Forest",
    "splatRules": {
        "base": "grass_dark",
        "slope": "mossy_rock",
        "high": "pine_needle"
    },
    "scatterLayers": ["pine_tree", "fern", "mushroom", "fallen_log"],
    "ambientAudio": "Audio/Ambience/forest_loop.wav",
    "fogColor": [0.2, 0.3, 0.15],
    "fogDensity": 0.008
}
```

## Runtime Terrain Modification API

Terrain can be modified at runtime for gameplay purposes (explosions, digging, building):

```cpp
// Raise or lower terrain in a circular area
terrain.ModifyHeight(worldPos, radius, deltaHeight, falloff);

// Flatten terrain to a target height
terrain.Flatten(worldPos, radius, targetHeight, strength);

// Smooth terrain (average neighboring heights)
terrain.Smooth(worldPos, radius, strength, iterations);

// Paint splatmap at runtime
terrain.PaintLayer(worldPos, radius, layerIndex, opacity, falloff);
```

### Undo Support

Runtime modifications are recorded in a circular buffer. The editor supports undo/redo for terrain edits:

```cpp
terrain.Undo();  // Revert last modification
terrain.Redo();  // Re-apply last undone modification
terrain.ClearHistory();  // Free modification history memory
```

### Performance Considerations

- Height modifications trigger a partial rebuild of the affected chunk's vertex buffer and collision shape.
- Only the dirty region is updated, not the entire chunk.
- Modifications are batched per frame -- multiple overlapping edits in the same frame are merged.
- For large-scale changes, use `terrain.BeginBatch()` / `terrain.EndBatch()` to defer GPU uploads until all edits are complete.

## Build availability

Heightmap terrain and the procedural-generation toolkit are part of the engine target. Do not pass invented `ENABLE_TERRAIN_SYSTEM` or `ENABLE_PROCEDURAL` cache values; CMake does not declare them.

---

## See Also

- [Physics](../subsystems/Physics.md) — Heightfield collision shapes
- [Rendering and Graphics](../subsystems/Rendering-and-Graphics.md) — Terrain rendering and LOD
- [Scene Management](../subsystems/Scene-Management.md) — Procedurally generated scenes
- [Asset Pipeline](Asset-Pipeline.md) — Terrain asset streaming and loading
- [Gameplay Systems](Gameplay-Systems.md) — Procedural content for gameplay
- [Event System](../subsystems/Event-System.md) — Terrain generation event hooks
