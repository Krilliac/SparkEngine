# Security Vulnerabilities

**Last updated:** 2026-03-16
**Type:** Issue
**Status:** Active (partially resolved)
**Severity:** Critical

## Description

Static analysis identified 2 critical, 3 high, and 4 medium security vulnerabilities. 6 of 9 have been fixed as of 2026-03-16.

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

### 5. Unsafe Network Deserialization — STILL OPEN

**File:** `Engine/SaveSystem/SaveSystem.cpp:409-510`

Component properties deserialized without type or bounds validation. Malicious save files can inject oversized strings.

**Fix needed:** Validate property types against component schema, enforce max string lengths.

---

## Medium — STILL OPEN

### 6. Unsafe CreateProcessW() Arguments
**File:** `Utils/ConsoleProcessManager.cpp:322` — Command line as single unquoted string.

### 7. Fixed Buffer in popen() Loop
**File:** `SparkEditor/Panels/ProjectBrowserPanel.cpp:514` — 1024-byte buffer, long lines silently truncated.

### 8. Network Message Type Not Validated
**File:** `Engine/Networking/NetworkManager.cpp` — Unknown message types not rejected before handler dispatch.

---

## Notes

- DLL injection and command injection are the most severe — they enable remote code execution
- Path traversal issues in save system and scene loading are now fixed
- Network deserialization still has only basic 64KB size limits but no schema validation
- 3 remaining issues are medium severity (not exploitable for RCE)
