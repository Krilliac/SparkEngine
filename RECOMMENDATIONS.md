# SparkEngine — Code Quality Recommendations

> Analysis date: 2026-03-10

Prioritized recommendations from a codebase audit covering code quality, error
handling, testing, architecture, tooling, and documentation.

## Priority Summary

| Category              | High | Medium | Low |
|-----------------------|------|--------|-----|
| Code Quality          | 3    | 2      | 0   |
| Error Handling        | 2    | 2      | 0   |
| Testing               | 2    | 1      | 1   |
| Architecture          | 0    | 2      | 1   |
| Performance & Tooling | 0    | 1      | 1   |
| Documentation         | 0    | 1      | 1   |
| **Total**             | **7**| **9**  | **4**|

**Priority definitions:**
- **High** — Bugs, safety issues, or architectural rot that can cause undefined behavior or data loss
- **Medium** — Maintainability, consistency, and robustness improvements
- **Low** — Nice-to-have polish and preventive measures

---

## 1. Code Quality

| Priority | Recommendation | Files |
|----------|----------------|-------|
| **High** | **Eliminate legacy global variables.** `g_physicsSystem` (PhysicsSystem.cpp:21), `g_hInst`/`g_szTitle`/`g_szClass` (SparkEngine.cpp:102-104), and `g_headlessMode` (EngineContext.cpp:31) should be moved to `EngineContext` registrations or class members. `g_engineContext` itself is acceptable as the single entry point but should use a static accessor instead of scattered `extern` declarations. | `SparkEngine/Source/Physics/PhysicsSystem.cpp`, `SparkEngine/Source/Core/SparkEngine.cpp`, `SparkEngine/Source/Core/EngineContext.cpp` |
| **High** | **Make `EngineContext::GetSystem<T>()` type-safe.** The current implementation stores `void*` and retrieves via `static_cast` with no type validation (EngineContext.h:105-119). A mismatched register/retrieve pair silently produces undefined behavior. Replace with `std::any` + `std::any_cast`, or store a `std::type_index` alongside the pointer and assert on retrieval. | `SparkEngine/Source/Core/EngineContext.h` |
| **High** | **Remove raw `new` in PhysicsSystem.** `btCylinderShape` and `btConeShape` are allocated with naked `new` (PhysicsSystem.cpp:82-85) while `CreateSphereShape`/`CreateCapsuleShape` use managed helpers. Wrap all Bullet shapes consistently via factory functions or `std::unique_ptr` with custom deleters. | `SparkEngine/Source/Physics/PhysicsSystem.cpp` |
| **Medium** | **Eliminate `const_cast` usage.** `ShaderCacheWarming.h:227,230` casts away const on `m_metrics` inside a const method — mark `m_metrics` as `mutable` instead. `RenderTarget.cpp:942,1457` casts away const on `RenderTargetDesc&` — refactor the API so mutation is explicit. | `SparkEngine/Source/Graphics/ShaderCacheWarming.h`, `SparkEngine/Source/Graphics/RenderTarget.cpp` |
| **Medium** | **Unify logging to `SPARK_LOG_*` macros.** Three patterns coexist: `SPARK_LOG_*` macros, `SimpleConsole::GetInstance().Log()` direct calls, and `LOG_TO_CONSOLE_IMMEDIATE()`. Standardize on the macro layer defined in `LogMacros.h` and migrate direct calls. | `SparkEngine/Source/Utils/LogMacros.h` and consumers |

---

## 2. Error Handling & Safety

| Priority | Recommendation | Files |
|----------|----------------|-------|
| **High** | **Add error indication to `NetBuffer` reads.** `ReadUint8()`, `ReadUint16()`, `ReadUint32()` (NetworkManager.cpp:66-89) silently return 0 on buffer overrun. Since 0 is a valid value, overruns are undetectable. Return `std::optional<T>`, use the existing `Result.h` type, or set an internal error flag checkable via `HasError()`. | `SparkEngine/Source/Engine/Networking/NetworkManager.cpp` |
| **High** | **Add null-pointer guards to `GetGfx`/`GetInput`/`GetTimer` helpers.** These helpers (SparkConsole.cpp:46-57) return `nullptr` silently when `g_engineContext` is null. Callers then dereference without checks. Add `SPARK_ASSERT` or at minimum `SPARK_LOG_ERROR` on null returns. | `SparkEngine/Source/Utils/SparkConsole.cpp` |
| **Medium** | **Check file I/O success in `ShaderCacheWarming::SaveToFile()`.** Multiple `file.write()` calls (ShaderCacheWarming.h:280-292) never verify stream state afterward. A disk-full or permission error would produce a silently corrupted cache file. | `SparkEngine/Source/Graphics/ShaderCacheWarming.h` |
| **Medium** | **Add null check on shader error blob.** Shader.cpp error paths call `errorBlob->GetBufferPointer()` without checking for null. The D3DCompile API can return a null error blob on certain failure modes. | `SparkEngine/Source/Graphics/Shader.cpp` |

