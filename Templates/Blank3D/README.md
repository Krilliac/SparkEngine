# Blank3D

A ready-to-edit 3D composition study with a visible primitive gallery, ground, two-light rig, live camera HUD, and six-axis fly controls.

This standalone installed-SDK example provides a pure fly-camera state with public movement, look, speed, reset, and timing methods. The reflected scene supplies a rotated hero cube, contrasting scale studies, and a three-piece arch that demonstrate naming, transforms, composition, lighting, shadows, and reusable primitives without engine-private headers.

## Configure and build

```powershell
cmake -S . -B build -DSparkEngine_DIR="<sdk>/lib/cmake/SparkEngine"
cmake --build build --config RelWithDebInfo
```

Load `Blank3D.dll` through `spark.modules.json`; the starter scene is `Scenes/Default.sparkscene`.

## Controls and first edit

- `W`, `A`, `S`, `D`: fly on the camera-relative horizontal plane
- `Q`, `E`: descend and ascend
- Hold right mouse and move: yaw and pitch the camera
- `R`: reset the camera to the authored overview

Start by duplicating `Starter Cube`, changing its transform, and renaming it. The `Scale Study` pair demonstrates non-uniform scale; the three `Composition Arch` entities demonstrate a small modular assembly. All six mesh entities intentionally use the built-in cube so a copied project stays immediately packageable.

## Live playtest checkpoints

1. The hero cube, scale studies, and complete arch are visible from the default overview; every mesh rests on the ground.
2. Diagonal movement remains normalized, `Q`/`E` change altitude, and right-mouse look clamps pitch before inversion.
3. `R` restores the exact overview pose, while the transform-gizmo HUD remains camera-relative.

## License and assets

See the SparkEngine root license. The asset manifest records repository-original HUD artwork and the built-in procedural primitives used by the scene.

## Runtime asset sheet

`Assets/blank3d_runtime_sheet.png` is the normalized 3x3 sprite sheet consumed by the live camera-control HUD. `Assets/runtime_sheet.json` gives nine stable, named 418x418 source rectangles with transparent gutters. The larger atlas remains concept and reference art; other sheet consumers should use the descriptor rather than guessing coordinates.
