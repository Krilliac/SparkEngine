# Troubleshooting

Common issues and solutions when building and running SparkEngine, organized by subsystem.

## Quick Verification

### Test [SparkConsole](../gameplay-tools/SparkConsole.md) Standalone

```batch
cd build\bin
SparkConsole.exe
```

Expected: "Spark Engine Console v1.0.0" banner. Try `diag`, `help`, `status`. Type `exit` to quit.

### Test SparkEngine

```batch
cd build\bin
SparkEngine.exe
```

Expected:
1. DirectX 11 window appears (blue background)
2. SparkConsole window opens automatically
3. Console shows initialization messages:
   ```
   [INFO] SparkConsole system initialized
   [INFO] External console connection established
   [INFO] All engine systems initialized
   ```
4. Console commands respond: `help`, `engine_status`, `fps`, `graphics_info`

### Full Diagnostic Sequence

Run these commands in the console to verify all subsystems:

```
diag                # Full system diagnostic
engine_status       # Overall engine health
fps                 # Frame rate check
graphics_info       # GPU and DirectX info
audio_info          # Audio system status
physics_info        # Physics system status
memory_info         # Memory usage
profile_report      # Performance breakdown
```

---

## Build Issues

### Submodule Errors

**Symptom:** CMake errors about missing directories or empty third-party folders.

```bash
git submodule sync
git submodule init
git submodule update --recursive
```

If submodules are corrupted, remove and re-clone:

```bash
rm -rf ThirdParty/JoltPhysics ThirdParty/entt ThirdParty/imgui
git submodule update --init --recursive --force
```

### Runtime Library Mismatch (MSVC)

**Symptom:** Linker errors like `LNK2038: mismatch detected for 'RuntimeLibrary'`.

SparkEngine uses `/MD` (dynamic CRT). If third-party libraries were built with `/MT`, you get linker errors. Clean rebuild:

```batch
rmdir /s /q build
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

### CMake Version Too Old

**Symptom:** `CMake Error: CMAKE_CXX_STANDARD is set to 20, but...`

SparkEngine requires CMake 3.16+. Check your version:

```bash
cmake --version
```

### Missing C++23 Support

**Symptom:** Compilation errors on `std::expected`, `std::print`, deducing `this`, or `if consteval`.

Ensure your compiler supports C++23:

| Compiler | Minimum Version |
|----------|----------------|
| MSVC | Visual Studio 2022 17.6+ (v143, _MSC_VER >= 1936) |
| GCC | Version 13 |
| Clang | Version 17 |

### Incomplete SparkEngine Installation (Standalone Projects)

**Symptom:** CMake error like `include could not find requested file: SparkEngineTargets.cmake` when configuring a standalone game project.

**Cause:** `SparkEngineConfig.cmake` exists but companion files (`SparkEngineTargets.cmake`, `SparkGameModule.cmake`) are missing -- typically from an interrupted install or pointing at a build tree instead of an install prefix.

**Solution:** Re-run the install step and reconfigure:

```bash
cmake --install <build-dir> --prefix <install-prefix>
cmake -B build -DCMAKE_PREFIX_PATH=<install-prefix>
```

**Tip:** Include `SparkEnginePreflight.cmake` in your project (before `find_package`) for automatic detection. See [Build System and CMake Modules](Build-System-and-CMake-Modules.md#sparkenginepreflightcmake).

### Clang-Format Failures in CI

**Symptom:** CI `check-format` job fails with formatting errors.

**Solution:** Run clang-format locally before committing:

```bash
# Check for issues
find SparkEngine/Source SparkEditor/Source SparkConsole/src SparkShaderCompiler/src GameModules/SparkGame/Source \
  -name '*.h' -o -name '*.cpp' | head -50 | xargs clang-format --dry-run --Werror 2>&1

# Auto-fix
find SparkEngine/Source SparkEditor/Source SparkConsole/src SparkShaderCompiler/src GameModules/SparkGame/Source \
  -name '*.h' -o -name '*.cpp' | xargs clang-format -i
```

### Linux Build Fails with Missing Headers

**Symptom:** `fatal error: d3d11.h: No such file or directory` or similar DirectX headers.

**Cause:** Linux builds use stub headers from `Core/Platform.h`. Ensure you are not including Windows-only headers outside `#ifdef SPARK_PLATFORM_WINDOWS` guards.

**Solution:** Check that all DirectX includes are wrapped:

```cpp
#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <xaudio2.h>
#endif
```

### MSVC /W4 Warning-as-Error Failures

**Symptom:** Build fails with warnings treated as errors on Windows but not Linux.

