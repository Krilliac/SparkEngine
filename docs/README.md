# SparkEngine Documentation

## Wiki

The primary documentation for SparkEngine lives in the [`wiki/`](../wiki/) directory at the repository root. It covers architecture, all engine subsystems, getting started guides, tutorials, tools, and contribution guidelines.

See the [Wiki Home](../wiki/Home.md) for a full table of contents.

## Documentation Automation

### Master Script (recommended)

```bash
docs/update-all-docs.sh              # Run all 6 doc scripts in order
docs/update-all-docs.sh quick        # Skip slow steps (API docs, flowchart)
docs/update-all-docs.sh check        # Dry-run: report what's stale
```

### Individual Scripts

| Script | What it updates | Deps | Speed |
|--------|----------------|------|-------|
| `sync-wiki.sh sync` | Wiki AUTO: sections (components, systems, panels, tests) | None | ~2s |
| `generate-api-docs.sh generate` | API reference (~250 headers → ~240 pages) | None | ~15s |
| `generate-flowchart.sh generate` | Engine-Architecture-Flowchart.md | Python 3 | ~5s |
| `update-codebase-stats.sh generate` | Codebase-Statistics.md (LOC, metrics, largest files) | None | ~5s |
| `update-readme-badges.sh update` | README.md counts, badge JSON, AI prompt files | None | ~3s |
| `update-context.sh update` | .claude/index.md and CLAUDE.md counts | None | ~2s |

All scripts support a `check` mode that exits 1 if stale (for CI use).

## Validation Scripts

Code quality and architectural integrity checks (all in `tools/`):

```bash
tools/validate-all.sh              # Run all 6 checks
tools/validate-all.sh --warn-only  # Report but don't fail
```

| Script | What it checks | Deps |
|--------|---------------|------|
| `check-pragma-once.sh` | All headers use `#pragma once` | None |
| `check-editor-panels.sh` | All panels registered in EditorPanelFactory.cpp | None |
| `check-wiki-nav.sh` | Wiki pages match `_Sidebar.md` | None |
| `check-wiring.sh` | Systems with Initialize() are actually called | None |
| `check-bloat.sh` | File size thresholds (500-line .cpp, 300-line .h) | None |
| `check-doxygen-coverage.sh` | Headers have @file/@brief docs (95% threshold) | None |
| `check-wiki-quality.sh` | Wiki template + stale metric quality checks | None |

### Legacy Doxygen (optional, requires doxygen + graphviz)

This `docs/` directory also contains tooling for Doxygen-based API documentation.

### Covered Source Directories

| Directory | Description |
|-----------|-------------|
| `SparkEngine/Source/` | Core engine library (all subsystems) |
| `SparkEditor/Source/` | ImGui visual editor (59 panels) |
| `SparkConsole/src/` | Standalone debug console application |
| `SparkShaderCompiler/src/` | Offline shader compilation tool |
| `GameModules/*/Source/` | 10 game modules (FPS, MMO, RPG, ARPG, RTS, Racing, Platformer, OpenWorld, VisualScript) |
| `SparkSDK/` | Public SDK headers |

### Quick Start (Legacy Doxygen)

```bash
# Generate docs (requires doxygen and graphviz)
cd docs
./generate-docs.sh

# Auto-regenerate on changes
./auto-update.sh monitor   # Watch for changes, regenerate automatically
./auto-update.sh check     # One-shot: regenerate if sources changed
./auto-update.sh force     # Force full regeneration
./auto-update.sh status    # Show last update time and file count
```

### What Gets Generated

```
docs/
|-- Doxyfile.txt           # Doxygen configuration
|-- generate-docs.sh       # Generation script
|-- auto-update.sh         # Continuous monitoring script
|-- output/
|   |-- html/              # Full Doxygen HTML output
|       |-- index.html     # Entry point — open in browser
```

### Writing Documentation

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

Supported tags: `@file`, `@brief`, `@param`, `@return`, `@note`, `@warning`, `@see`, `@example`, `@todo`, `@bug`, `@deprecated`

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

## License

Part of the SparkEngine project. Spark Open License 1.0.
