# SparkEngine Default Assets

This directory contains procedurally generated placeholder assets for
SparkEngine development. They are intentionally minimal (small, solid-color
textures and simple synthesized audio) so the engine and editor can run
without missing-asset errors.

## Regenerating

```bash
python3 tools/generate_default_assets.py
```

## Directory Layout

```
Assets/
  Audio/            — Placeholder WAV sounds (gunshot, footstep, UI, etc.)
  Materials/        — PBR material definitions (JSON)
  Models/           — OBJ mesh files (weapons, props, characters)
  Scenes/           — Scene files (.scene JSON)
  Scripts/          — AngelScript game scripts
  Textures/
    Default/        — Engine defaults (checkerboard, white, black, flat normal, grid, UV test)
    Decals/         — Bullet hole, scorch mark, blood splatter
    Terrain/        — Grass, dirt, rock, sand, snow
    Sky/            — Sky gradient
```

## Replacing with Production Assets

These placeholders are meant to be replaced with real assets. Some
recommended free / CC0 asset sources:

| Source | URL | Content |
|--------|-----|---------|
| ambientCG | https://ambientcg.com | PBR textures (CC0) |
| Poly Haven | https://polyhaven.com | Textures, models, HDRIs (CC0) |
| Kenney | https://kenney.nl | Game assets, UI, audio (CC0) |
| Freesound | https://freesound.org | Audio samples (various CC licenses) |
| OpenGameArt | https://opengameart.org | 2D/3D art, audio (various licenses) |
| Quaternius | https://quaternius.com | Low-poly 3D models (CC0) |
| Mixamo | https://mixamo.com | Character animations (free with Adobe account) |
| Sonniss GDC Bundle | https://sonniss.com/gameaudiogdc | Professional SFX (royalty-free) |

When replacing textures, match the filenames or update the corresponding
material JSON and engine references.
