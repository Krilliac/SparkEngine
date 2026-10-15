# Malformed content fixtures

Provenance: authored for SparkEngine (ENG-220), written by hand and by a few lines of Python
(`truncated.glb`). They contain no third-party data and carry no license obligation beyond the
repository license. Each is a few dozen bytes; none is derived from a shipped asset.

Consumer: `Tests/PackageSmoke/RunInstalledMalformedContent.cmake`, registered as
`FPSPackage_MalformedContentDiagnostics`. It copies these files outside the package and passes
each to the installed `SparkEngine.exe -headless -scene <file>`, the only installed entry point
that accepts a content path.

What the installed host does with a content path: `-scene` runs the reflected-scene loader
(`LoadWorld` in `SceneManager/ReflectedScenePersistence.cpp`). It does not decode glTF or
material files; a scene's mesh and material references are only checked for existence
(`SPARK_SCENE_ASSETS`). So the GLB and material fixtures prove that a corrupt binary or JSON
asset handed to the content path fails the launch with a named reason, not that the glTF
importer or the material loader rejects them (those are covered by the `GLTF_*` and
`BasicMaterial_*` SparkTests families).

| File | Defect | Expected diagnostic substring |
|------|--------|-------------------------------|
| `valid-empty.sparkscene` | none (positive control) | `SPARK_SCENE_LOADED entities=0` |
| `truncated-scene.sparkscene` | JSON ends inside an entity | `scene could not be read` |
| `unsupported-version.sparkscene` | `version` 999 | `scene 'version' 999 is unsupported` |
| `missing-version.sparkscene` | no `version` field | `declares no 'version' field` |
| `corrupt-material.json` | material JSON cut mid-array | `scene could not be read` |
| `truncated.glb` | GLB header and JSON chunk header, chunk body cut off | `scene could not be read` |

The substrings come from `DeserializeInto` and `CheckSceneVersion` in
`SparkEngine/Source/SceneManager/`. Changing those messages means changing the table in the
runner.
