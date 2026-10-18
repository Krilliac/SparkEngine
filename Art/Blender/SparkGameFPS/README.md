# SparkGameFPS training-arena kit

Five original props for the FPS arena, authored by `tools/blender/author_fps_kit.py` on the shared
`tools/blender/spark_kit.py` library (see `Art/Blender/README.md`, "Per-module kits"). Art direction:
industrial training arena, poured concrete with painted hazard markings, heavy chamfered slabs and
stencil-stripe bands. Every MTL diffuse is one of four palette colours (concrete `#8C8A84`, hazard
yellow `#E3B23C`, gunmetal `#3A3F45`, signal red `#C8453A`); shades differ only by roughness and
metalness. No texture maps.

| Asset | Size (m) | Triangles (budget) | LOD1 | Collision |
|-------|----------|--------------------|------|-----------|
| `cover_barrier` | 2.0 x 1.0 x 0.5 | 296 (800) | 104 | hull, 124 |
| `ammo_crate` | 0.6 x 0.35 x 0.4 | 464 (700) | 166 | box, 12 |
| `spawn_pad` | 2.0 diameter, 0.17 tall | 1004 (1100) | 320 | hull, 180 |
| `target_dummy` | 0.72 x 1.8 x 0.68 | 776 (1600) | 276 | hull, 112 |
| `weapon_rack` | 1.6 x 1.2 x 0.49 | 888 (1400) | 318 | box, 12 |

Exports live in `Assets/Models/FPS/Kit/` (`<asset>`, `<asset>_lod1`, `<asset>_collision`, OBJ + MTL):
metres, pivot at the ground-contact centre, +Y up, each prop's front facing +Z. `fps_kit.blend` keeps
every prop as an editable collection, and `provenance.json` hash-binds the script, library, source,
exports and license. `preview.png` is a Workbench render of the kit; it is not hash-bound.

`Game::CreateCombatArena` (`GameModules/SparkGameFPS/Source/Game/GameConsoleOps.cpp`) places the kit:
spawn pads at the four default spawns of `Scenes/level1.scene` (the east and west pads sit 1.6 m toward
+Z, clear of the arena barriers standing on those spawns), cover barriers in front of the north and south
spawns, weapon racks and ammo crates at the back of each base (behind Alpha's weapon displays), and
target dummies staggered 3 m behind the practice targets, facing the shooting lane.

```sh
PYTHONHOME=/usr xvfb-run -a blender -b --factory-startup --python-exit-code 1 \
  --python tools/blender/author_fps_kit.py -- --repo .
python3 tools/blender/validate_kit.py Art/Blender/SparkGameFPS/provenance.json
```

Reruns are byte-identical for the exports, `.blend` and `provenance.json`; the preview PNG is not.
All fifteen OBJ files load through the engine's `LoadOBJStaticMesh` with every triangle wound
consistently with its authored normal and every submesh assigned a material. The Windows D3D11
`Model` ignores MTL data, so in game each prop takes the procedural material of its main surface
(concrete or metal) instead of its palette colours. Nobody has reviewed the props in game yet.
