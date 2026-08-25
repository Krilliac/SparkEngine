# SparkEngine model pipeline

This directory contains the deterministic source pipeline for SparkEngine's
authored starter models. Blender is the source tool; shipping assets are
triangulated OBJ/MTL because that is the format supported by the Windows live
renderer today.

## Requirements

- Blender 5.2.0 LTS (`fbe6228777e7`, 2026-07-14) for byte-stable exports.
- PowerShell 7 on Windows.
- Python 3.10 or newer for the standalone validator invoked after Blender.

Set `SPARK_BLENDER` to a Blender executable, or install the portable build at
`D:\Applications\Blender 5.2 Portable\blender.exe`.

## Generate and validate

```powershell
pwsh -File Tools/model_pipeline/generate_assets.ps1
python Tools/model_pipeline/validate_models.py --repo-root .
pwsh -File Tools/model_pipeline/capture_template_screenshots.ps1
```

Pass `-Name platformer_kit` (or another listed case) to refresh one runtime
capture without launching every template. `-PreviewRoot <path>` redirects
captures for path/automation testing.

The authored packs cover FPS, MMO, local multiplayer arena, platformer, RPG,
third-person adventure, and top-down action templates. Blank3D and EmptyProject
remain intentionally minimal composition surfaces and use built-in primitives.

Generation is deterministic on the pinned Blender build: all geometry is built
from explicit dimensions, materials use fixed colors, meshes are triangulated,
transforms are baked, and Blender's set-like UV table and face blocks are
canonically sorted and remapped before hashing. The manifest records Blender version, OBJ and MTL
SHA-256 hashes, and meter-space bounds. The export contract is Y-up, +Z
forward, meters, with a ground-level pivot, authored normals, and UV0 where
Blender supplies it.

The generated contact sheets under `docs/images/model-pipeline/` are the visual
review surface for each collection. Runtime integration still needs an editor
or game screenshot because an offline render cannot prove importer behavior.

## Ownership

The generator, generated models, and renders are original project assets and
are distributed under the repository's Spark Open License 1.0.