Common MSVC-specific warnings and fixes:

| Warning | Meaning | Fix |
|---------|---------|-----|
| `C4244` | Narrowing conversion | Add explicit cast: `static_cast<float>(value)` |
| `C4267` | `size_t` to `int` conversion | Use `static_cast<int>(container.size())` |
| `C4100` | Unreferenced parameter | Use `[[maybe_unused]]` or `(void)param;` |
| `C4189` | Unused local variable | Remove or use the variable |
| `C4458` | Member hides class member | Rename local or use `this->` |

---

## Runtime Issues

### SparkEngine Crashes Immediately

**Cause:** Graphics initialization failure or missing DirectX runtime.

**Diagnostic steps:**
1. Run `dxdiag` to verify DirectX 11 support
2. Update graphics drivers to latest version
3. Check Visual Studio Output window for assertion failures
4. Try running as administrator
5. Check for missing DLLs with Dependency Walker or `dumpbin /dependents SparkEngine.exe`

**Common assertions:**

| Assertion | Cause | Fix |
|-----------|-------|-----|
| `D3D11CreateDevice failed` | No DirectX 11 GPU | Update drivers or use software adapter |
| `CreateSwapChain failed` | Invalid window handle | Check window creation code |
| `CompileShader failed` | Missing shader files | Verify `Shaders/` directory exists |

### SparkConsole Shows "Standalone Mode"

**Cause:** SparkEngine failed to launch or crashed during startup, so the named pipe connection was never established.

