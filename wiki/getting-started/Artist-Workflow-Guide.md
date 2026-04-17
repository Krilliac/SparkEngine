# Artist Workflow Guide

This guide is for 3D artists, level designers, and technical artists who want to create content for SparkEngine games without writing C++. It covers supported formats, recommended tools, the SparkEditor workflow, and performance guidelines.

**Related pages:** [Asset Pipeline](../gameplay-tools/Asset-Pipeline.md), [Asset Format Specifications](../specifications/Asset-Format-Specifications.md), [SparkEditor](../gameplay-tools/SparkEditor.md), [Terrain and Procedural Generation](../gameplay-tools/Terrain-and-Procedural-Generation.md), [Collaborative Editing](../subsystems/Collaborative-Editing.md)

---

## Supported Asset Formats

| Category | Formats | Notes |
|----------|---------|-------|
| **3D Models** | `.glb` / `.gltf` (glTF 2.0), `.obj` (Wavefront) | glTF is preferred — supports PBR materials, skeletal animation, and embedded textures |
| **Textures** | `.png`, `.jpg`, `.bmp`, `.tga`, `.dds` | DDS is fastest (GPU-ready); PNG is lossless and good for normals/masks |
| **Audio** | `.wav` | 16-bit PCM recommended; XAudio2 backend |
| **Heightmaps** | Grayscale `.png`, `.raw` (16-bit) | 16-bit RAW gives 65,536 height levels for smoother terrain |
| **Animations** | `.glb` / `.gltf`, `.skel` | Skeletal clips exported from DCC tools or authored in-editor |
| **Scenes** | `.scene` | SparkEngine JSON scene format, saved from the editor |
| **Scripts** | `.as` | AngelScript — see [Scripting with AngelScript](../subsystems/Scripting-with-AngelScript.md) |

### Format Recommendations

- **Use glTF 2.0 (`.glb`) as your primary model format.** It embeds PBR material definitions, supports skinned meshes and morph targets, and loads faster than OBJ.
- **Use OBJ only for simple static meshes** that do not need materials or animations.
- **Use DDS for shipping textures** when possible. The asset pipeline accepts PNG/JPG/TGA for authoring convenience, but DDS avoids runtime compression.
- **Audio must be WAV.** Convert MP3/OGG to 16-bit 44.1 kHz WAV before importing.

---

## Recommended DCC Tools

Any tool that exports glTF 2.0 or OBJ will work. Tested and recommended:

| Tool | Cost | glTF Export | Notes |
|------|------|-------------|-------|
| **Blender** (3.6+) | Free | Built-in | Best free option; excellent glTF exporter |
| **Autodesk Maya** | Commercial | Via plugin | Use Babylon.js or official Khronos exporter |
| **Autodesk 3ds Max** | Commercial | Via plugin | Use Babylon.js exporter |
| **Substance Painter** | Commercial | Direct export | PBR texture authoring; export as PNG or DDS |

### glTF Export Settings

When exporting from any DCC tool, use these settings for SparkEngine compatibility:

| Setting | Value |
|---------|-------|
| Up axis | **Y-up** |
| Scale | **1 unit = 1 meter** |
| Triangulation | **On** (triangulate all meshes) |
| Format | **glTF Binary (.glb)** preferred |
| Normals | **Include** |
| Tangents | **Include** (required for normal maps) |
| Materials | **glTF PBR (metallic-roughness)** |
| Textures | **Embed in GLB** or place alongside as PNG |
| Animations | **Include** if the model is animated |

### Blender Quick Export

1. Select your object(s)
2. **File > Export > glTF 2.0 (.glb/.gltf)**
3. Set Format to **glTF Binary (.glb)**
4. Under **Include**, check **Selected Objects** if you do not want the whole scene
5. Under **Transform**, set **+Y Up**
6. Under **Mesh**, check **Apply Modifiers** and **Triangulate Faces**
7. Under **Material**, set **Images** to **Automatic**
8. Export

---

## PBR Texture Workflow

SparkEngine uses a metallic-roughness PBR model. Each material can reference up to six texture maps:

| Map | Channel(s) | Description |
|-----|-----------|-------------|
| **Albedo** (Base Color) | RGB | Surface color without lighting; alpha channel for transparency |
| **Metallic** | Grayscale | 0 = dielectric, 1 = metal. Binary for most real surfaces |
| **Roughness** | Grayscale | 0 = mirror-smooth, 1 = fully rough |
| **Normal** | RGB (tangent-space) | Per-pixel surface detail. Use OpenGL convention (+Y up) |
| **Ambient Occlusion** | Grayscale | Baked cavity shadows. Multiplied with indirect lighting |
| **Emission** | RGB | Self-illumination color and intensity |

