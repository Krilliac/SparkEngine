# SparkGameARPG Action RPG Dungeon kit

Four original props for the ARPG crypt, authored by `tools/blender/author_arpg_kit.py` on the shared
`tools/blender/spark_kit.py` library (see `Art/Blender/README.md`, "Per-module kits"). Art direction:
obsidian crypts lit by arcane violet and embers, where loot pops against dark ground. Jagged spires,
cracked slabs, rune-cut bands and glowing inlays. Every MTL diffuse is one of four palette colours
(obsidian `#1E1B24`, bone `#D8CFB8`, arcane violet `#7B4FD1`, ember `#D9502B`); shades differ only by
roughness and metalness (polished obsidian, matte slab, blackened iron, bone, pale gilt). No texture maps.

| Asset | Size (m, W x H x D) | Triangles (budget) | LOD1 | Collision | Materials |
|-------|---------------------|--------------------|------|-----------|-----------|
| `spike_trap` | 2.0 x 0.53 x 2.0 (2 x 2 m plate) | 1196 (1400) | 426 | hull, 104 | 5 |
| `destructible_urn` | 0.64 x 0.9 x 0.61 | 676 (900) | 242 | hull, 188 | 5 |
| `loot_pile` | 1.2 x 0.86 x 1.19 | 994 (1600) | 338 | hull, 132 | 5 |
| `portal_gate` | 4.01 x 4.0 x 2.01 | 1072 (2600) | 384 | hull, 152 | 5 |

Every LOD1 is 34-36% of its source (limit 40%), and every collision mesh is at most 200 triangles.
Exports live in `Assets/Models/ARPG/Kit/` (`<asset>`, `<asset>_lod1`, `<asset>_collision`, OBJ + MTL):
metres, pivot at the ground-contact centre, +Y up, each prop's front facing +Z. `arpg_kit.blend` keeps
every prop as an editable collection, and `provenance.json` hash-binds the script, library, source,
exports and license. `preview.png` is a Workbench render of the kit (fixed camera and lighting, 1600x900);
it is not hash-bound.

`ARPGDungeonSystem::PlaceCryptKit` (`GameModules/SparkGameARPG/Source/Dungeon/ARPGDungeonSystem.cpp`)
dresses the crypt entry room when the module loads with a world. The hero starts at the origin looking
down -Z. Three urns flank the approach, the spike trap guards the aisle at z = -7, the loot pile sits by
the far wall, and the portal gate at z = -14 closes the room, facing back toward the hero.
`ARPGDungeonSystem::Shutdown` removes all six entities.

```sh
PYTHONHOME=/usr xvfb-run -a blender -b --factory-startup --python-exit-code 1 \
  --python tools/blender/author_arpg_kit.py -- --repo .
python3 tools/blender/validate_kit.py Art/Blender/SparkGameARPG/provenance.json
```

Reruns are byte-identical for the exports, `.blend` and `provenance.json`; the preview PNG is not.
All twelve OBJ files load through the engine's `LoadOBJStaticMesh` with every triangle wound consistently
with its authored normal and every submesh assigned a material.

Known limits:
- The props are set dressing only. The urns are not linked to the `arpg_urn` fracture pattern that
  `ARPGEngineSystems` registers, the trap has no damage trigger, and the portal does not start the floor
  descent. No collider is attached.
- The convex-hull collisions are coarse. The trap's hull is reduced to fit the triangle cap, so the
  tips of the tallest spikes stand up to 12 cm outside it. The portal's hull closes the opening, so
  walking through it needs a trigger volume or a custom proxy.
- Decimation lifts the lowest LOD1 vertices of the trap and portal 1-1.3 cm off the ground plane.
- Flush inlays (runes, glyphs, cracks) are 1 cm thick, too thin for the 2-4 cm chamfer, so their edges
  stay square. The sword blade uses a 6 mm chamfer for the same reason.
- The glowing inlays and membrane are only coloured violet or ember. `MeshRenderer::emissive` applies to
  a whole mesh, so the kit does not set it.
- The loot pile dips 2 mm below the ground plane where a tilted coin touches it.
- Nobody has reviewed the props in game yet.
