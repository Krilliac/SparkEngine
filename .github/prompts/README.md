# SparkEngine Development Prompts

Streamlined prompt system for AI-assisted development. 9 files (down from 34), zero duplication.

## File Index

| File | Auto-loaded | Purpose |
|------|:-----------:|---------|
| [`copilot-instructions.md`](copilot-instructions.md) | Yes | Shared context: architecture, APIs, standards, build info |
| [`engine-core.prompt.md`](engine-core.prompt.md) | | ECS (EnTT), components, systems, engine lifecycle |
| [`graphics-rendering.prompt.md`](graphics-rendering.prompt.md) | | DX11 pipeline, shaders, PBR, post-processing |
| [`audio-physics.prompt.md`](audio-physics.prompt.md) | | XAudio2 audio, Jolt Physics, collision system |
| [`gameplay-systems.prompt.md`](gameplay-systems.prompt.md) | | Game module, AI, animation, weapons, procedural gen, saves |
| [`editor-tools.prompt.md`](editor-tools.prompt.md) | | SparkEditor (ImGui), asset pipeline, debugging tools |
| [`build-test.prompt.md`](build-test.prompt.md) | | CMake build, CI/CD, testing, dependencies |
| [`console-scripting.prompt.md`](console-scripting.prompt.md) | | Console system (200+ commands), AngelScript scripting |
| [`token-optimization.prompt.md`](token-optimization.prompt.md) | | AI token optimization tips for all providers |

## How It Works

1. **`copilot-instructions.md`** is auto-loaded by GitHub Copilot (also at `.github/copilot-instructions.md`). It provides shared project context — architecture map, key APIs, coding standards.

2. **Domain prompts** cover specific engine areas. Load one at a time with `#prompt:<name>`:
   ```
   #prompt:engine-core          ← for ECS work
   #prompt:graphics-rendering   ← for shader/rendering work
   #prompt:audio-physics        ← for audio or physics work
   ```

3. **No duplication** — coding standards, console registration patterns, and build info appear only in `copilot-instructions.md`. Domain prompts reference it.

## Quick Reference: Which Prompt?

| I want to... | Load this |
|--------------|-----------|
| Add/modify ECS components or systems | `engine-core` |
| Work on rendering, shaders, materials | `graphics-rendering` |
| Implement audio or physics features | `audio-physics` |
| Build game logic, AI, animation, weapons | `gameplay-systems` |
| Create editor panels or debug tools | `editor-tools` |
| Configure builds, CI, or write tests | `build-test` |
| Add console commands or scripts | `console-scripting` |
| Optimize AI assistant token usage | `token-optimization` |

## Directory Exclusion

The `.promptignore` file at the repo root lists directories and file patterns that should be excluded from AI prompt context (ThirdParty/, build/, Shaders/Compiled/, etc.). Tools and scripts should read this file to automatically filter paths, similar to how `.gitignore` works.

## Contributing

When adding prompts:
- **Don't duplicate** info that's in `copilot-instructions.md`
- **Reference real code** — use actual file paths, class names, method signatures
- **Mark unimplemented features** — don't describe planned features as working
- **Use tables and lists** — terse formatting saves tokens
- **One prompt per domain** — don't split unless a prompt exceeds ~3K tokens