### Texture Naming Convention

The asset pipeline auto-detects texture purpose from suffixes:

```
Props/Barrel/barrel_albedo.png
Props/Barrel/barrel_metallic.png
Props/Barrel/barrel_roughness.png
Props/Barrel/barrel_normal.png
Props/Barrel/barrel_ao.png
Props/Barrel/barrel_emission.png
```

### PBR Tips

- **Roughness 0.3 - 0.8** covers most realistic surfaces. Values below 0.1 create mirror-like reflections that require careful lighting.
- **Metallic is binary for most materials.** Use 0 for wood, plastic, stone, fabric. Use 1 for raw metals. Partial values (0.0 - 1.0) are only for transitions like dirt on metal.
- **Never put lighting information in albedo.** No baked shadows, no ambient occlusion in the base color texture.
- **Normal maps use OpenGL convention** (+Y pointing up in tangent space). If your normals look inverted, flip the green channel.
- **Pack metallic + roughness into one texture** (metallic in blue, roughness in green) when optimizing for fewer texture samples. The Material Editor supports channel packing.

---

## Asset Directory Structure

Organize your project assets under the `Assets/` directory:

```
Assets/
  Models/           -- 3D models (.obj, .glb)
    Characters/     -- Player, NPCs, enemies
    Props/          -- Barrels, crates, furniture
    Vehicles/       -- Cars, ships
    Weapons/        -- Weapon meshes
  Textures/         -- PBR texture sets
    Characters/
    Environment/
    UI/
  Sounds/           -- Audio files (.wav)
    SFX/            -- Sound effects
    Music/          -- Background music
    Ambient/        -- Environmental audio
  Materials/        -- Material definitions (.mat)
  Scenes/           -- Scene files (.scene)
  Scripts/          -- AngelScript files (.as)
  Animations/       -- Skeletal animation clips (.skel)
  Terrain/          -- Heightmaps, terrain splatmaps
  Particles/        -- Particle effect definitions
  Prefabs/          -- Reusable entity prefabs
```

Keep texture sets alongside their models when practical, or mirror the folder structure under `Textures/`. Consistency matters more than the exact layout.

---

## Using SparkEditor

Launch SparkEditor and press **F1** to toggle the editor overlay. The editor is built on Dear ImGui with dockable panels.

### Core Panels

| Panel | Purpose | How to Open |
|-------|---------|-------------|
| **Scene Hierarchy** | Tree view of all entities in the scene | Window > Scene Hierarchy |
| **Properties** | Edit components on the selected entity (transform, mesh, material, etc.) | Window > Properties |
| **Asset Browser** | Browse, search, and drag-drop assets into the scene | Window > Asset Browser |
| **Material Editor** | Create and edit PBR materials | Window > Material Editor |
| **Terrain Editor** | Sculpt and paint terrain | Window > Terrain Editor |
| **Lighting** | Configure directional, point, and spot lights | Window > Lighting |
| **Particle Editor** | Author particle effects | Window > Particle Editor |
| **Console** | Engine console for commands and log output | Window > Console |
| **Viewport** | 3D scene view with gizmos | Always visible |

### Gizmo System

The viewport provides translate, rotate, and scale gizmos for selected entities:

| Hotkey | Gizmo | Description |
|--------|-------|-------------|
| **W** | Translate | Move along X/Y/Z axes |
| **E** | Rotate | Rotate around X/Y/Z axes |
| **R** | Scale | Scale along X/Y/Z axes |
| **Q** | None | Deselect gizmo (free camera) |
| **X / Y / Z** | Axis lock | Constrain to a single axis |
| **Ctrl+Z** | Undo | Undo last operation |
| **Ctrl+Y** | Redo | Redo last undone operation |
| **Ctrl+S** | Save | Save current scene |
| **Ctrl+P** | Command Palette | Search and run editor commands |
| **Delete** | Delete | Remove selected entity |

### Basic Workflow

1. **Create or open a scene** via File > New Scene or File > Open Scene
2. **Import assets** by dragging files from the Asset Browser into the viewport
3. **Position objects** using the translate gizmo (W) and snap settings
4. **Assign materials** in the Properties panel or drag from the Material Editor
5. **Add lights** from the Lighting panel or right-click > Add Light
6. **Test** by pressing Play (or Ctrl+Enter) to enter play mode
7. **Save** with Ctrl+S

---

## Material Editor Walkthrough

The Material Editor panel lets you create PBR materials without code.

### Creating a New Material

