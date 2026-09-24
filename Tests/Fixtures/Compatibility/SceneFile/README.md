# SceneFile compatibility fixtures

Owner decision OD-03: the editor scene loader (`SceneSerializer`, SceneFile JSON)
reads the current format (N = v2) and the previous one (N-1 = v1), migrates v1 in
memory, writes v2 only, and rejects any other version with an error that names the
file's version and the supported window.

Both files are the unmodified output of the v1 `SceneSerializer::SaveScene` JSON
write path at commit `3ef790f42f84f0711bb2693b4d22714123e041f7` (the last commit
before the v2 schema-tagged component payloads). The generator compiled that
commit's `SparkEditor/Source/SceneSystem` sources with GCC on Linux; only the
logging and validation macros were replaced by no-op shims, which do not affect the
bytes written. The odd whitespace (closing brackets on the same line) is the v1
`JSONWriter`'s real output.

| File | SHA-256 | Purpose |
|---|---|---|
| `v1-hierarchy.sparkscene` | `6e10b4f468cc0e2ad7547ff09ac9e27b9fdc3b4e7a192cd2a2044b89b54b3193` | Two objects with a parent/child edge, marker `Transform` components (one disabled), environment, default camera, and an asset reference. Must migrate to v2 with every declared field intact. |
| `v1-raw-light-payload.sparkscene` | `3d6e637b736ce80b60c46e86f5d3cf7092127e68a2fc6c04fa8fff5391c675c8` | The same scene plus a `Light` component whose `data` is v1's hex-encoded raw C++ object image (including uninitialised padding bytes). Must fail closed: raw object images have no schema and are never decoded. |

Tests (`SceneMigration_*` in `Tests/TestSceneSerializerReal.cpp`) read these files
in place and check they are never rewritten.
