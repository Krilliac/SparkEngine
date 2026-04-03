# SparkEngine Documentation

## Wiki

The primary documentation for SparkEngine lives in the [`wiki/`](../wiki/) directory at the repository root. It covers architecture, all engine subsystems, getting started guides, tutorials, tools, and contribution guidelines.

See the [Wiki Home](../wiki/Home.md) for a full table of contents.

## API Reference

Two documentation tools are available:

### Custom API Docs (recommended, no external deps)

```bash
docs/generate-api-docs.sh generate    # Full API reference (~250 headers → ~240 pages)
docs/generate-api-docs.sh check       # Only regenerate if headers changed (checksum-based)
```

### Wiki Sync

```bash
docs/sync-wiki.sh sync               # Update auto-generated wiki sections
```

### Legacy Doxygen (optional, requires doxygen + graphviz)

This `docs/` directory also contains tooling for Doxygen-based API documentation.

### Covered Source Directories

| Directory | Description |
|-----------|-------------|
| `SparkEngine/Source/` | Core engine library (all subsystems) |
| `SparkEditor/Source/` | ImGui visual editor (57 panels) |
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

Part of the SparkEngine project. MIT License.