1. Open **Window > Material Editor**
2. Click **New Material** and give it a name
3. Set base properties:
   - **Albedo Color** — base color tint (white if using a texture)
   - **Albedo Texture** — drag a texture from the Asset Browser
   - **Metallic** — slider 0.0 to 1.0, or assign a metallic map
   - **Roughness** — slider 0.0 to 1.0, or assign a roughness map
   - **Normal Map** — drag a tangent-space normal map
   - **AO Map** — drag an ambient occlusion texture
   - **Emission Color** — color and intensity for self-illumination
4. Preview the material on the built-in sphere/cube/custom mesh
5. Click **Save** to write the `.mat` file to `Assets/Materials/`

### Applying Materials

- **Drag-drop** from the Material Editor onto a mesh in the viewport
- **Properties panel** — select an entity, expand the Mesh Renderer component, and pick a material from the dropdown

---

## Lighting Best Practices

SparkEngine supports three light types plus environment lighting:

| Light Type | Use Case | Shadow Support | Performance Cost |
|------------|----------|---------------|-----------------|
| **Directional** | Sun, moon, large area fill | Yes (cascaded shadow maps) | Low (one per scene typical) |
| **Point** | Lamps, fires, explosions | Yes (omnidirectional) | Medium per light |
| **Spot** | Flashlights, stage lights, cones | Yes (single frustum) | Medium per light |

### Guidelines

- **Use one directional light as your sun.** Enable shadows with 2-4 cascades for outdoor scenes.
- **Limit shadow-casting point/spot lights.** Each shadow-casting light costs a render pass. Use 4-8 shadow-casting lights maximum in view.
- **Non-shadow lights are cheap.** Use many point lights without shadows for ambient fill.
- **Place light probes** at key locations for indirect illumination. The engine interpolates between probes for smooth ambient transitions.
- **Time-of-day:** The [Day/Night Cycle](../gameplay-tools/Day-Night-Cycle-and-Weather.md) system drives the directional light color, intensity, and angle automatically. Artists configure the sky gradient and light curve in the editor.

---

## Terrain Workflow

See also: [Terrain and Procedural Generation](../gameplay-tools/Terrain-and-Procedural-Generation.md)

### Importing a Heightmap

1. Open **Window > Terrain Editor**
2. Click **Import Heightmap**
3. Select a grayscale `.png` or 16-bit `.raw` file
4. Set terrain dimensions (width, length, max height in meters)
5. Click **Generate** — the terrain mesh appears in the viewport

### Sculpt Brushes

| Brush | Shortcut | Description |
|-------|----------|-------------|
| **Raise** | 1 | Push terrain upward |
| **Lower** | 2 | Push terrain downward |
| **Smooth** | 3 | Average neighboring heights |
| **Flatten** | 4 | Level terrain to a target height |
| **Noise** | 5 | Apply procedural noise to the brush area |

Adjust **brush radius** and **strength** with the sliders or hold **Ctrl + Scroll** to resize the brush.

### Texture Painting

1. In the Terrain Editor, switch to the **Paint** tab
2. Add terrain layers (grass, dirt, rock, snow, etc.) with albedo + normal textures
3. Select a layer and paint directly on the terrain in the viewport
4. The engine blends layers with smooth transitions using splatmap rendering

### LOD

Terrain uses quadtree LOD automatically. Chunks closer to the camera render at full resolution; distant chunks use simplified geometry. No artist configuration needed.

---

## Particle Effects

The Particle Editor creates GPU-accelerated particle effects.

### Creating an Effect

1. Open **Window > Particle Editor**
2. Click **New Emitter**
3. Configure emitter properties:

| Property | Description |
|----------|-------------|
| **Emission Rate** | Particles per second |
| **Lifetime** | How long each particle lives (seconds) |
| **Start Size / End Size** | Size over lifetime |
| **Start Color / End Color** | Color and alpha over lifetime |
| **Velocity** | Initial speed and direction |
| **Gravity** | Downward acceleration |
| **Blend Mode** | Additive (fire, sparks), Alpha (smoke, dust) |
| **Texture** | Sprite or flipbook atlas |
| **Collision** | Bounce, stick, or die on contact |

### Common Recipes

| Effect | Emission Rate | Lifetime | Blend | Velocity | Gravity | Color |
|--------|--------------|----------|-------|----------|---------|-------|
| **Campfire** | 50-100 | 1.0-2.0s | Additive | Up (2-5) | -0.5 | Orange to red, fade alpha |
| **Smoke** | 20-40 | 3.0-5.0s | Alpha | Up (1-3) | -0.2 | Gray, low alpha |
| **Sparks** | 200-500 | 0.3-0.8s | Additive | Random (5-15) | 9.8 | Yellow-white |
| **Rain** | 1000+ | 1.0-2.0s | Alpha | Down (10-20) | 0 | White, thin streaks |
| **Dust Motes** | 10-20 | 5.0-10.0s | Alpha | Random (0.1-0.5) | 0 | Tan, very low alpha |