**Solutions:**
1. Check Visual Studio Output window for errors
2. Verify both executables are in `build\bin\`
3. Run from Visual Studio with debugger attached
4. Check Windows Event Viewer for crash details

### Neither Program Starts

**Cause:** Missing build output or incomplete build.

```batch
cmake --build build --config Debug
dir build\bin\*.exe
```

Both `SparkEngine.exe` and `SparkConsole.exe` must exist in `build\bin\`.

### Console Connects but Commands Fail

- Run `engine_status` to check system initialization
- Verify the main engine loop is running (CPU usage should be active, not 0%)
- Check debug output for errors
- Try `diag` for a full diagnostic dump

---

## Graphics Issues

### Black Screen After Initialization

**Cause:** Shaders failed to compile or render target not cleared.

**Solutions:**
1. Check console for shader compilation errors: `graphics_info`
2. Verify `Shaders/` directory contains `.hlsl` files
3. Check that the swap chain present is being called: look for `[ERROR] Present failed` in logs
4. Try a debug build for more verbose DirectX error messages

### Low Frame Rate

**Diagnostic:** Run `fps` and `profile_report` in console.

| Profile Category | Normal Range | If Too High |
|-----------------|-------------|-------------|
| Render | 2-8 ms | Reduce draw calls, simplify shaders |
| Physics | 0.5-3 ms | Reduce collision bodies, simplify meshes |
| Animation | 0.1-1 ms | Reduce animated entities, lower bone counts |
| AI | 0.1-2 ms | Reduce AI agents, simplify behavior trees |
| Audio | 0.05-0.5 ms | Reduce active audio sources |

### Shader Compilation Errors

**Symptom:** `[ERROR] Failed to compile shader: <name>` in console.

**Solutions:**
1. Verify shader model compatibility with your GPU
2. Check for syntax errors in `.hlsl` files
3. On MSVC, ensure the Windows SDK is installed (provides `d3dcompiler.h`)
4. Use `graphics_info` to check supported shader model version

---

## Physics Issues

### Physics Not Updating

**Cause:** PhysicsSystem must run on the main thread only.

**Checklist:**
1. Verify the Jolt dependency configured successfully and physics initialized
2. Check that `PhysicsSystem::Update()` is called each frame
3. Ensure physics bodies are properly registered with the system
4. Check console: `physics_info`

### Objects Falling Through Floor

**Cause:** Collision mesh mismatch or time step too large.

**Solutions:**
1. Verify floor has a static rigid body with collision shape
2. Check that the collision margin is not too large
3. Use fixed time step for physics: typically 1/60s
4. Enable continuous collision detection for fast-moving objects

### Physics Performance Degradation

**Symptom:** Frame time spikes in `profile_report` under Physics.

**Solutions:**
1. Reduce number of active rigid bodies (deactivate distant objects)
2. Use simpler collision shapes (box/sphere instead of mesh)
3. Increase the physics sleep threshold so resting objects deactivate sooner

---

## Audio Issues

### No Sound Output

**Diagnostic:** Run `audio_info` in console.

**Checklist:**
1. Verify `AudioEngine::Initialize()` was called successfully
2. Check master volume is not 0: `audio_master 1.0`
3. Verify sound files are loaded: `audio_list`
4. Check system audio output device
5. On Linux, verify OpenAL Soft is installed: `apt install libopenal-dev`

### 3D Audio Not Working

**Cause:** Listener position or orientation not being updated.

**Solution:** Ensure these are called each frame:

```cpp
audio.SetListenerPosition(cameraPosition);
audio.SetListenerOrientation(cameraForward, cameraUp);
```

### Audio Crackling or Stuttering

**Cause:** Too many simultaneous audio sources or audio thread contention.

**Solutions:**
1. Check active source count: `audio_sources`
2. Reduce `maxSources` if memory is constrained
3. Ensure `AudioEngine::Update()` is called every frame
4. Lower the audio quality or sample rate for non-critical sounds

### Music Crossfade Glitches

**Cause:** Both tracks briefly playing at full volume.

**Solution:** Ensure `MusicManager::Update(deltaTime)` is called every frame so crossfade progress is updated smoothly.

---

## AI and Navigation Issues

### AI Agents Not Moving

**Checklist:**
1. Verify the AI/navigation sources are present in the configured engine target
2. Check that NavMesh is generated for the scene
3. Verify agents have valid start/target positions on the NavMesh
4. Run `ai_status` in console

### NavMesh Not Generating

**Cause:** Scene geometry not suitable for navmesh generation.

**Solutions:**
1. Ensure floor meshes are marked as walkable
2. Check that agent radius and height are appropriate for the geometry
3. Verify the navmesh baking area covers the playable region

### AI Behavior Tree Not Executing

**Cause:** Tree root node not returning `Running` status.

**Solutions:**
1. Check that the behavior tree is assigned to the entity
2. Verify node connections in the tree
3. Enable AI debug visualization: `ai_debug_draw true`

---

## Animation Issues

### Skeletal Animation Not Playing

**Checklist:**
1. Verify the animation sources are present in the configured engine target
2. Check that the animation clip is loaded and assigned
3. Verify the skeleton bone hierarchy matches the mesh
4. Check animation state machine transitions

### Animation Jittering

**Cause:** Incorrect interpolation or missing keyframes.

**Solutions:**
1. Verify animation frame rate matches expected rate
2. Check for duplicate bone names in the skeleton
3. Ensure animation blending weights sum to 1.0

---

## ECS Issues

### Component Not Found on Entity

**Symptom:** `world.GetComponent<T>(entity)` returns null or crashes.

**Solutions:**
1. Verify the component was added with `world.AddComponent<T>(entity)`
2. Check that the entity is still alive (not destroyed)
3. Ensure you are using the correct entity ID (not a stale reference)

### System Execution Order Problems

**Symptom:** Systems read stale data from other systems.

The ECS execution order is: **Physics -> Animation -> AI -> Audio -> Lifecycle -> Render**

If your custom system needs data from another system, ensure it runs after that system in the pipeline.

---

## Networking Issues

### NetworkManager Not Available

**Cause:** Networking is disabled by default.

**Solution:** Enable in CMake:

```bash
cmake -B build -DENABLE_NETWORKING=ON ...
```

### Connection Refused

**Checklist:**
1. Verify server is running and listening on the expected port
2. Check firewall settings
3. Verify IP address and port configuration
4. Check console: `net_status`

---

## Editor Issues

### SparkEditor Panels Not Appearing

**Cause:** Panel initialization order or missing subsystem.

**Solutions:**
1. Verify `ENABLE_EDITOR` is ON in CMake
2. Check that all 22 editor subsystems initialized: `editor_status`
3. Reset panel layout: `editor_reset_layout`

### Editor Crash on Scene Load

**Cause:** Scene file corruption or missing asset references.

**Solutions:**
1. Check the scene JSON for syntax errors
2. Verify all referenced assets exist on disk
3. Try loading a known-good scene first
4. Check console output for the specific error

---

## Debug Commands Reference

Once SparkConsole is connected, these commands are available for diagnosing issues:

### General

| Command | Description |
|---------|-------------|
| `help` | List all available commands |
| `engine_status` | Check overall system status |
| `diag` | Full diagnostic dump |
| `fps` | Show current frame rate |
| `frame_time` | Detailed frame time analysis |
| `profile_report` | Performance breakdown by system |

### Graphics

| Command | Description |
|---------|-------------|
| `graphics_info` | GPU info, DirectX version, resolution |
| `render_stats` | Draw calls, triangles, textures |
| `wireframe_toggle` | Toggle wireframe rendering |

### Audio

| Command | Description |
|---------|-------------|
| `audio_info` | Audio system status and metrics |
| `audio_master <vol>` | Set master volume (0.0-1.0) |
| `audio_sfx <vol>` | Set SFX volume |
| `audio_music <vol>` | Set music volume |
| `audio_play <name>` | Play a loaded sound |
| `audio_stop_all` | Stop all playing sounds |
| `audio_list` | List all loaded sounds |
| `audio_sources` | Show active audio source count |

### Memory

| Command | Description |
|---------|-------------|
| `memory_info` | Current memory usage by category |
| `memory_dump` | Detailed allocation dump |

### Physics

| Command | Description |
|---------|-------------|
| `physics_info` | Physics system status |
| `physics_debug` | Toggle physics debug visualization |

### AI

| Command | Description |
|---------|-------------|
| `ai_status` | AI system status and agent count |
| `ai_debug_draw <bool>` | Toggle AI debug visualization |

### Dialogue

| Command | Description |
|---------|-------------|
| `dialogue_status` | Dialogue system state |
| `dialogue_trees` | List loaded dialogue trees |

## Required Files

For the engine to run correctly, these files must be present in `build/bin/`:

| File | Purpose |
|------|---------|
| `SparkEngine.exe` | Main engine executable |
| `SparkConsole.exe` | Debug console |
| `SparkGame.dll` | Default game module |

And these directories should be present:
- `Shaders/` -- Compiled shader bytecode
- `Assets/` -- Game assets (textures, models, audio)
- `Data/` -- Data files (dialogue JSON, scene files)

## Platform-Specific Issues

### Windows

| Issue | Solution |
|-------|----------|
| Missing Visual C++ Redistributable | Install from Microsoft |
| DirectX 11 not available | Update GPU drivers or use WARP adapter |
| Named pipes permission denied | Run as administrator or adjust pipe ACLs |
| Antivirus blocking executables | Add build directory to exclusion list |
| Windows Defender slow builds | Exclude `build/` from real-time scanning |

### Linux

| Issue | Solution |
|-------|----------|
| SparkEditor not available | Editor is Windows-only; use console for Linux |
| SparkConsole uses stdout | Expected behavior; named pipes are Windows-only |
| OpenAL not found | `sudo apt install libopenal-dev` |
| Missing X11 headers | `sudo apt install libx11-dev libxrandr-dev` |
| Vulkan SDK missing | Install from LunarG Vulkan SDK |
| GCC link errors with `-lstdc++fs` | Use GCC 11+ (filesystem is in libstdc++ proper) |

### macOS

| Issue | Solution |
|-------|----------|
| Metal backend not implemented | macOS support is experimental |
| OpenAL deprecation warnings | Expected; SparkEngine uses OpenAL Soft, not Apple OpenAL |
| Code signing issues | Use `codesign --force --deep -s -` for local development |

## Last Resort

1. Clean build:
   ```bash
   rm -rf build
   cmake -B build -G "..." -DCMAKE_BUILD_TYPE=Debug
   cmake --build build
   ```

2. Re-clone with submodules:
   ```bash
   git clone --recurse-submodules https://github.com/Krilliac/SparkEngine.git
   ```

3. Try the minimal preset:
   ```bash
   cmake --preset minimal
   cmake --build --preset minimal
   ```

4. Enable verbose logging:
   ```bash
   # Set environment variable before running
   SPARK_LOG_LEVEL=TRACE ./build/bin/SparkEngine
   ```

5. Check [GitHub Issues](https://github.com/Krilliac/SparkEngine/issues) for known problems.

6. File a new issue with:
   - OS and compiler version
   - CMake configuration output
   - Full build log (if build failure)
   - Console `diag` output (if runtime failure)
   - Steps to reproduce

---

## See Also

- [Getting Started](../getting-started/Getting-Started.md) -- Build instructions
- [Build System and CMake Modules](Build-System-and-CMake-Modules.md) -- Build configuration
- [SparkConsole](../gameplay-tools/SparkConsole.md) -- Debug console usage
- [Rendering and Graphics](../subsystems/Rendering-and-Graphics.md) -- Graphics troubleshooting and render pipelines
- [Audio](../subsystems/Audio.md) -- Audio system details
- [AI and Navigation](../subsystems/AI-and-Navigation.md) -- AI system details
- [Entity Component System](../subsystems/Entity-Component-System.md) -- ECS architecture
- [Physics](../subsystems/Physics.md) -- Physics system details
