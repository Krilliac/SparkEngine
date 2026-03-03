# SparkEngine Troubleshooting Guide

## Quick Verification

### 1. Test SparkConsole standalone

```batch
cd build\bin
SparkConsole.exe
```

Expected: "Spark Engine Console v1.0.0" banner. Try `diag`, `help`, `status`. Type `exit` to quit.

### 2. Test SparkEngine

```batch
cd build\bin
SparkEngine.exe
```

Expected: DirectX 11 window appears, SparkConsole window opens automatically, console shows initialization messages.

---

## Startup Checklist

When working correctly you should see:

1. SparkEngine window (DirectX 11, blue background)
2. SparkConsole window (opens automatically)
3. Console log output:
   ```
   [INFO] SparkConsole system initialized
   [INFO] External console connection established
   [INFO] All engine systems initialized
   ```
4. Console commands respond: `help`, `engine_status`, `fps`, `graphics_info`

---

## Common Issues

### SparkConsole shows "standalone mode"

**Cause:** SparkEngine failed to launch or crashed during startup.

- Check Visual Studio Output window for errors
- Verify both executables are in `build\bin\`
- Run from Visual Studio with debugger attached

### SparkEngine crashes immediately

**Cause:** Graphics initialization failure or missing DirectX runtime.

- Update graphics drivers
- Verify DirectX 11 support: run `dxdiag`
- Check Visual Studio Output for assertion failures
- Try running as administrator

### Neither program starts

**Cause:** Missing build output or incomplete build.

```batch
cmake --build build --config Debug
dir build\bin\*.exe
```

Both `SparkEngine.exe` and `SparkConsole.exe` must exist in `build\bin\`.

### Console connects but commands fail

- Run `engine_status` to check system initialization
- Verify the main engine loop is running (check CPU usage — should be active, not 0%)
- Check debug output for errors

### Build fails with submodule errors

```bash
# Re-initialize and update all submodules
git submodule sync
git submodule init
git submodule update --recursive
```

### Build fails with runtime library mismatch

SparkEngine uses `/MD` (dynamic CRT). If third-party libraries were built with `/MT` (static CRT), you'll get linker errors. Clean rebuild:

```batch
rmdir /s /q build
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

---

## Debug Commands

Once SparkConsole is connected, use these commands:

**System Info:**
| Command | Description |
|---|---|
| `help` | List all available commands |
| `engine_status` | Show all engine systems status |
| `console_status` | Show console connection info |
| `fps` | Current frame rate |
| `graphics_info` | Graphics engine details |
| `memory_info` | System memory usage |
| `diag` | SparkConsole diagnostics |

**Test Commands (use carefully):**
| Command | Description |
|---|---|
| `test_assert` | Trigger a test assertion |
| `crash_mode on/off` | Toggle crash dump generation |

---

## Memory Usage Reference

| State | Expected Memory |
|---|---|
| SparkEngine running | ~25-30 MB |
| SparkEngine crashed/failed startup | ~10-15 MB |
| SparkConsole standalone | ~5-8 MB |

If SparkEngine shows under 20 MB, it likely crashed during initialization.

---

## Required Files

Ensure these exist in `build\bin\` after a successful build:

- `SparkEngine.exe` — Main engine executable
- `SparkConsole.exe` — Debug console executable
- `Shaders/` — Directory with compiled/copied shader files

---

## Getting More Debug Info

1. **Visual Studio**: Run with debugger attached (F5), check Output window
2. **Windows Event Viewer**: Look under Application logs for crash entries
3. **Crash dumps**: Enable with `crash_mode on` in SparkConsole; dumps saved alongside the executable
4. **Console logs**: SparkConsole displays connection status and command output in real time

---

## Last Resort

1. **Clean rebuild:**
   ```batch
   rmdir /s /q build
   cmake -B build -G "Visual Studio 17 2022" -A x64
   cmake --build build --config Debug
   ```

2. **Re-clone submodules:**
   ```bash
   git submodule deinit --all -f
   git submodule init
   git submodule update --recursive
   ```

3. **Check antivirus**: Add the `build\` directory to exclusions

4. **Run as administrator**: Some systems require elevated privileges for named pipe communication

5. **Verify dependencies**: Ensure Visual C++ 2022 Redistributable and DirectX End-User Runtime are installed
