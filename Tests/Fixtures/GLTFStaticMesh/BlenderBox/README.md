# Blender-authored static box

This original procedural fixture is authored for SparkEngine and distributed under the repository's [Spark Open License 1.0](../../../../LICENSE). No third-party meshes, materials, textures, fonts, or generated imagery are used. `authored_box.blend` preserves the editable source; `author.py` records the entire authoring and export operation; `authored_box.glb` is the exported CPU import fixture. `provenance.json` binds their SHA-256 hashes, Blender version, geometry contract, and exact export options.

## Authored contract

- Blender 4.0.2, glTF exporter `Khronos glTF Blender I/O v4.0.44`.
- One box with dimensions `(2, 4, 6)` and center `(1, 2, 3)` in Blender's Z-up coordinates.
- Location, rotation, and scale applied to the mesh; no node transform is needed to recover its geometry.
- Six outward-facing flat-shaded quads, exported as twelve triangles with 24 split vertices and 36 indices.
- Each face uses the explicitly ordered UV rectangle `(0.125, 0.25)` to `(0.875, 0.625)`.
- glTF Y-up conversion maps `(x, y, z)` to `(x, z, -y)`, giving AABB minimum `(0, 0, -4)` and maximum `(2, 6, 0)`.
- glTF export flips V, so the exported UV rectangle spans `(0.125, 0.375)` to `(0.875, 0.75)`.
- Only `POSITION`, `NORMAL`, `TEXCOORD_0`, and unsigned triangle indices are exported. There are no materials, textures, skins, animations, morph targets, or required extensions.

The production `GLTFStaticMesh_LoadsBlenderAuthoredStaticBox` test in `Tests/TestGLTFStaticMeshLoader.cpp` imports this GLB through the real `LoadGLTFStaticMesh()`. It checks transformed bounds, every flat normal, each geometric corner's UV association on all six faces, index bounds, oriented triangle area/winding, and referenced vertices. This is CPU static-mesh interoperability evidence, not a rendered D3D11 image, scene-hierarchy, skeletal, animation, or installed-package qualification.

## Regenerate

From the repository root, using Blender 4.0.2:

```sh
blender --background --factory-startup --python-exit-code 1 \
  --python Tests/Fixtures/GLTFStaticMesh/BlenderBox/author.py
```

This rewrites the fixture's `.blend`, `.glb`, and provenance file. To inspect an independent regeneration without replacing tracked artifacts, append `-- --output-dir /path/to/fresh-directory`. The `.blend` is compressed and may contain varying save metadata; byte-identical `.blend` regeneration is not required. Independently regenerated GLB bytes matched during this change's validation.

On the Linux development host used here, embedded Python initially selected an unrelated runtime and lacked `_ctypes`/NumPy. Running the distro Blender with `PYTHONHOME=/usr` and the distro `python3-numpy` package restored the proper environment. Standard Blender distributions include their own Python/NumPy; the Linux workaround is not a general prerequisite. Blender emitted an optional Draco-library warning, but Draco is explicitly disabled and the export completed successfully.
