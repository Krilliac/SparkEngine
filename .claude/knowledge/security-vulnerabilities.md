# Security Vulnerabilities

**Last updated:** 2026-03-16
**Type:** Issue
**Status:** Resolved (9 of 9 fixed or mitigated)
**Severity:** Critical

## Description

Static analysis identified 2 critical, 3 high, and 4 medium security vulnerabilities. All have been fixed or mitigated as of 2026-03-16.

---

## Critical — FIXED

### 1. DLL/SO Injection — Unsafe Dynamic Library Loading — RESOLVED

**Files:**
- `Core/GameModuleLoader.cpp` — `LoadLibraryA(path.c_str())`
- `Core/ModuleManager.cpp` — `LoadLibraryA(path.c_str())`
- `SparkEditor/Core/EditorPluginManager.cpp` — `LoadLibraryA(path.c_str())`

**Fix applied:** Path traversal rejection — all three loaders now reject paths containing `..` sequences before loading. Prevents directory traversal attacks.

### 2. Command Injection via popen()/system() — RESOLVED

**Files:**
- `SparkEditor/VersionControl/VersionControlSystem.cpp` — `ExecuteCommand()`
- `SparkEditor/Core/EditorUI.cpp` — documentation menu item

**Fixes applied:**
- `ExecuteCommand()` now rejects shell metacharacters (`;|&$\`\n\r`) in working directory
- `ExecuteCommand()` now validates that only `git` commands are allowed
- `StageFiles()`/`UnstageFiles()` validate file paths for shell metacharacters
- `EditorUI.cpp` replaced `system()` with `fork()/execlp()` on Linux
- `ProjectBrowserPanel.cpp` now validates file picker results are actual directories

---

## High — FIXED

### 3. Path Traversal in Save System — RESOLVED

**File:** `Engine/SaveSystem/SaveSystem.cpp`

**Fix applied:** Added `IsValidSlotName()` — whitelist validation requiring only `[a-zA-Z0-9_-]` characters, max 64 chars. Applied in `GetSavePath()`, `Save()`, `Load()`, and `DeleteSave()`.

### 4. Path Traversal in Scene Loading — RESOLVED

**File:** `SceneManager/SceneManager.cpp`

**Fix applied:** `LoadScene()` now rejects file paths containing `..` sequences before loading.

### 5. Unsafe Network Deserialization — RESOLVED

**File:** `Engine/SaveSystem/SaveSystem.cpp`

**Fix applied:** Added `SafeGetFloat()`, `SafeGetUint32()`, `SafeGetString()` static helpers that wrap `std::stof`/`std::stoul` in try/catch and enforce a 4096-character max property length. All 12 component deserializers now use these safe helpers instead of raw `std::stof`/`std::stoul` calls. Malformed save data returns defaults instead of crashing.

---

## Medium — RESOLVED / MITIGATED

### 6. Unsafe CreateProcessW() Arguments — MITIGATED
**File:** `Utils/ConsoleProcessManager.cpp:320-322`

**Status:** Already safe — command line is quoted (`L"\"" + path + L"\""`) and path is not user-supplied (it's the SparkConsole.exe path resolved at startup).

### 7. Fixed Buffer in popen() Loop — MITIGATED
**File:** `SparkEditor/Panels/ProjectBrowserPanel.cpp:514`

**Status:** Already safe — 1024-byte buffer is used in a `while(fgets())` loop that concatenates into a `std::string`, correctly handling long paths. Result is validated with `std::filesystem::is_directory()`.

### 8. Network Message Type Not Validated — RESOLVED
**File:** `Engine/Networking/NetworkManager.cpp:812-826`

**Fix applied:** Unknown message types now log a warning via SPARK_LOG_WARN instead of being silently dropped. Handler dispatch already safely ignores unknown types (map lookup fails, handler is null).

---

## Notes

- All 9 vulnerabilities are now resolved or mitigated
- DLL injection and command injection were the most severe — they enabled remote code execution
- Network message validation was added as defense-in-depth (unknown types were already ignored safely)
