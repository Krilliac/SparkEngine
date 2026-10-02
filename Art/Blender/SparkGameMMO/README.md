# SparkGameMMO TownSquare kit

Three original props for the MMO hub, authored by `tools/blender/author_mmo_kit.py` on the shared
`tools/blender/spark_kit.py` library (see `Art/Blender/README.md`, "Per-module kits"). Art direction:
a warm fantasy market town matching the existing MMO props. Carved oak, brass fittings and teal guild
cloth; timber frames with pegged joints, brass corner caps and gently curved roofs. Every MTL diffuse is
one of four palette colours (oak `#6B3A1E`, brass `#9E6A2A`, guild teal `#1F5C55`, cream plaster
`#D1B98A`); shades differ only by roughness and metalness. No texture maps.

| Asset | Size (m, W x H x D) | Triangles (budget) | LOD1 | Collision |
|-------|---------------------|--------------------|------|-----------|
| `quest_board` | 2.0 x 2.2 x 0.66 | 1156 (1400) | 410 | hull, 100 |
| `vendor_stall` | 3.0 x 2.6 x 2.02 | 2364 (2600) | 846 | box, 12 |
| `portal_ring` | 3.2 x 3.63 x 1.8 (3.2 m ring) | 1924 (2400) | 690 | hull, 132 |

Each prop uses five materials. Exports live in `Assets/Models/MMO/Kit/` (`<asset>`, `<asset>_lod1`,
`<asset>_collision`, OBJ + MTL): metres, pivot at the ground-contact centre, +Y up, each prop's front
facing +Z. `mmo_kit.blend` keeps every prop as an editable collection, and `provenance.json` hash-binds
the script, library, source, exports and license. `preview.png` is a Workbench render of the kit; it is
not hash-bound.

`MMOEngineSystems::PlaceTownSquareKit` (`GameModules/SparkGameMMO/Source/Core/MMOEngineSystems.cpp`)
places the kit around the fountain of `Assets/Scenes/MMO/town_square.scene`: the quest board and vendor
stall north of it between the two market stalls, facing the default spawn, and the ShadowCrypt portal
south of it, facing the square.

```sh
PYTHONHOME=/usr xvfb-run -a blender -b --factory-startup --python-exit-code 1 \
  --python tools/blender/author_mmo_kit.py -- --repo .
python3 tools/blender/validate_kit.py Art/Blender/SparkGameMMO/provenance.json
```

Reruns are byte-identical for the exports, `.blend` and `provenance.json`. Workbench output varies by a
few pixels between runs, so the script re-renders `preview.png` only when the scene digest or the
authoring script changed (or the PNG is missing); an unchanged rerun leaves it untouched too.
All nine OBJ files load through the engine's `LoadOBJStaticMesh` with every triangle wound consistently
with its authored normal and every submesh assigned a material.

Known limits:
- The portal's convex-hull collision closes the ring opening. Walking through it needs a trigger volume
  or a custom compound proxy.
- The vendor stall's box collision also covers the space under the awning.
- The props carry no `materialPath`: the Windows world renderer (`WorldBasicRenderer.cpp`) draws each OBJ
  material group with its MTL `Kd` colour. Roughness and metalness are only in the MTL `Ns`/`illum`
  values, which that renderer does not read.
- Nobody has reviewed the props in game yet.
