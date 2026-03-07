# Terrain and Procedural Generation

SparkEngine includes heightmap terrain rendering and a procedural generation toolkit for creating game content at runtime.

**Source:** `SparkEngine/Source/Game/` (terrain), `SparkEngine/Source/Engine/Procedural/ProceduralGeneration.h`

## Heightmap Terrain

`ENABLE_TERRAIN_SYSTEM=ON`

The terrain system renders large outdoor environments using heightmaps:

- **Quadtree LOD** — Automatic level-of-detail based on camera distance
- **Texture Splatting** — Multi-texture blending based on height, slope, or paint masks
- **Heightfield Collision** — Bullet Physics `Heightfield` shape for terrain collisions
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

## CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `ENABLE_TERRAIN_SYSTEM` | ON | Heightmap terrain rendering |
| `ENABLE_PROCEDURAL` | ON | Procedural generation toolkit |

## See Also

- [[Physics]] — Heightfield collision shapes
- [[Rendering and Graphics]] — Terrain rendering and LOD
- [[Scene Management]] — Procedurally generated scenes
