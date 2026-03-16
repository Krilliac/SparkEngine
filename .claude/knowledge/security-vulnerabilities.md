# Security Vulnerabilities

**Last updated:** 2026-03-16
**Type:** Issue
**Status:** Active
**Severity:** Critical

## Description

Static analysis identified 2 critical, 3 high, and 4 medium security vulnerabilities. These must be fixed before any production deployment.

---

## Critical — Fix Immediately

### 1. DLL/SO Injection — Unsafe Dynamic Library Loading

**Files:**
- `Core/GameModuleLoader.cpp:42` — `LoadLibraryA(path.c_str())`
- `Core/ModuleManager.cpp:118` — `LoadLibraryA(path.c_str())`
- `SparkEditor/Core/EditorPluginManager.cpp:79` — `LoadLibraryA(path.c_str())`

No path validation, signature verification, or whitelisting. Attacker can place malicious DLL in engine directory.

**Fix:** Validate paths are within whitelisted directories, use `SetDllDirectory()`, implement signature verification.

### 2. Command Injection via popen()/system()

**Files:**
- `SparkEditor/VersionControl/VersionControlSystem.cpp:1357` — `popen(fullCommand.c_str(), "r")`
- `SparkEditor/Panels/ProjectBrowserPanel.cpp:506,510` — `popen("zenity/kdialog ...")`
- `SparkEditor/Core/EditorUI.cpp:1398` — `system("xdg-open docs/ &")`

User input (repo names, branch names, file paths) concatenated into shell commands without escaping. Shell metacharacters (`;`, `|`, `&`) can execute arbitrary commands.

**Fix:** Use `fork()/execv()` or `CreateProcessW()` with argument arrays. Never pass user input through shell.

---

## High — Fix Before Release

### 3. Path Traversal in Save System

**File:** `Engine/SaveSystem/SaveSystem.cpp:1136-1138`
```cpp
return m_saveDirectory + "/" + slotName + ".spark_save";
```
No validation of `slotName`. Input like `"../../../etc/passwd"` writes outside save directory.

**Fix:** Whitelist regex `^[a-zA-Z0-9_-]{1,64}$`, use `std::filesystem::canonical()` to verify path stays within save directory.

### 4. Path Traversal in Scene Loading

**File:** `SceneManager/SceneManager.cpp:371,592`

Scene file paths not validated. Can read arbitrary files via `"../../../sensitive_config.json"`.

**Fix:** Restrict to `Assets/Scenes/` directory, validate via centralized `ValidatePath()`.

### 5. Unsafe Network Deserialization

**File:** `Engine/SaveSystem/SaveSystem.cpp:409-510`

Component properties deserialized without type or bounds validation. Malicious save files can inject oversized strings.

**Fix:** Validate property types against component schema, enforce max string lengths.

---

## Medium

### 6. Unsafe CreateProcessW() Arguments
**File:** `Utils/ConsoleProcessManager.cpp:322` — Command line as single unquoted string.

### 7. Fixed Buffer in popen() Loop
**File:** `SparkEditor/Panels/ProjectBrowserPanel.cpp:514` — 1024-byte buffer, long lines silently truncated.

### 8. Network Message Type Not Validated
**File:** `Engine/Networking/NetworkManager.cpp` — Unknown message types not rejected before handler dispatch.

### 9. File Picker Result Not Sanitized
**File:** `SparkEditor/Panels/ProjectBrowserPanel.cpp:506-528` — External tool results used as paths without validation.

---

## Notes

- DLL injection and command injection are the most severe — they enable remote code execution
- Path traversal issues affect save system, scene loading, and asset loading
- Network deserialization has basic 64KB size limits but no schema validation
