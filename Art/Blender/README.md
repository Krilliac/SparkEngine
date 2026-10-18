# Blender asset quality work

Editable Blender sources belong here, outside the installed runtime asset tree.
Exported game files keep their existing paths under `Assets` and `Templates`.
Authoring and inspection use Blender 4.0.2. The repository's `LICENSE` applies to
new original assets; existing third-party attributions remain authoritative.

## Baseline inspection

`asset-audit-before.json` records the original asset bytes at
`ff951c19a5e12be6ec3724696af6fe030070e8c4`: 222 OBJ models, 367 PNG images,
125 material libraries, and 170 WAV files. Blender loaded the models, images,
and audio; material-library definitions and WAV headers were also inspected.
Other files, including the four SVG/GIF/MP4/WebM branding assets, are inventoried
separately and are not certified by this inspection.

```sh
blender --background --factory-startup --python-exit-code 1 \
  --python tools/blender/audit_assets.py -- --root . --output /tmp/asset-audit.json
```

On the current Linux host, use the distro `python3-numpy` package and prefix the
command with `PYTHONHOME=/usr` to select Blender's compatible system Python.
Standard Blender distributions normally include their required Python modules.

An inspection is not an artistic quality score or an engine-rendering result.
Blender can synthesize missing attributes, so successful import alone does not
prove authored normals, UVs, material references, or gameplay compatibility.
The report flags near-zero-area faces at a stated threshold. Open surfaces and
flat-color textures can be intentional and must be reviewed before modification.
Audio inspection does not certify mastering, clipping, loop seams, or perceived
quality. No D3D11, animation, installed-package, or release readiness claim follows
from this report.

## Improvement order

1. Replace the sixteen MMO placeholder props with distinct Blender-authored
   silhouettes and coherent materials, preserving original filenames and bounds.
2. Verify source attributes and material references, then exercise actual engine
   import and inspect before/after renders.
3. Investigate the eleven MMOFPS meshes with near-zero-area faces before repair.
4. Review remaining models, textures, branding motion, and audio in their actual
   uses; retain intentional collision proxies, primitive examples, and utility
   textures.

This is an ongoing quality pass. The baseline audit does not mean every asset has
been improved or approved for release.

## Per-module kits

Each game module's props are authored by one script built on the shared library
`tools/blender/spark_kit.py`:

| Item | Location |
|------|----------|
| Authoring script | `tools/blender/author_<module>_kit.py` |
| Editable source | `Art/Blender/<Module>/<module>_kit.blend` |
| Exports | `Assets/Models/<ModuleShort>/Kit/<asset>.obj` + `.mtl`, `<asset>_lod1.obj`/`.mtl`, `<asset>_collision.obj`/`.mtl` |
| Provenance | `Art/Blender/<Module>/provenance.json` |
| License | Spark Open License 1.0 (repository `LICENSE`) |

Exports use the `Assets/Models/ModuleKits` convention: meters, ground-level pivot,
triangulated OBJ with UVs and normals, Y-up with `forward_axis='Z'` (props face
Blender -Y, which becomes OBJ -Z). Colors live in the MTL files. `<asset>_lod1` is
a Decimate (collapse) reduction at the ratio recorded in provenance, and
`<asset>_collision` is a convex hull (or box) under a recorded triangle cap.

```sh
PYTHONHOME=/usr blender -b --factory-startup --python-exit-code 1 \
  --python tools/blender/author_<module>_kit.py -- --repo .
python3 tools/blender/validate_kit.py Art/Blender/<Module>/provenance.json
```

Reruns are byte-identical. Blender 4.0 writes memory addresses into `.blend` files,
so an unchanged scene (same `blend.scene_sha256` content digest) keeps its existing
`.blend` rather than being re-saved. The validator checks recorded hashes, OBJ
attributes, bounds, triangle budgets, LOD reduction, collision caps, and material
resolution. It does not judge art quality or engine rendering. Engine import and
in-game review are still separate steps.
