# SparkGameRTS Real-Time Strategy kit

Five original props for the RTS skirmish, authored by `tools/blender/author_rts_kit.py` on the shared
`tools/blender/spark_kit.py` library (see `Art/Blender/README.md`, "Per-module kits"). Art direction:
readable from a high camera, neutral stone and iron with bold faction colours. Stepped bases, big roof planes,
faction-colour banners and trims. Every MTL diffuse is one of the palette colours (Azure `#2F6FD6` for faction 1,
Crimson `#C8352F` for faction 2, Stone `#8B8680`). Iron, dressed stone and ore crystal are Stone shades that
differ only by roughness and metalness. There are no texture maps.

| Asset | Size (m, W x H x D) | Triangles (budget) | LOD1 | Collision | Materials |
|-------|---------------------|--------------------|------|-----------|-----------|
| `command_center` | 10.0 x 8.36 x 10.0 | 1280 (3000) | 458 | hull, 100 | 5 |
| `barracks` | 8.0 x 5.75 x 6.0 | 888 (2400) | 318 | hull, 80 | 5 |
| `resource_node` | 2.89 x 3.0 x 2.68 | 494 (1600) | 176 | hull, 92 | 3 |
| `rally_flag` | 1.39 x 3.0 x 0.7 | 196 (400) | 70 | box, 12 | 4 |
| `unit_marker` | 1.0 x 0.05 x 1.0 | 240 (300) | 86 | box, 12 | 3 |

`command_center_crimson`, `barracks_crimson` and `rally_flag_crimson` are the same meshes with Crimson in place
of Azure. The player's Human faction uses the Azure props. The Swarm opponent uses the Crimson variants, the same
red the RTS Battlefield panel draws it in. The third faction, Sentinel (Verdant), never appears in the demo
skirmish, so the kit has no Verdant variant. Adding one takes one more entry in the script's `FACTIONS` tuple.
`resource_node` is faction-neutral. `unit_marker` is the player's selection ring and comes in Azure only.

Every LOD1 is about 36% of its source (limit 40%), and every collision mesh has at most 200 triangles.
Exports live in `Assets/Models/RTS/Kit/` (`<asset>`, `<asset>_lod1`, `<asset>_collision`, OBJ + MTL):
metres, pivot at the ground-contact centre, +Y up, each prop's front facing +Z. `rts_kit.blend` keeps
every prop as an editable collection, and `provenance.json` hash-binds the script, library, source,
exports and license. `preview.png` is a Workbench render of the kit (fixed camera and lighting, 1600x900):
the Azure props and the resource node are in front, the Crimson variants behind. It is not hash-bound.

`RTSDemoPresentation::SyncKitProps` (`GameModules/SparkGameRTS/Source/Demo/RTSDemoPresentation.cpp`) runs
after every simulation advance and keeps the world in step with the skirmish, at 2.5 m per grid cell (a
building's 4 x 4 cell footprint is the command center's 10 x 10 m):
- each command center and barracks gets its mesh, with its front facing the map centre;
- each barracks gets a rally flag on the cell where its finished units appear;
- each resource node that still holds resources gets a crystal cluster;
- each selected unit gets a marker ring.

Props are removed when their building is destroyed, their node runs dry or their unit is deselected.
`Shutdown` removes all of them.

```sh
PYTHONHOME=/usr xvfb-run -a blender -b --factory-startup --python-exit-code 1 \
  --python tools/blender/author_rts_kit.py -- --repo .
python3 tools/blender/validate_kit.py Art/Blender/SparkGameRTS/provenance.json
```

Reruns are byte-identical for the exports, `.blend` and `provenance.json`. The preview PNG is not.
All 24 OBJ files load through the engine's `LoadOBJStaticMesh`. Every triangle is wound consistently with its
authored normal, and every submesh has a material assigned.

Known limits:
- The props are presentation only: no collider is attached, and the simulation still runs on its grid.
- The only buildings that get meshes are command centers and barracks. The kit has no mesh for the other six
  building types (factory, tech lab, supply depot, refinery, turret, starport), so they stay invisible in 3D.
  Units have no body mesh, only the selection ring.
- The unit marker's ring, chevrons and studs are 3-5 cm pieces, too small for the 2-4 cm chamfer, so their
  edges stay square. The flag cloth has a 1 cm chamfer, and the banners a 2 cm one.
- LOD1 decimation moves the ground contact: the resource node's LOD1 dips 1.6 cm below the ground plane, and the
  command center, barracks and rally flag LOD1 meshes float 1.3 cm above it.
- Nobody has reviewed the props in game with a GPU renderer yet. The headless run only confirms that the
  entities are placed.
