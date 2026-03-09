# AI Token Optimization Guide

How to use SparkEngine's prompt system efficiently across AI providers. The goal: load the minimum context needed for each task while maintaining accurate project understanding.

## Hierarchical Loading Model

```
Layer 1: copilot-instructions.md    ← Always loaded (shared context, ~4K tokens)
Layer 2: One domain prompt           ← Load per task (~1.5-2K tokens each)
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

---

## Provider-Specific Tips

### GitHub Copilot

`copilot-instructions.md` is auto-loaded from `.github/` — no action needed.

```
# Load domain context:
#prompt:engine-core
#prompt:graphics-rendering

# Reference specific files:
#file:SparkEngine/Source/Graphics/GraphicsEngine.h
#folder:SparkEngine/Source/Engine/ECS/Components

# Quick actions:
/fix       — Fix selected code
/explain   — Explain selected code
/tests     — Generate tests
/doc       — Generate documentation
```

### Claude (Code / API)

**Prompt caching optimization**: Claude caches prompt prefixes. Structure requests so static context comes first, variable content last:

```
System message: [copilot-instructions.md content]     ← cached across turns
User message:   [domain prompt] + [specific task]     ← variable per turn
```

- Put `copilot-instructions.md` in the system message — it gets cached and reused
- Add the relevant domain prompt at the start of the user message
- Keep per-turn additions (your specific question/task) at the end
- For multi-turn sessions: the system message stays cached, saving ~4K tokens per turn
- Use `#file:` references to load specific source files instead of pasting code
- Break large tasks into focused sub-tasks to avoid context window waste

### OpenAI (GPT-4o / o1 / o3)

- Use system message for `copilot-instructions.md` content (benefits from system message caching)
- Use structured output (`response_format`) to reduce response token waste for data extraction tasks
- For deterministic results on repeated queries, set `seed` parameter
- Load domain prompts as the first user message, then ask your question

### Google Gemini

- Use Gemini's **Context Caching API** to cache `copilot-instructions.md` — avoids re-processing on every request
- Gemini's large context window (1M+ tokens) tempts loading everything — resist this; focused prompts produce better results
- Use **grounding with Google Search** for researching DX11/Bullet/XAudio2 best practices instead of including background info in prompts

### Cursor / Windsurf / Other IDE Agents

- Place project context in `.cursorrules` or equivalent (mirrors `copilot-instructions.md`)
- Use `@file` references for targeted file inclusion
- Avoid workspace-wide indexing of `ThirdParty/` — it's all vendored dependencies
- Exclude `Shaders/Compiled/` from context (binary `.cso` files)

---

## General Token-Saving Principles

1. **Never duplicate information** — All shared info lives in `copilot-instructions.md`. Domain prompts reference it, never repeat it.

2. **Use terse formatting** — Tables and bullet lists over prose paragraphs. The prompts use this format intentionally.

3. **Prefix ordering matters** — `[role/constraints] → [project context] → [domain context] → [task]`. This ordering maximizes prefix caching across all providers that support it.

4. **Reference over inline** — Point to files (`#file:SparkEngine/Source/Physics/PhysicsSystem.h`) instead of pasting their contents. Let the AI tool fetch what it needs.

5. **Scope your context** — Working on shaders? Load `graphics-rendering` only. Don't also load `audio-physics` "just in case."

6. **Prune stale context** — In multi-turn conversations, don't keep accumulating file contents. Summarize findings and drop raw code from context when possible.

7. **Exclude from indexing**:
   - `ThirdParty/` — Vendored dependencies (large, not your code)
   - `Shaders/Compiled/` — Binary shader bytecode
   - `build/` — Build artifacts
   - `.vs/` / `.vscode/` — IDE settings
