# SparkGamePlatformer Level 0 kit

Five original props for the platformer's levels, authored by `tools/blender/author_platformer_kit.py` on the
shared `tools/blender/spark_kit.py` library (see `Art/Blender/README.md`, "Per-module kits"). Art direction:
a bright toybox with saturated primaries, soft rounded forms and hazards you can't miss. Pill and rounded-box
shapes, chunky bevelled outlines and exaggerated proportions. Every MTL diffuse is one of four palette colours
(sky `#6CC3F0`, grass `#5BBF4A`, coin gold `#F5C542`, brick `#C4633A`). Shades differ only by roughness and
metalness (matte and glazed brick, satin, polished and mirror gold, painted sky steel). No texture maps.

| Asset | Size (m, W x H x D) | Triangles (budget) | LOD1 | Collision | Materials |
|-------|---------------------|--------------------|------|-----------|-----------|
| `floating_platform` | 3.14 x 1.0 x 3.14 (3 x 3 m turf top, grass drips overhang) | 672 (900) | 240 | box, 12 | 4 |
| `coin` | 0.5 x 0.5 x 0.09 | 292 (300) | 104 | hull, 124 | 3 |
| `spring_pad` | 1.0 x 0.58 x 1.0 | 768 (800) | 276 | hull, 164 | 5 |
| `spike_hazard` | 1.0 x 0.64 x 1.0 | 448 (500) | 160 | box, 12 | 4 |
| `goal_flag` | 1.79 x 3.5 x 0.9 | 336 (600) | 120 | box, 12 | 5 |

Every LOD1 is 35-36% of its source (limit 40%), and every collision mesh is at most 200 triangles.
Exports live in `Assets/Models/Platformer/Kit/` (`<asset>`, `<asset>_lod1` and `<asset>_collision`, each as
OBJ + MTL). Units are metres, +Y is up, the pivot is the ground-contact centre and each prop's front faces +Z.
`platformer_kit.blend` keeps every prop as an editable collection, and `provenance.json` hash-binds the script,
library, source, exports and license. `preview.png` is a Workbench render of the kit (fixed camera and lighting,
1600x900). It is not hash-bound.

How the module places the kit (only when the engine has a world, so headless tests place nothing):

- `PlatformerLevelFlow::PlaceLevelKit` (`GameModules/SparkGamePlatformer/Source/Core/PlatformerLevelFlow.cpp`)
  runs on every `StartLevel`. It adds one mesh per platform collider: a `floating_platform` stretched to the
  collider's footprint with its turf top on the walkable surface, or a uniformly scaled `spring_pad` for Bouncy
  platforms. It then stands the `goal_flag` near the back edge of the level's last platform, which is the goal
  platform in every level. The flag is turned 180 degrees so it faces the follow camera on the -Z side.
  `SyncLevelKit` moves the platform meshes with their colliders each fixed step and hides Disappearing
  platforms while they are not solid. The next `StartLevel` and the flow's destructor remove the entities.
- `PlatformerHazardSystem::PlaceSpikeTiles` tiles each spike pit's damage box with 1 m `spike_hazard` tiles.
  The tiles are stretched so the spike tips reach the top of the box. `Shutdown` removes them.
- `PlatformerCollectibleSystem::PlaceCoinMeshes` adds a `coin` at every Coin collectible, lowered by half its
  height so the disc is centred on the item. `Render` spins and bobs each coin and hides it once collected, and
  `Shutdown` removes them.

```sh
PYTHONHOME=/usr xvfb-run -a blender -b --factory-startup --python-exit-code 1 \
  --python tools/blender/author_platformer_kit.py -- --repo .
python3 tools/blender/validate_kit.py Art/Blender/SparkGamePlatformer/provenance.json
```

Reruns produce byte-identical exports, `.blend` and `provenance.json`. The preview PNG can differ between runs.
All fifteen OBJ files load through the engine's `LoadOBJStaticMesh`. Every triangle winds consistently with its
authored normal, and every submesh has a material assigned.

Known limits:

- The goal flag's position comes from the convention that each level ends on its goal platform, not from
  `LevelDef::goalPoint`, which is private to `PlatformerLevelSystem`.
- The meshes are visual only. Platformer collision stays in the module's axis-aligned colliders, and no
  collider or trigger component is attached. Rotating platforms show their mesh unrotated at its start
  footprint, while their collider is the axis-aligned bounds of the rotated footprint (up to about 5.7 x 5.7 m
  for the 8 x 2 m platform in level 2), so part of what the player can stand on is not drawn. Turning the mesh
  would not close that gap; the fix is oriented collision in the level simulation.
- Wide platforms stretch the island non-uniformly, for example 3.3x along X for the 10 m starting platform.
- Nobody has reviewed the props in game yet.
