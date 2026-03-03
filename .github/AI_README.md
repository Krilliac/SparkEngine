# SparkEngine AI Prompt Library

Standardized prompts for AI-assisted development with SparkEngine. Compatible with GitHub Copilot, GPT, Claude, Perplexity, and Gemini.

## Quick Start

1. **GitHub Copilot**: Automatically loads `.github/copilot-instructions.md`
   - Use `#prompt:<name>` to load specific prompt files
   - Example: `#prompt:02_editor_ui_tasks`

2. **Other AI assistants**: Use the assistant-specific prompt files in `.github/prompts/`
   - `OPENAI_PROMPTS.md` for GPT
   - `CLAUDE_PROMPTS.md` for Claude
   - `PERPLEXITY_PROMPTS.md` for research queries
   - `GEMINI_PROMPTS.md` for Gemini

## Prompt Categories

### Core Setup
- Project overview and architecture
- Refactoring rules and code standards
- Build system (CMake) guidance

### Engine Systems
- Core engine, graphics (DX11), audio (XAudio2), physics (Bullet)
- Collision detection, post-processing, IBL lighting

### Gameplay
- Game logic, weapons, projectiles
- Shader authoring (HLSL/GLSL)
- Console commands and debug tools

### Tools & Editor
- ImGui editor panels
- Asset pipeline and browser
- Deployment and debugging workflows

### Advanced
- Networking, scripting (AngelScript), AI/NavMesh
- Performance optimization, testing

## SparkEngine Context

The prompt library covers SparkEngine's full stack:

- **Rendering**: DirectX 11 pipeline with PBR, deferred/forward+, post-processing (bloom, tone mapping, FXAA), IBL, shadow mapping (PCF/VSM/CSM), particles, decals
- **Physics**: Bullet Physics with rigid bodies, constraints, raycasting, collision callbacks
- **Audio**: XAudio2 3D spatial audio with object pooling
- **ECS**: EnTT-based entity component system
- **Editor**: ImGui with docking, gizmos (ImGuizmo), node graphs (imnodes)
- **Scripting**: AngelScript with hot-reload and engine API bindings
- **Gameplay**: FPS controller, weapons, vehicles, class system, HUD, terrain, mesh LOD
- **Tooling**: Crash handler, debug console (200+ commands), profiler, scene serializer

## Best Practices

- Load the project overview prompt first for context
- Choose task-specific prompts for focused work
- Request complete, compile-ready code with error handling
- Validate thread safety and performance impact
- Include console integration for new systems