---

## Collaborative Editing

Multiple artists can work on the same scene simultaneously using SparkEngine's collaborative editing system. One editor hosts the session; others connect via TCP.

- **Real-time presence:** See other artists' cursors and selections in the viewport
- **Node locking:** When you select an entity, it locks for you — others cannot move it until you deselect
- **Edit broadcasting:** Changes propagate instantly to all connected editors
- **Live push:** Optionally forward edits to a running game server for immediate in-game preview

See [Collaborative Editing](../subsystems/Collaborative-Editing.md) for setup and networking details.

---

## Performance Guidelines

### Triangle Budgets

| Asset Type | Triangle Budget | Example |
|------------|----------------|---------|
| Hero character | 80,000 - 100,000 | Player character, main NPCs |
| Secondary character | 30,000 - 50,000 | Background NPCs, enemies |
| Hero prop | 10,000 - 20,000 | Weapons, key items |
| Small prop | 2,000 - 10,000 | Barrels, crates, bottles |
| Background prop | 500 - 2,000 | Distant buildings, rocks |
| Vegetation | 500 - 5,000 | Trees, bushes (use alpha cutout) |

### Texture Size Guidelines

| Asset Type | Max Resolution | Notes |
|------------|---------------|-------|
| Hero character | 2048 x 2048 | Full PBR set (albedo, normal, metallic, roughness, AO) |
| Props (large) | 1024 x 1024 | Crates, furniture |
| Props (small) | 512 x 512 | Bottles, bullets, small items |
| Terrain layers | 1024 x 1024 | Tiling textures for grass, dirt, rock |
| UI elements | Power-of-two | Match display resolution needs |
| Particle sprites | 256 x 256 | Often smaller; flipbook atlases up to 1024 |

### LOD Meshes

Provide 2-3 LOD levels per model to reduce distant geometry cost:

| LOD Level | Distance | Triangle Target |
|-----------|----------|----------------|
| **LOD0** | 0 - 20m | Full detail (100%) |
| **LOD1** | 20 - 50m | ~50% of LOD0 |
| **LOD2** | 50m+ | ~25% of LOD0 |

Name LOD meshes with a `_LOD0`, `_LOD1`, `_LOD2` suffix. The asset pipeline detects these automatically and groups them.

### General Tips

- **Use decals instead of geometry** for bullet holes, blood splatter, cracks, and surface detail that does not affect the silhouette.
- **Atlas small textures** into shared texture sheets to reduce draw calls.
- **Reuse materials.** Every unique material is a potential draw call. Fewer materials = faster rendering.
- **Avoid alpha transparency where possible.** Alpha-blended surfaces are expensive. Use alpha-test (cutout) for vegetation and fences.
- **Keep draw calls under 2,000** per frame for typical scenes. Use the Profiler panel (Window > Profiler) to monitor.
- **Bake ambient occlusion** into vertex colors or a separate AO map rather than relying on runtime SSAO for static objects.

---

## Quick Reference Card

| Task | Where |
|------|-------|
| Import a model | Drag `.glb` from Asset Browser into viewport |
| Create a material | Window > Material Editor > New Material |
| Edit terrain | Window > Terrain Editor |
| Place a light | Right-click viewport > Add Light, or Lighting panel |
| Create particles | Window > Particle Editor > New Emitter |
| Test the scene | Ctrl+Enter (Play mode) |
| Save the scene | Ctrl+S |
| Undo / Redo | Ctrl+Z / Ctrl+Y |
| Switch gizmo | W (translate), E (rotate), R (scale) |
| Command palette | Ctrl+P |
| Collaborative edit | Window > Collaborative Session > Host or Join |

---

## Further Reading

- [Asset Pipeline](../gameplay-tools/Asset-Pipeline.md) — how assets are loaded, cached, and streamed
- [Asset Format Specifications](../specifications/Asset-Format-Specifications.md) — internal data formats
- [Scripting with AngelScript](../subsystems/Scripting-with-AngelScript.md) — gameplay scripting without C++
- [Visual Scripting](../subsystems/Visual-Scripting.md) — node-based logic for non-programmers
- [Rendering and Graphics](../subsystems/Rendering-and-Graphics.md) — graphics pipeline details
- [Day Night Cycle and Weather](../gameplay-tools/Day-Night-Cycle-and-Weather.md) — time-of-day and weather systems
- [Shader Pipeline](../gameplay-tools/Shader-Pipeline.md) — custom shader authoring
