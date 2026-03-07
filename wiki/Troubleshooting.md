# Troubleshooting

Common issues and solutions when building and running SparkEngine.

## Quick Verification

### Test [[SparkConsole]] Standalone

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

## Build Issues

### Submodule Errors

```bash
git submodule sync
git submodule init
git submodule update --recursive
```

### Runtime Library Mismatch (MSVC)

SparkEngine uses `/MD` (dynamic CRT). If third-party libraries were built with `/MT`, you'll get linker errors. Clean rebuild:

```batch
rmdir /s /q build
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

### CMake Version Too Old

SparkEngine requires CMake 3.16+. Check your version:

```bash
cmake --version
```

### Missing C++20 Support

Ensure your compiler supports C++20:
- MSVC: Visual Studio 2022 (v143) or later
- GCC: Version 11 or later
- Clang: Version 14 or later

## Runtime Issues

### SparkEngine Crashes Immediately

**Cause:** Graphics initialization failure or missing DirectX runtime.

Solutions:
- Update graphics drivers
- Verify DirectX 11 support: run `dxdiag`
- Check Visual Studio Output window for assertion failures
- Try running as administrator

### SparkConsole Shows "Standalone Mode"

**Cause:** SparkEngine failed to launch or crashed during startup.

Solutions:
- Check Visual Studio Output window for errors
- Verify both executables are in `build\bin\`
- Run from Visual Studio with debugger attached

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

## Debug Commands

Once SparkConsole is connected:

```
help              # List all commands
engine_status     # Check system status
fps               # Show framerate
graphics_info     # GPU information
diag              # Full diagnostics
```

## Memory Reference

Quick reference for debugging memory issues:

| Command | Description |
|---------|-------------|
| `memory_info` | Show current memory usage |
| `profile_report` | Performance breakdown |
| `frame_time` | Frame time analysis |

## Required Files

For the engine to run correctly, these files must be present in `build/bin/`:

| File | Purpose |
|------|---------|
| `SparkEngine.exe` | Main engine executable |
| `SparkConsole.exe` | Debug console |
| `SparkGame.dll` | Default game module |

And these directories should be present:
- `Shaders/` — Compiled shader bytecode
- `Assets/` — Game assets

## Platform-Specific Issues

### Windows

- Ensure Visual C++ Redistributable is installed
- DirectX 11 capable GPU required
- Named pipes require appropriate permissions

### Linux

- SparkEditor is not available (Windows-only)
- SparkConsole uses stdout instead of named pipes
- Vulkan SDK may be needed for the Vulkan backend
- Some features may have limited support (experimental platform)

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

4. Check [GitHub Issues](https://github.com/Krilliac/SparkEngine/issues) for known problems.

---

## See Also

- [[Getting Started]] — Build instructions
- [[Build System and CMake Modules]] — Build configuration
- [[SparkConsole]] — Debug console usage
- [[Rendering and Graphics]] — Graphics troubleshooting and render pipelines