---

## 3. Testing

| Priority | Recommendation | Files |
|----------|----------------|-------|
| **High** | **Add unit tests for `EngineContext` service locator.** The service locator is the backbone of the engine but has zero test coverage. Test registration, retrieval, double-registration, retrieval of unregistered types, and shutdown ordering. Fully testable in headless mode. | `Tests/`, `SparkEngine/Source/Core/EngineContext.h` |
| **High** | **Add `NetBuffer` round-trip tests.** Write/read all types, test boundary conditions (empty buffer, exact capacity, overrun). Pure logic with no GPU dependency. This directly validates the error-handling fix above. | `Tests/`, `SparkEngine/Source/Engine/Networking/NetworkManager.cpp` |
| **Medium** | **Expand test coverage to untested core subsystems.** 39 test files exist but there are no tests for: `GraphicsEngine`, `Shader`, `RenderTarget`, `MaterialSystem`, `TextureSystem`, `PhysicsSystem` (only `PhysicsComponents` is tested), or `ModuleManager`. | `Tests/` |
| **Low** | **Add editor subsystem smoke tests.** The 22+ editor subsystems have zero test coverage. At minimum, add construction/destruction tests to catch RAII issues and null-pointer crashes. | `Tests/`, `SparkEditor/Source/` |

---

## 4. Architecture

| Priority | Recommendation | Files |
|----------|----------------|-------|
| **Medium** | **Document thread-safety contracts on all public system headers.** `CLAUDE.md` documents thread safety for 4 systems, but headers like `TextureSystem.h` (background streaming threads) and `AnimationSystem.h` (state machine updates) lack any annotations. Add `@threadsafety` Doxygen tags or `// Thread safety:` comments. | `SparkEngine/Source/Graphics/TextureSystem.h`, `SparkEngine/Source/Engine/Animation/AnimationSystem.h`, and all system headers |
| **Medium** | **Provide `EngineContext::Get()` static accessor.** The global `g_engineContext` is declared `extern` in 6+ .cpp files. A single static accessor eliminates the scattered declarations and makes the access pattern grep-able. | `SparkEngine/Source/Core/EngineContext.h`, `SparkEngine/Source/Core/EngineContext.cpp`, and 6+ consumer files |
| **Low** | **Complete one gap analysis as a reference.** The template at `docs/gap-analysis/GAP_ANALYSIS_TEMPLATE.md` is comprehensive but no completed analyses exist. Pick one system (e.g., Networking) and fill it out as an example for the team. | `docs/gap-analysis/` |

---

## 5. Performance & Tooling

| Priority | Recommendation | Files |
|----------|----------------|-------|
| **Medium** | **Add code coverage reporting to CI.** The project has 39+ test files and CTest integration but no coverage reporting. Add `gcov`/`lcov` or `llvm-cov` to the Linux CI build to identify untested code paths. | `CMakeLists.txt`, `.github/workflows/` |
| **Low** | **Enable `clang-tidy` in CI.** A `.clang-tidy` config exists with bugprone, modernize, performance, and readability checks, but CI only enforces `clang-format`. Running `clang-tidy` on changed files in PRs would catch patterns like raw `new` and `const_cast` automatically. | `.clang-tidy`, `.github/workflows/` |

---

## 6. Documentation

| Priority | Recommendation | Files |
|----------|----------------|-------|
| **Medium** | **Add `@threadsafety` Doxygen tag.** Register a custom `@threadsafety` alias in the Doxyfile and annotate all system headers. This makes thread-safety guarantees searchable in generated API docs. | `docs/Doxyfile.txt`, system headers |
| **Low** | **Maintain low TODO/FIXME count.** Only 4 TODO/FIXME/HACK comments exist across `SparkEngine/Source/` — this is excellent discipline. Add a CI check or pre-commit hook that warns when the count exceeds a threshold (e.g., 20). | CI or pre-commit config |

---

## Next Steps

Start with the three **High-priority Code Quality** items, as they address potential
undefined behavior:

1. Make `EngineContext::GetSystem<T>()` type-safe (`EngineContext.h`)
2. Remove raw `new` in `PhysicsSystem` (`PhysicsSystem.cpp`)
3. Eliminate legacy global variables (`PhysicsSystem.cpp`, `SparkEngine.cpp`, `EngineContext.cpp`)
