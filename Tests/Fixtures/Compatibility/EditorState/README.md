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

## Editor layouts (`Layouts/`)

`SparkEditor::EditorLayoutManager` (`SparkEditor/Source/Core/EditorLayoutManager.cpp`)
keeps named panel layouts as `<EditorData>/Layouts/<name>.json`.

- It reads and writes `"version": 1` (`EditorLayoutManager::kLayoutFormatVersion`).
  A file without `"version"` is the legacy dialect, which is identical to version 1.
- A larger version fails closed. `LoadLayout` returns false, `GetLastError()`
  names the file, its version and the supported version ("reads layout version 1
  only"), and no panel changes.
- The whole file is parsed before any panel is applied. A malformed or truncated
  panel, or a file cut after its panels array, fails the load without changing a
  panel. The old reader applied every panel it had parsed and then reported
  success.
- Saves go through `SaveFileDurability::WriteFileAtomically` (`<name>.json.tmp`,
  then rename), so a failed or interrupted save leaves the previous layout file
  intact. No `.bak` is kept, because a layout can be recreated.

| File | Purpose |
|---|---|
| `Layouts/v1-layout.json` | Hand-written in the exact layout `EditorLayoutManager::WriteLayoutFile` emits, with three panels that differ in every field. It must apply every declared value, and it must still load when the `"version"` line is removed (legacy dialect). |
| `Layouts/v2-future-layout.json` | Declares `"version": 2`. It must fail closed with a versioned error and leave every panel and the current layout name unchanged. |

The layout tests read these files in place, because loading a layout never
writes. They check that neither file is rewritten and that no `.tmp` sibling
appears.

Tests (`EditorStateMigration_*` in `Tests/TestEditorStateCompatibility.cpp`,
CTest `SparkEditorStateCompatibilityTests`) copy each fixture into a scratch
project directory before opening it. Opening a project adds any missing build
scaffold files next to the document, so the committed fixtures are never opened
in place. The tests check the copy's bytes rather than a pinned digest, so a
Windows checkout that converts line endings still passes.
