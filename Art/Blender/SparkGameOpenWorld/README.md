# SparkGameOpenWorld landmark kit

Three original landmark props for the open world, authored by `tools/blender/author_openworld_kit.py` on the
shared `tools/blender/spark_kit.py` library (see `Art/Blender/README.md`, "Per-module kits"). Art direction
(owner-approved, "Open World Landmarks"): a weathered frontier of moss-stained stone and pine timber you can
navigate by from a distance, built from irregular stacked stone with broken silhouettes and tall vertical
landmarks. Every MTL diffuse is one of four palette colours (weathered stone `#7D7A70`, moss `#4E6B3A`, pine
`#2F4A2A`, sunset clay `#B8643C`); shades differ only by roughness (dressed stone is the smoother stone).
Faceted flat shading, 2-3 cm chamfers on hard edges, no texture maps.

| Asset | Size (m, W x H x D) | Triangles (budget) | LOD1 | Collision |
|-------|---------------------|--------------------|------|-----------|
| `watchtower` | 4.2 x 8.0 x 3.8 (3.0 m keep) | 2040 (2800) | 734 | hull, 108 |
| `bridge_segment` | 6.0 x 2.4 x 2.4 (6.0 x 2.4 m deck) | 1892 (2000) | 680 | box, 12 |
| `ruined_arch` | 4.6 x 5.0 x 2.2 (3.9 m arch) | 1540 (1800) | 554 | hull, 120 |

The overall widths include moss mats and fallen blocks on the ground around the pivot. Each prop uses five
materials (stone, dressed stone, moss, pine, clay). Exports live in `Assets/Models/OpenWorld/Kit/` (`<asset>`,
`<asset>_lod1`, `<asset>_collision`, OBJ + MTL): metres, pivot at the ground-contact centre, +Y up, each prop's
front facing +Z. The bridge deck runs along X, so segments chain end to end. `openworld_kit.blend` keeps every
prop as an editable collection, and `provenance.json` hash-binds the script, library, source, exports and
license. `preview.png` is a Workbench render of the kit; it is not hash-bound.

`OWWorldSetup::RegisterAreasWithStreaming` (`GameModules/SparkGameOpenWorld/Source/World/OWWorldSetup.cpp`)
adds the three meshes to every region's streaming manifest next to the ModuleKits landmark props.

```sh
PYTHONHOME=/usr xvfb-run -a blender -b --factory-startup --python-exit-code 1 \
  --python tools/blender/author_openworld_kit.py -- --repo .
python3 tools/blender/validate_kit.py Art/Blender/SparkGameOpenWorld/provenance.json
```

Reruns are byte-identical for the exports, `.blend` and `provenance.json`; the preview PNG is not.
All nine OBJ files load through the engine's `LoadOBJStaticMesh` with every triangle wound consistently with
its authored normal and every submesh assigned a material.

Known limits:
- The regions have no scene or placement data, so the kit is streamed with each region but no module code
  positions instances in the world yet. The natural sites are the exploration POIs: `watchtower` at Ranger's
  Watchtower and Frost Watchtower, `ruined_arch` at Windmill Ruins and Sunken Ruins, `bridge_segment` where
  the Marshway and Bogwalk roads cross water.
- The arch's convex-hull collision closes the opening; walking through it needs a custom compound proxy.
- The bridge's box collision covers the space under the deck and reaches the rail tops (2.4 m); a walkable
  deck needs a custom proxy.
