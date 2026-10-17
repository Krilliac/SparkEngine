# Reflected-World scene compatibility fixtures

Owner decision OD-03 applied to the reflected-World scene dialect used by editor
File > Open/Save and `-scene` (`Spark::LoadWorld` / `Spark::SaveWorld` in
`SparkEngine/Source/SceneManager/`). The dialect reads `version: 1` and the
pre-reflection editor `sceneVersion: 1` dialect, migrates the latter in memory,
writes `version: 1` only, and rejects any other version with an error that names
the version field, the file's value and the supported window.

The reflected dialect has only ever written `version: 1`, so the legacy editor
dialect is the only older format it reads.

| File | SHA-256 | Purpose |
|---|---|---|
| `v1-reflected-courtyard.sparkscene` | `38a97c2a27577f70b3c2dacc09fdb6e33a21bd8c15e065e9f13503395c08f11b` | Unmodified output of the production `Spark::SerializeWorld` on 2026-09-27 for a four-entity world: a parent/child `Transform` edge, `MeshRenderer`, `RigidBodyComponent` and `LightComponent` enums, `CollisionMaskComponent` masks and a main `Camera`. Must load every declared field and re-save to the same entities. |
| `legacy-editor-sceneVersion1.sparkscene` | `6221b4d8e32b6d328971e4c3174199f905748d38d63e97a773c3c9682e6014be` | Hand-written in the pre-reflection editor schema the loader supports (`sceneVersion`, inline component values, `DirectionalLight`, `CharacterController`, Camera `nearClip`/`farClip`, MeshRenderer `mesh`). No writer for this dialect survives in repository history, so this file is not writer output. Must migrate in memory and re-save as `version: 1` only. |
| `v2-future.sparkscene` | `536defb88c02f6dd5ceb5959ac546fb8e29d6958c3e3cee5eeb08c986b1e519f` | A document declaring `version: 2`, which no build has written yet. Must fail closed with a versioned error and leave the caller's world untouched. |

Tests (`SceneMigration_Reflected*` in `Tests/TestReflectedSceneCompatibility.cpp`,
CTest `SparkSceneCompatibilityTests`) read these files in place through the
production `LoadWorld` path, check they are never rewritten and that no `.bak`,
`.tmp` or `.bak.tmp` file appears beside them.

The digests are of the LF bytes committed here. `.gitattributes` marks these
fixtures `-text`, so a Windows checkout with `core.autocrlf=true` keeps the same
bytes and digests.
