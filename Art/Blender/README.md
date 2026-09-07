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
