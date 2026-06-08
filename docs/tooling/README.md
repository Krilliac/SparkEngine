# Documentation Tooling

Scripts, configuration, and automation that keep SparkEngine documentation — wikis, API reference, badges, statistics, and AI context — in sync with the source tree.

All automation lives under `docs/` (generator scripts) and `tools/` (validation scripts). Every script supports a `check` or `--warn-only` mode for CI dry-runs.

---

## Master Orchestrator

```bash
docs/update-all-docs.sh              # Run all doc scripts in order
docs/update-all-docs.sh quick        # Skip slow steps (API docs, flowchart)
docs/update-all-docs.sh check        # Dry-run: report what's stale
```

## Individual Generator Scripts

| Script | What it updates | Deps | Speed |
|--------|----------------|------|-------|
| `docs/sync-wiki.sh sync` | Wiki AUTO: sections (components, systems, panels, tests) | None | ~2s |
| `docs/generate-api-docs.sh generate` | API reference (~250 headers → ~240 pages) | None | ~15s |
| `docs/generate-symbol-index.sh generate` | Symbol/Function/Class/Enum/Macro indexes | None | ~2s |
| `docs/generate-file-tree.sh generate` | File tree (LOC + Mermaid module graph) | None | ~10s |
| `docs/generate-class-hierarchy.sh generate` | Inheritance Mermaid classDiagrams | None | ~5s |
| `docs/generate-flowchart.sh generate` | `wiki/getting-started/Engine-Architecture-Flowchart.md` | Python 3 | ~5s |
| `docs/update-codebase-stats.sh generate` | `wiki/advanced/Codebase-Statistics.md` (LOC, metrics, largest files) | None | ~5s |
| `docs/update-readme-badges.sh update` | `README.md` counts, badge JSON, AI prompt files | None | ~3s |
| `docs/update-context.sh update` | `CLAUDE.md` counts | None | ~2s |

All scripts support a `check` mode that exits 1 if stale (for CI use).

## Validation Scripts

Code quality and architectural integrity checks (all in `tools/`):

```bash
tools/validate-all.sh              # Run all checks
tools/validate-all.sh --warn-only  # Report but don't fail
```

| Script | What it checks | Deps |
|--------|---------------|------|
| `tools/check-pragma-once.sh` | All headers use `#pragma once` | None |
| `tools/check-editor-panels.sh` | All panels registered in EditorPanelFactory.cpp | None |
| `tools/check-wiki-nav.sh` | Wiki pages match `_Sidebar.md` | None |
| `tools/check-wiring.sh` | Systems with Initialize() are actually called | None |
| `tools/check-bloat.sh` | File size thresholds (500-line .cpp, 300-line .h) | None |
| `tools/check-doxygen-coverage.sh` | Headers have @file/@brief docs (95% threshold) | None |
| `tools/check-wiki-quality.sh` | Wiki template + stale metric quality checks | None |

## Legacy Doxygen (optional)

This `docs/` tree also contains configuration for Doxygen-based HTML API docs,
preserved for anyone who wants the interactive SVG call/caller graphs.

### Quick Start

```bash
cd docs
./generate-docs.sh

# Auto-regenerate on changes
./auto-update.sh monitor   # Watch for changes, regenerate automatically
./auto-update.sh check     # One-shot: regenerate if sources changed
./auto-update.sh force     # Force full regeneration
./auto-update.sh status    # Show last update time and file count
```

### Output

```
docs/
├── Doxyfile.txt           # Doxygen configuration
├── generate-docs.sh       # Generation script
├── auto-update.sh         # Continuous monitoring script
└── output/
    └── html/              # Full Doxygen HTML output
        └── index.html     # Entry point — open in browser
```

### Covered Source Directories

| Directory | Description |
|-----------|-------------|
| `SparkEngine/Source/` | Core engine library (all subsystems) |
| `SparkEditor/Source/` | ImGui visual editor (59 panels) |
| `SparkConsole/src/` | Standalone debug console application |
| `SparkShaderCompiler/src/` | Offline shader compilation tool |
| `GameModules/*/Source/` | 10 game modules (FPS, MMO, RPG, ARPG, RTS, Racing, Platformer, OpenWorld, VisualScript) |
| `SparkSDK/` | Public SDK headers |

### Writing Doxygen Comments

Use Doxygen-style comments in header files:

```cpp
/**
 * @brief Main audio engine with XAudio2 backend
 *
 * Manages all audio operations including 2D/3D playback,
 * sound effect loading, and spatial audio positioning.
 *
 * @note Initialize() must be called before any audio operations
 */
class AudioEngine
{
public:
    /**
     * @brief Initialize the audio engine
     * @param maxSources Maximum simultaneous audio sources (typical: 32-64)
     * @return HRESULT indicating success or failure
     */
    HRESULT Initialize(size_t maxSources);
};
```

Supported tags: `@file`, `@brief`, `@param`, `@return`, `@note`, `@warning`, `@see`, `@example`, `@todo`, `@bug`, `@deprecated`.

### Configuration

**Doxygen (`Doxyfile.txt`)**
- **Input**: All engine, editor, console, shader compiler, game, and SDK sources
- **Output**: HTML with search, treeview navigation, source browser, timestamps
- **Diagrams**: Class hierarchies, collaboration graphs, include dependencies, call/caller graphs (SVG, interactive)
- **STL support**: Built-in STL type recognition enabled

**Auto-Update (`auto-update.sh`)**
- Watches `.h` and `.hpp` files in engine and editor source directories
- Checks every 30 seconds in monitor mode
- Uses MD5 checksums for change detection
- Lock file prevents concurrent regeneration

### Dependencies

- **Doxygen** 1.9+ — Documentation generator
- **GraphViz** — Diagram rendering (required for class/call graphs)

```bash
# Ubuntu/Debian
sudo apt install doxygen graphviz

# macOS
brew install doxygen graphviz

# Windows (via Chocolatey)
choco install doxygen.install graphviz
```

### CMake Integration

```cmake
find_package(Doxygen)
if(DOXYGEN_FOUND)
    add_custom_target(docs
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/docs/generate-docs.sh
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/docs
        COMMENT "Generating API documentation with Doxygen"
        VERBATIM
    )
endif()
```

---

Back to [documentation index](../README.md).
