# SparkGameRPG village and dungeon kit

Five original props for Oakhollow and the Shadow Crypt, authored by `tools/blender/author_rpg_kit.py` on the
shared `tools/blender/spark_kit.py` library (see `Art/Blender/README.md`, "Per-module kits"). Art direction
(owner-approved, "Village & Dungeon RPG"): a storybook village by day and a torch-lit dungeon by night, built
from rounded timber, thatch overhangs, iron bands and hand-hewn stone. Every MTL diffuse is one of four palette
colours (village timber `#7A4A28`, thatch `#C9A55A`, slate `#4A5058`, torch flame `#E8752A`); shades differ only
by roughness and metalness (iron is metallic slate, the chest's brass is metallic flame, well water is glossy
slate). Faceted flat shading, 2-3 cm chamfers on hard edges (1-2 cm on bands and plates too thin for more),
no texture maps.

| Asset | Size (m, W x H x D) | Triangles (budget) | LOD1 | Collision | Materials |
|-------|---------------------|--------------------|------|-----------|-----------|
| `village_well` | 2.04 x 2.40 x 2.06 (1.6 m stone ring) | 1788 (2200) | 642 | hull, 124 | 5 |
| `quest_signpost` | 1.23 x 2.08 x 0.71 | 656 (700) | 236 | hull, 110 | 5 |
| `treasure_chest` | 0.89 x 0.60 x 0.61 | 916 (1500) | 328 | box, 12 | 4 |
| `wall_sconce` | 0.15 x 0.60 x 0.28 | 386 (500) | 138 | hull, 90 | 4 |
| `barrel` | 0.74 x 0.90 x 0.74 | 628 (800) | 218 | hull, 152 | 3 |

The well's overall width is its thatch roof and eave rolls overhanging the 1.6 m stone ring. Exports live in
`Assets/Models/RPG/Kit/` (`<asset>`, `<asset>_lod1`, `<asset>_collision`, OBJ + MTL): metres, +Y up, each prop's
front facing +Z, pivot at the ground-contact centre. The wall sconce is wall-mounted: its pivot is the bottom
centre of its wall plate, the wall is the OBJ z = 0 plane and the torch reaches out along +Z. `rpg_kit.blend`
keeps every prop as an editable collection, and `provenance.json` hash-binds the script, library, source,
exports and license. `preview.png` is a Workbench render of the kit; it is not hash-bound.

`RPGWorldSetup::RegisterAreasWithStreaming` (`GameModules/SparkGameRPG/Source/World/RPGWorldSetup.cpp`) streams
the kit with the areas where each prop belongs: Village (well, signpost, barrel), Forest and Swamp (signpost),
Dungeon and Castle (sconce, chest, barrel).

```sh
PYTHONHOME=/usr xvfb-run -a blender -b --factory-startup --python-exit-code 1 \
  --python tools/blender/author_rpg_kit.py -- --repo .
python3 tools/blender/validate_kit.py Art/Blender/SparkGameRPG/provenance.json
```

Reruns are byte-identical for the exports, `.blend` and `provenance.json`; the preview PNG is not hash-bound.
All fifteen OBJ files load through the engine's `LoadOBJStaticMesh` with every triangle wound consistently with
its authored normal and every submesh assigned a material.

Known limits:
- The areas have no scene or placement data, so the kit is streamed with each area but no module code
  positions instances in the world yet.
- The well's convex-hull collision is a solid volume up to the roof; the well is not meant to be entered.
- The flame is flat-coloured geometry; the MTL has no emissive term, so the torch light itself must come from
  a scene light.
