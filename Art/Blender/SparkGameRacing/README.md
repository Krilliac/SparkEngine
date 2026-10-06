# SparkGameRacing Circuit Racing kit

Five original trackside props for the racing module, authored by `tools/blender/author_racing_kit.py` on the
shared `tools/blender/spark_kit.py` library (see `Art/Blender/README.md`, "Per-module kits"). Art direction:
a sunlit circuit of asphalt, kerb stripes and a single sponsor accent for speed read. Long low extrusions,
rounded safety profiles and a repeated stripe rhythm. Every MTL diffuse is one of four palette colours
(asphalt `#2E2F31`, kerb red `#D63A2F`, kerb white `#F2F2EE`, sponsor cyan `#1FB5C9`); shades differ only by
roughness and metalness (matte concrete, rubber, painted kerb, white-painted steel). No texture maps.

| Asset | Size (m, W x H x D) | Triangles (budget) | LOD1 | Collision | Materials |
|-------|---------------------|--------------------|------|-----------|-----------|
| `barrier_segment` | 4.0 x 0.9 x 0.6 | 584 (700) | 210 | box, 12 | 4 |
| `traffic_cone` | 0.42 x 0.7 x 0.42 | 264 (300) | 94 | hull, 76 | 3 |
| `checkpoint_arch` | 12.0 x 6.0 x 1.4 | 980 (1800) | 352 | hull, 178 | 4 |
| `start_gantry` | 14.0 x 7.0 x 1.2 | 2076 (2600) | 746 | box, 12 | 5 |
| `tyre_stack` | 0.64 x 1.2 x 0.66 | 940 (1200) | 336 | hull, 116 | 4 |

Every LOD1 is 34-36% of its source (limit 40%), and every collision mesh is at most 200 triangles.
Exports live in `Assets/Models/Racing/Kit/` (`<asset>`, `<asset>_lod1`, `<asset>_collision`, OBJ + MTL):
metres, pivot at the ground-contact centre, +Y up, each prop's front facing +Z (the gantry's start lights
and sponsor board, the barrier's sponsor band). `racing_kit.blend` keeps every prop as an editable
collection, and `provenance.json` hash-binds the script, library, source, exports and license.
`preview.png` is a Workbench render of the kit (fixed camera and lighting, 1600x900); it is not hash-bound.

`RacingTrackSystem::PlaceTrackKit` (`GameModules/SparkGameRacing/Source/Track/RacingTrackSystem.cpp`)
dresses the active track whenever `LoadDemoTrack` runs with a world (module start, the editor track
selector, save restore). Each prop is posed against the centerline segment under it and set at that
segment's height:

- the finish checkpoint gets the start gantry across the track, facing oncoming cars, with a tyre stack
  outboard of each tower;
- every other checkpoint gets a checkpoint arch, facing oncoming cars, with a cone outboard of each foot;
- a barrier hazard (the Crossover Arena crossing) gets a barrier segment running along the traffic;
- an oil-slick hazard (Sunset Circuit) gets a warning cone on each side.

`RacingTrackSystem::Shutdown` and every track change remove the placed entities.

```sh
PYTHONHOME=/usr xvfb-run -a blender -b --factory-startup --python-exit-code 1 \
  --python tools/blender/author_racing_kit.py -- --repo .
python3 tools/blender/validate_kit.py Art/Blender/SparkGameRacing/provenance.json
```

Reruns are byte-identical for the exports, `.blend` and `provenance.json`; the preview PNG is not.
All fifteen OBJ files load through the engine's `LoadOBJStaticMesh` with every triangle wound consistently
with its authored normal and every submesh assigned a material.

Known limits:
- The props are set dressing only. Checkpoints remain trigger circles, the barrier hazard stays data-only,
  and no collider is attached.
- The gates use the authored metre sizes, sized for the 12-14 m `TrackWaypoint::width`. The surface query
  treats that width as a half-width, so a car running very wide can pass outside a gate's legs.
- The checkpoint arch's convex hull closes the opening, so a physical collider for it needs two leg boxes
  or a custom proxy. The gantry's collision is its bounding box for the same reason.
- The red start-light lenses are only coloured. `MeshRenderer::emissive` applies to a whole mesh, so the
  kit does not set it.
- If a world save captures the placed props, a restore can leave a second copy until the next track change.
- Nobody has reviewed the props in game yet.
