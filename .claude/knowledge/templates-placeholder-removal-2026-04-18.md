# Templates: placeholder removal → real, compilable modules

**Last updated:** 2026-04-18
**Type:** Decision
**Status:** Resolved

## Description

Converted every project template under `Templates/` from `{{PROJECT_NAME}}`-style
scaffolding into concrete, real, compilable `Spark::IModule` implementations.
No placeholder syntax anywhere in shipped template source. Template rename is
now handled at project-creation time by rewriting the template's concrete name
(e.g. `FPSStarter`) to the user's chosen project name.

## Context

- Applies to all five shipped templates: `EmptyProject`, `FPSStarter`,
  `MultiplayerArena`, `PlatformerKit`, `RPGStarter`.
- Affects three creation paths: `ProjectManager::CreateProjectFromTemplate`
  (editor UI), `Tools/spark-cli/spark_cli.py` (CLI), and manual
  `cp -r + sed` (documented in each template README).

## Details

**Before:**
- Template files contained `{{PROJECT_NAME}}` placeholders.
- Because `{{PROJECT_NAME}}` is not valid C++ syntax, clang-format rejected the
  files, each `.cpp` failed to compile on its own, and an earlier stopgap was
  a `Templates/.clang-format` with `DisableFormat: true` to hide the errors.
- Four of the five templates also called a non-existent `SPARK_REGISTER_MODULE`
  macro — would have failed to link even after placeholder substitution.

**After:**
- Each template's source uses its own concrete module class name:
  - `EmptyProject` → `EmptyProjectModule`
  - `FPSStarter` → `FPSStarterModule`
  - `MultiplayerArena` → `MultiplayerArenaModule`
  - `PlatformerKit` → `PlatformerKitModule`
  - `RPGStarter` → `RPGStarterModule`
- Each `GameModule.cpp` uses the real `SPARK_IMPLEMENT_MODULE(...)` + the
  shared `<Spark/ModuleDllMain.h>` include from the `ModuleRegistry` extraction.
- `ProjectManager::CopyTemplate` and `spark_cli.py` now rewrite every textual
  occurrence of the template directory's name to the user's project name in
  one pass (e.g. `FPSStarter` → `MyGame` across `.h`, `.cpp`, `.cmake`,
  `.json`, `.md`). If the user picks the same name as the template, the copy
  is the final artifact — no rewrite needed.
- `Templates/.clang-format` deleted — templates format cleanly as real C++.

## Solution / Summary

- `Tests/TestTemplatesCompile.cpp` was added to SparkTests. It #includes every
  template header and exercises each module's lifecycle (`OnLoad`, `OnUpdate`,
  `OnRender`, `OnUnload`) + `GetModuleInfo()`. If a template ever regresses
  into uncompilable state or stops matching its advertised name, the test
  binary fails to build or fails assertions at runtime. Five new tests added.
- `build/linux-gcc-release/bin/SparkTests`: 5636 tests pass, 0 fail, 124046
  assertions pass, ~36s runtime.
- clang-format over both CI scope and Templates/: 0 violations.

## Notes

- The only surviving `{{...}}` in our first-party tree is
  `//{{NO_DEPENDENCIES}}` at the top of `SparkEngine/Source/Core/resource.h`
  — that is Microsoft's Visual Studio resource-editor marker, consumed by
  `rc.exe`, not a user-facing placeholder. It stays.
- ThirdParty code (Jolt, tinyobjloader, etc.) contains its own `FIXME/XXX/TODO`
  markers — out of scope for this project.
- Documentation updated: `Templates/README.md`,
  `Templates/EmptyProject/README.md`, `docs/specs/plugin-abi-guide.md`,
  `wiki/getting-started/Creating-a-Game-Module.md`.
- Related: `.claude/knowledge/module-dllmain-extraction-2026-04-18.md`,
  `.claude/knowledge/preexisting-bugs-fixed-2026-04-18.md`.
