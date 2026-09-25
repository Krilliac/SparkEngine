# SparkGameMMOFPS (TERRAFRONT) frontline-logistics kit

Four original props for the TERRAFRONT sanctuary, authored by `tools/blender/author_mmofps_kit.py` on the
shared `tools/blender/spark_kit.py` library (see `Art/Blender/README.md`, "Per-module kits"). Art direction:
near-future frontline logistics. Olive composites, orange safety accents and night-ops blue; angular
armour plates, recessed panel lines, stencilled IDs and a 0.5 m modular grid. Every MTL diffuse is one of
four palette colours (olive `#4B5320`, composite grey `#5E6468`, safety orange `#D9822B`, night blue
`#1D2733`); shades differ only by roughness and metalness. No texture maps.

| Asset | Size (m, W x H x D) | Triangles (budget) | LOD1 | Collision |
|-------|---------------------|--------------------|------|-----------|
| `supply_drop_pod` | 1.4 x 2.4 x 1.4 | 1036 (2200) | 352 | hull, 180 |
| `deployable_barricade` | 2.4 x 1.2 x 0.87 | 748 (1200) | 254 | hull, 84 |
| `comms_relay` | 1.79 x 3.5 x 1.57 | 880 (1800) | 298 | hull, 94 |
| `vehicle_pad` | 6.0 x 0.175 x 6.0 | 276 (900) | 84 | box, 12 |

Exports live in `Assets/Models/MMOFPS/Kit/` (`<asset>`, `<asset>_lod1`, `<asset>_collision`, OBJ + MTL):
metres, pivot at the ground-contact centre, +Y up, each prop's front (hatch, dish, drive-on arrow) facing
+Z. `mmofps_kit.blend` keeps every prop as an editable collection, and `provenance.json` hash-binds the
script, library, source, exports and license. `preview.png` is a Workbench render of the kit; it is not
hash-bound.

`TFSanctuaryDecor.cpp` (`GameModules/SparkGameMMOFPS/Source/World/`) places the kit as an east logistics
yard past the sanctuary's east barrier (400/3776): the vehicle pad at 430/3776 with a barricade either side
of its approach, the comms relay and a supply drop pod behind it. Each row uses yaw -90 (-110 for the pod)
so the fronts face west toward the plaza, and leaves the material empty so the ECS draw path uses the MTL
`Kd` colours.

```sh
PYTHONHOME=/usr xvfb-run -a blender -b --factory-startup --python-exit-code 1 \
  --python tools/blender/author_mmofps_kit.py -- --repo .
python3 tools/blender/validate_kit.py Art/Blender/SparkGameMMOFPS/provenance.json
```

Reruns are byte-identical for all outputs, `preview.png` included: the script turns off every
`use_stamp_*` field, because Blender otherwise writes the render date and time into the PNG metadata.

Known limits:
- The decor rows are visual only: nothing loads the `_collision` or `_lod1` meshes yet, and the vehicle pad
  is not tied to the vehicle spawn system.
- Roughness and metalness are only in the MTL `Ns`/`illum` values, which the world renderer does not read.
- Nobody has reviewed the props in game yet.
