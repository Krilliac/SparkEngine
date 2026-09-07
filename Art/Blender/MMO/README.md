# Blender-authored MMO props

Sixteen distinct stylized props replace the original primitive placeholders.
Exports retain every existing OBJ path and original axis-aligned bounds. The
kit uses authored UVs, normals and diffuse material colors without external
texture dependencies, with at most 5,000 triangles per model. New original
assets use the repository root Spark Open License 1.0.

## Reproduce and verify

Use Blender 4.0.2 and a Git checkout retaining baseline commit
`ff951c19a5e12be6ec3724696af6fe030070e8c4`. A shallow clone must fetch that
object first. Keep the supplied `provenance.json`: it records the original
baseline; without it the authoring script uses HEAD as the baseline. Run the
full command below. The development `--only` option replaces the scene and
provenance with a partial set and is not a complete-kit regeneration.

```sh
blender --background --factory-startup --python-exit-code 1 \
  --python tools/blender/author_mmo_props.py -- --repo .
python3 tools/blender/validate_mmo_props.py --root .
python3 Tests/Tools/test_blender_mmo_assets.py
```

On this Linux host, prefix Blender with `PYTHONHOME=/usr`; standard Blender
distributions normally bundle their compatible Python. OBJ/MTL bytes are the
reproducibility target; editable `.blend` metadata may vary. `props.blend`
contains all sixteen named editable collections with recorded runtime bounds.
`provenance.json` binds the author script, source, exports and root license by
SHA-256. The legacy MMO generator validates and preserves these models before
writing anything; invalid or absent curated exports stop generation.

## Evidence and limits

Sixteen focused tests load these files through the production CPU MeshAsset
implementation: 560,148 assertions pass. Source checks cover triangle area,
normal orientation, UVs, referenced-vertex bounds, material dependencies and
provenance; nine Python regressions include legacy-generator preservation.
These are focused import results, not a full engine suite or Windows test.

The before/after review uses actual exported OBJ files rendered by Blender
Cycles on CPU with the same camera and lighting and diffuse colors only.
It does not establish D3D11 appearance, gameplay collision, performance,
installed-package behavior, animation or release readiness. The broader
asset audit and outstanding work are recorded in ../README.md.

Independent regeneration in a separate baseline checkout produced byte-identical
OBJ and MTL files for all sixteen props (32 files). Reviewed comparison sheets:
[page 1](preview-1.jpg), [page 2](preview-2.jpg). Some original placeholders
render poorly or disappear because of invalid authored normals; the comparison
retains those source bytes instead of silently repairing the baseline.
