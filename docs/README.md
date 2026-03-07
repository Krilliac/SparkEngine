# SparkEngine Documentation

Auto-generated API documentation and wiki system built on Doxygen. Extracts documentation from C++ header files and produces a searchable, cross-referenced HTML site.

## Quick Start

```bash
# Generate docs (requires doxygen and graphviz)
cd docs
./generate-docs.sh

# View locally
open wiki/index.html
# or serve over HTTP:
cd wiki && python3 -m http.server 8000
```

### Auto-Update

```bash
./auto-update.sh monitor   # Watch for changes, regenerate automatically
./auto-update.sh check     # One-shot: regenerate if sources changed
./auto-update.sh force     # Force full regeneration
./auto-update.sh status    # Show last update time and file count
```

## What Gets Generated

```
docs/
|-- Doxyfile                # Doxygen configuration
|-- generate-docs.sh        # Generation script
|-- auto-update.sh          # Continuous monitoring script
|-- FEATURE_ROADMAP.md      # Planned features (3 tiers)
|-- PROJECT_STATUS.md       # Current system status report
|-- output/
|   |-- html/              # Full Doxygen HTML output
|-- wiki/
    |-- index.html          # Wiki homepage
    |-- audio.html          # Audio module docs
    |-- graphics.html       # Graphics module docs
    |-- core.html           # Core module docs
    |-- output/html/        # Copy of Doxygen output
```

## Covered Modules

The documentation system scans these engine modules:

| Module | Description |
|---|---|
| Audio | XAudio2 3D audio engine with spatial positioning |
| Graphics | DirectX 11 pipeline, shaders, post-processing, PBR |
| Core | Main engine framework, entry point |
| Input | Keyboard, mouse, gamepad handling |
| Physics | Bullet Physics integration |
| Camera | First-person camera with smooth controls |
| Game | Game objects, weapons, vehicles, HUD |
| Editor | ImGui visual editor components |
| Engine | ECS, AI, animation, save system, terrain |
| Utils | Timers, logging, crash handler, file I/O |

## Writing Documentation

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

## Configuration

### Doxygen (`Doxyfile`)

- **Input**: `SparkEngine/Source/` and `SparkEditor/Source/`
- **Output**: HTML with search, responsive design, source browser
- **Diagrams**: Class hierarchies, collaboration graphs, include dependencies (requires GraphViz)

### Auto-Update (`auto-update.sh`)

- Watches `.h` and `.hpp` files in engine and editor source directories
- Checks every 30 seconds in monitor mode
- Uses MD5 checksums for change detection
- Lock file prevents concurrent regeneration

## Dependencies

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

## CMake Integration

Add a docs target to your build:

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

## CI Integration

```yaml
- name: Generate Documentation
  run: |
    sudo apt install doxygen graphviz
    cd docs && ./generate-docs.sh

- name: Deploy to GitHub Pages
  uses: peaceiris/actions-gh-pages@v4
  with:
    github_token: ${{ secrets.GITHUB_TOKEN }}
    publish_dir: ./docs/wiki
```

## License

Part of the SparkEngine project. MIT License.
