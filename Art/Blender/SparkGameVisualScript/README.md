# SparkGameVisualScript blueprint-lab kit

Four original props for the visual-script demo, authored by `tools/blender/author_visualscript_kit.py` on the
shared `tools/blender/spark_kit.py` library (see `Art/Blender/README.md`, "Per-module kits"). Art direction: a
blueprint lab where every interactive piece shows its state in colour and shape, with clear mechanical affordances
(handles, rails, plates and lamps). Every MTL diffuse is one of four palette colours (blueprint navy `#1B2A4A`,
node cyan `#34C6D3`, signal green `#58C26B`, off grey `#6B7280`). Shades differ only by roughness and metalness.
Faceted flat shading, 2-3 cm chamfers on hard edges, no texture maps.

The state language is the same on every prop: signal green means on or armed, off grey means off or idle, and
node cyan marks the part a player touches or a script drives (lever grip, door handle, plate rim, lamp junction).

| Asset | Size (m, W x H x D) | Triangles (budget) | LOD1 | Collision | Materials |
|-------|---------------------|--------------------|------|-----------|-----------|
| `lever` | 0.62 x 1.2 x 0.55 (handle thrown forward, "on") | 556 (600) | 200 | hull, 90 | 5 |
| `sliding_door` | 2.0 x 3.0 x 0.47 (closed, green "unlocked" header lamp) | 724 (900) | 260 | box, 12 | 5 |
| `pressure_plate` | 1.2 x 0.17 x 1.2 | 328 (400) | 116 | box, 12 | 4 |
| `signal_lamp` | 0.5 x 2.4 x 0.5 (green "on" and grey "off" lens) | 504 (700) | 178 | hull, 72 | 5 |

Every LOD1 is 36% of its source (limit 40%), and every collision mesh is at most 200 triangles.
Exports live in `Assets/Models/VisualScript/Kit/` (`<asset>`, `<asset>_lod1` and `<asset>_collision`, each as
OBJ + MTL). Units are metres, +Y is up, the pivot is the ground-contact centre and each prop's front faces +Z.
`visualscript_kit.blend` keeps every prop as an editable collection, and `provenance.json` hash-binds the script,
library, source, exports and license. `preview.png` is a Workbench render of the kit (fixed camera and lighting,
1600x900). It is not hash-bound.

How the module places the kit: `DemoWorld::PlaceKitProps`
(`GameModules/SparkGameVisualScript/Source/Core/VisualScriptDemoWorld.cpp`) runs at the end of a successful
`Spawn()`, after all eleven script entities attached. It adds five script-less `VSKit_*` entities: a
`pressure_plate` under the player spawn, a `lever` beside it, and the `sliding_door` behind the coin row at
z = 17 with a `signal_lamp` on each side. The lever, door and lamps are turned 180 degrees to face the player, who
walks toward +Z. The props are tracked apart from the script entities (`GetKitProps()`), so the eleven-entity
`VS_*` contract is unchanged, and `DestroyEntities()` removes them on `vs_restart`, rollback and unload.

```sh
PYTHONHOME=/usr xvfb-run -a blender -b --factory-startup --python-exit-code 1 \
  --python tools/blender/author_visualscript_kit.py -- --repo .
python3 tools/blender/validate_kit.py Art/Blender/SparkGameVisualScript/provenance.json
```

A fresh run reproduces all 24 exports byte for byte with the same scene digest, and a rerun over the committed
source leaves the `.blend` and `provenance.json` byte-identical. Blender's UV sphere and Decimate emit faces and
vertices in a varying order, so the script sorts the lever knob's faces and rewrites every exported OBJ with
sorted, deduplicated positions and normals (`canonicalize_vertex_order`). The preview PNG can differ between runs.

Known limits:

- The props are set dressing only. They carry no collider, trigger or script, so the lever does not throw, the
  plate does not arm and the door does not open; the demo's gameplay stays in the generated scripts.
- `signal_lamp_lod1`'s collapsed plinth base sits about 1.3 cm above the ground.
- The `MeshRenderer` components carry no `materialPath`, so whether the MTL colours show depends on the
  renderer's MTL support. Nobody has reviewed the props in a running engine yet.
