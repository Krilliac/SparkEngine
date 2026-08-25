# SparkEngine module model kits

These repository-original Blender-authored props give the ARPG, Racing, RTS,
and OpenWorld examples recognizable gameplay landmarks instead of placeholder
primitives. The Windows runtime consumes the triangulated OBJ/MTL files.

Every named prop has three forms:

- `<name>.obj` is the detailed near-camera model.
- `<name>_lod1.obj` is the simplified distance model.
- `<name>_collision.obj` is a simplified gameplay collision proxy; decorative
  overhangs and effects may sit outside it.

All meshes use meters, a ground-level pivot, and SparkEngine's Y-up,
positive-Z-forward export convention. The validator permits up to 5 mm of
procedural-surface tolerance around the ground plane. Colors live in the
adjacent MTL files, so the kits have no external texture dependency.

Regenerate the assets and their review renders with:

```powershell
pwsh -File Tools/model_pipeline/generate_assets.ps1
```

The source-of-truth construction code is
`Tools/model_pipeline/generate_starter_models.py`. Contact sheets live under
`docs/images/model-pipeline/module_*.png`; collision proxies are deliberately
shown there in white so their footprint can be reviewed alongside both visual
LODs.
