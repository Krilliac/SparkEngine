# SparkEngine Documentation

## Wiki

The primary documentation for SparkEngine lives in the [`wiki/`](../wiki/) directory at the repository root. It covers architecture, all engine subsystems, getting started guides, tutorials, tools, and contribution guidelines.

See the [Wiki Home](../wiki/Home.md) for a full table of contents.

## API Reference (Doxygen)

This `docs/` directory contains tooling for auto-generated API documentation using Doxygen. It extracts documentation from C++ header files and produces a searchable, cross-referenced HTML site.

### Quick Start

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
- **Input**: `SparkEngine/Source/` and `SparkEditor/Source/`
- **Output**: HTML with search, responsive design, source browser
- **Diagrams**: Class hierarchies, collaboration graphs, include dependencies (requires GraphViz)

**Auto-Update (`auto-update.sh`)**
- Watches `.h` and `.hpp` files in engine and editor source directories
- Checks every 30 seconds in monitor mode
- Uses MD5 checksums for change detection
- Lock file prevents concurrent regeneration

### Dependencies

- **Doxygen** 1.9+ — Documentation generator
- **GraphViz** — Diagram rendering (optional but recommended)

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
