# Editor-state compatibility fixtures

SAVE-230 fixtures for the editor's project document, `<Project>.sparkproject`
(`SparkEditor::ProjectManager`, `SparkEditor/Source/Core/ProjectManager.cpp`).

## Version window

- The editor reads `projectFileVersion` 1 and writes 1 only
  (`ProjectManager::kProjectFileVersion`).
- A document without `projectFileVersion` is the legacy dialect. Hand-written
  documents and `spark.project.json` module documents use it, and it loads as
  version 1. Every writer since `ce5a3d701` and every `Templates/*/*.sparkproject`
  emits `"projectFileVersion": 1`.
- A larger version fails closed. `OpenProject` returns false with an error that
  names the file, the declared version and the supported window ("reads 1 and
  writes 1"), and it tells the user to use a newer SparkEditor. The open project
  is left unchanged, and the retained `.bak` is not used: loading an older copy
  and saving over the newer document would discard its data.
- Version 0, a non-integer version, an empty file and a document that is not one
  complete JSON object (a truncated write) are rejected. The loader then falls
  back to `<Project>.sparkproject.bak`, the previous-good copy that every project
  save keeps through `SaveFileDurability::WriteFileAtomically`. It reports both
  reasons when the backup is unusable too.

## N-1 policy

The project document has only ever been written as version 1, so there is no
older version to migrate. The fixture set is therefore version 1 (N), which
loads in place without a rewrite, plus a version 2 document that must fail
closed. The reflected-scene dialect takes the same stance
(`Tests/Fixtures/Compatibility/ReflectedScene/README.md`). Promoting the SAVE-230
"N-1 fixtures migrate" criterion on this basis needs an owner decision.

| File | Purpose |
|---|---|
| `v1-project/V1Project.sparkproject` | Hand-written in the exact layout `ProjectManager::SaveProjectFile` emits. It carries a template identity, distinct default and last-opened scenes, non-zero timestamps, two modules and two scenes. It must open with every declared field intact and must not be rewritten by the open. |
| `v2-future-project/Future.sparkproject` | Declares `projectFileVersion: 2` and adds a field no current build knows. It must fail closed with a versioned error and leave the previously open project in place. |

Tests (`EditorStateMigration_*` in `Tests/TestEditorStateCompatibility.cpp`,
CTest `SparkEditorStateCompatibilityTests`) copy each fixture into a scratch
project directory before opening it. Opening a project adds any missing build
scaffold files next to the document, so the committed fixtures are never opened
in place. The tests check the copy's bytes rather than a pinned digest, so a
Windows checkout that converts line endings still passes.
