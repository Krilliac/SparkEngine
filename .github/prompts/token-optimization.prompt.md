# AI Token Optimization Guide

How to use SparkEngine's prompt system efficiently. Load minimum context per task.

## Hierarchical Loading

```
Layer 1: copilot-instructions.md    ← Always loaded (shared context)
Layer 2: One domain prompt           ← Per task
Layer 3: Specific file references    ← Only as needed (#file: or @file)
```

**Never load all domain prompts at once.** Pick the one matching your task.

### Which Prompt to Load

| Task | Load |
|------|------|
| ECS components, entity lifecycle, engine init | `engine-core` |
| DX11 rendering, shaders, post-processing | `graphics-rendering` |
| Audio playback, physics bodies, collisions | `audio-physics` |
| Game logic, AI, animation, weapons, saves | `gameplay-systems` |
| Editor panels, ImGui, debugging, profiling | `editor-tools` |
| CMake, CI/CD, tests, dependencies | `build-test` |
| Console commands, AngelScript, modding | `console-scripting` |

## Token-Saving Principles

1. **No duplication** — shared info lives only in `copilot-instructions.md`
2. **Terse formatting** — tables/bullets over prose
3. **Prefix ordering** — `[role] → [project context] → [domain context] → [task]` maximizes prefix caching
4. **Reference over inline** — `#file:SparkEngine/Source/Physics/PhysicsSystem.h` instead of pasting
5. **Scope context** — load only the relevant domain prompt
6. **Prune stale context** — in multi-turn sessions, summarize and drop raw code
7. **Exclude from indexing**: See `.promptignore` at repo root for the full exclusion list (ThirdParty/, Shaders/Compiled/, build/, .vs/, .vscode/, and more). Tools and scripts should read `.promptignore` to automatically filter excluded paths.

## Automated Enforcement

Run `./tools/validate-prompts.sh` (or `--ci` for non-zero exit on errors) to check:

1. **Anti-drift** — verifies source paths referenced in prompts still exist in the codebase
2. **Overload prevention** — ensures prompt files follow single-domain loading guidance
3. **Stale detection** — flags references to removed files, classes, or directories
4. **Mapping validation** — confirms task-to-prompt mapping entries have corresponding files
5. **.promptignore** — verifies exclusion rules are present and cover key directories

This runs automatically in CI via the `validate-prompts` job in `build.yml`.

## Provider Notes

- **Copilot**: `copilot-instructions.md` auto-loaded; use `#prompt:<name>` for domains
- **Claude**: Put shared context in system message (cached across turns); domain prompt + task in user message
- **OpenAI**: System message for shared context; `response_format` for structured output
- **Gemini**: Use Context Caching API; resist loading everything despite large context window
- **Cursor/Windsurf**: Mirror shared context in `.cursorrules`; use `@file` references
