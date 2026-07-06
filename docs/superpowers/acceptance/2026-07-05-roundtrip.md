# D2 Acceptance: Editor Save -> Runtime Render Round-Trip

Date: 2026-07-05
Branch: `claude/editor-ecs-roundtrip`

## What this proves

SparkEditor's real save path (`EditorUI::SaveCurrentScene` -> `Spark::SaveWorld`)
writes a full-fidelity reflected-JSON scene of the live, seeded ECS `World`, and
SparkEngine's runtime `-scene` loader (`Spark::LoadWorld` + `Spark::RenderWorldBasic`)
loads that exact file and renders the entity it describes. This is the
end-to-end round-trip for the editor<->engine ECS scene format.

## What was added

- `SparkEditor/Source/main.cpp`: parses `--save-scene <path>`. When present,
  after `EditorApplication::Initialize()` succeeds (which seeds `m_world` with
  the demo "Soldier" entity inside `EditorUI::SetGraphicsDevice`, before any
  project browser or interactive loop), it calls `EditorUI::SaveCurrentScene(path)`
  via a new `EditorApplication::GetUI()` accessor, then shuts down and exits —
  without ever entering the interactive main loop.
- `SparkEditor/Source/Core/EditorApplication.h`: added `EditorUI* GetUI() const`.
- `Tools/accept_shot.cfg`: `t3 gfx_screenshot Screenshots/acceptance.png`.

## Commands run (from CWD `D:\SparkEngine`)

Build:
```
powershell -File "C:\Users\natew\.claude\scripts\build.ps1" "cmake --build build --config Release --target SparkEditor"
powershell -File "C:\Users\natew\.claude\scripts\build.ps1" "cmake --build --preset windows-release --target SparkEngine"
```
(The engine rebuild was required: `build\windows-release\bin\SparkEngine.exe` was
stale — built at 16:20, before the `-scene` feature landed in commit `80c0b862`
at 20:42. The brief's assumption that it was already up to date was wrong; the
stale binary silently skipped scene loading with no error, producing a blank
blue screenshot. Rebuilding picked up `-scene` support and fixed it.)

Editor save-and-exit:
```
build\bin\SparkEditor.exe --save-scene D:/SparkEngine/acceptance.scene.json
```
Exit code 0. Produced `D:\SparkEngine\acceptance.scene.json`.

Runtime render:
```
build\windows-release\bin\SparkEngine.exe -scene D:/SparkEngine/acceptance.scene.json -game D:/SparkEngine/README.md -exec Tools/accept_shot.cfg -test-seconds 6 -window-size 1280 720
```
Exit code 0. Log confirms: `[-scene] Loaded 'D:/SparkEngine/acceptance.scene.json' (1 entities)`.
The bogus `-game D:/SparkEngine/README.md` correctly fails module load with
`error 193` (not a valid PE) and falls back to engine-only mode, avoiding the
known landmine where a no-`-game` run auto-scans `build/bin` for `*Game*.dll`.

## Scene JSON (full reflected format, not lossy)

`D:\SparkEngine\acceptance.scene.json`:
```json
{
  "entities": [
    {
      "components": [
        {
          "fields": {
            "position": "0.000000,0.000000,0.000000",
            "rotation": "0.000000,0.000000,0.000000",
            "scale": "1.000000,1.000000,1.000000"
          },
          "type": "Transform"
        },
        {
          "fields": {
            "castShadows": "true",
            "materialPath": "",
            "meshPath": "Assets/Models/MMOFPS/characters/soldier.obj",
            "receiveShadows": "true",
            "visible": "true"
          },
          "type": "MeshRenderer"
        }
      ],
      "id": 0,
      "name": "Soldier",
      "parent": -1
    }
  ],
  "version": 1
}
```
Confirmed via grep: `"MeshRenderer"` (1 hit), `"soldier.obj"` (1 hit), `"Transform"` (1 hit).

## Screenshot

`D:\SparkEngine\Screenshots\acceptance.png` (1280x720) was viewed directly.
It shows a dark, unlit humanoid character mesh — the Soldier's geometry
(head/torso/limbs plus an arm-mounted shape consistent with the soldier model)
— rendered against the engine's clear-color blue background, centered in
frame. The mesh is unlit/silhouetted (no lighting was set up for this bare
`-scene` render path) but its shape is unambiguously a rendered 3D character
model, not an empty frame. This is the entity the editor authored and saved,
loaded and rendered by the runtime from the reflected JSON alone.

## Verdict

**Round-trip verified.** Editor Save (`--save-scene`, real `SaveWorld` path) ->
reflected JSON (`Transform` + `MeshRenderer` on the Soldier entity) -> runtime
`-scene` load + render (`LoadWorld` + `RenderWorldBasic`) produces a visible
rendered mesh matching the editor-authored entity. No fabrication: the initial
run against the stale engine binary produced a blank screenshot and is called
out above as a real blocker that was found and fixed (engine rebuild), not
hidden.
