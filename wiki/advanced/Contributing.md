# Contributing

SparkEngine welcomes contributions of all kinds. This guide covers code style, architecture conventions, and the contribution workflow.

## License

SparkEngine is licensed under the **Spark Open License 1.0**. All contributions are subject to the same license.

## Getting Started

1. Fork the repository on GitHub
2. Clone your fork with submodules:
   ```bash
   git clone --recurse-submodules https://github.com/YOUR_USERNAME/SparkEngine.git
   ```
3. Create a feature branch:
   ```bash
   git checkout -b feature/my-feature
   ```
4. Make your changes
5. Build and test:
   ```bash
   cmake -B build -DBUILD_TESTS=ON
   cmake --build build
   ctest --test-dir build --output-on-failure --no-tests=error
   ```
6. Push and open a Pull Request

## Pre-Commit Quality Checks

Before opening a PR, run these checks locally. CI enforces all of them automatically.

### 1. Format with clang-format

The repo ships a `.clang-format` (Microsoft-based, Allman braces, 4-space indent, 120-col limit). CI rejects PRs with formatting violations.

```bash
# Check formatting (dry run — reports violations without changing files)
find SparkEngine/Source SparkEditor/Source -name '*.h' -o -name '*.cpp' \
  | xargs clang-format --dry-run --Werror

# Auto-fix formatting
find SparkEngine/Source SparkEditor/Source -name '*.h' -o -name '*.cpp' \
  | xargs clang-format -i
```

### 2. Static analysis with clang-tidy

```bash
# Generate compile_commands.json, then run clang-tidy
cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
clang-tidy -p build SparkEngine/Source/**/*.cpp
```

### 3. Build and test

```bash
cmake --preset linux-gcc-release     # or windows-release
cmake --build build --config Release
cd build && ctest --output-on-failure --no-tests=error
```

All checks must pass before submitting a PR.

---

## Code Style

### Language Standard

- **C++23** — All code must compile with C++23 enabled
- No compiler-specific extensions (`CMAKE_CXX_EXTENSIONS OFF`)

### Naming Conventions

| Element | Convention | Example |
|---------|-----------|---------|
| Classes / Structs | PascalCase | `PhysicsSystem`, `SceneNode` |
| Functions / Methods | PascalCase | `CreateEntity()`, `GetTransform()` |
| Variables | camelCase | `deltaTime`, `maxHealth` |
| Member variables | m_ prefix | `m_registry`, `m_position` |
| Constants | UPPER_SNAKE | `SPARK_SDK_VERSION` |
| Enums | PascalCase | `RenderPath::Forward` |
| Namespaces | PascalCase | `Spark::AI`, `Spark::Graphics` |
| Files | PascalCase | `PhysicsSystem.h`, `GraphicsEngine.cpp` |

### Code Organization

- **Headers (.h)** — Declarations, inline implementations for templates
- **Source (.cpp)** — Implementations
- One class per file (with exceptions for small related types)
- Include guards: `#pragma once`

### Component Design

Components must be **pure POD structs** (state only, no behavior):

```cpp
// Good: POD component
struct HealthComponent {
    float health = 100.0f;
    float maxHealth = 100.0f;
    bool isDead = false;
};

// Bad: behavior in component
struct HealthComponent {
    float health = 100.0f;
    void TakeDamage(float amount) { health -= amount; }  // NO
};
```

## Architecture Guidelines

### Adding a New Subsystem

1. Create a directory under `SparkEngine/Source/Engine/YourSystem/`
2. Add a CMake toggle: `option(ENABLE_YOUR_SYSTEM "..." ON)`
3. Guard code with `#ifdef SPARK_ENABLE_YOUR_SYSTEM`
4. Register console commands if applicable
5. Add unit tests in `Tests/`
6. Document in the wiki

### Adding a New Component (see [Entity Component System](../subsystems/Entity-Component-System.md))

1. Define the POD struct in `Components.h`
2. Add a system function in `ECSystems.h` if needed
3. Register serialization in the save system
4. Add editor UI in SparkEditor if applicable

### Module Interface

If exposing functionality through the SDK:
1. Add interface header to `SparkSDK/Include/Spark/`
2. Use forward declarations to minimize includes
3. Maintain backward compatibility

## AI Disclosure

This project makes extensive use of AI-assisted development. All AI-generated code is reviewed, tested, and validated. Contributions may also use AI tools, but all code must be reviewed and tested before submission.

## Pull Request Guidelines

- Keep PRs focused on a single feature or fix
- Include tests for new functionality
- Update wiki documentation if adding user-facing features
- Ensure CI passes (GitHub Actions builds on Windows and Linux)
- Reference related issues in the PR description

### PR Title Format

Use a clear, imperative title that describes the change:

| Type | Example |
|------|---------|
| Feature | `Add cloth simulation system` |
| Bug fix | `Fix crash when loading empty scene` |
| Refactor | `Refactor physics collision callbacks` |
| Docs | `Update ECS wiki with component examples` |
| Test | `Add unit tests for NavMesh pathfinding` |

### PR Description Template

```markdown
## Summary
Brief description of what this PR does and why.

## Changes
- List of specific changes made

## Testing
- How this was tested (unit tests, manual testing, etc.)

## Related Issues
Closes #123
```

### Code Review Process

1. **Automated checks** — CI runs clang-format, builds (Windows + Linux), tests, and sanitizers
2. **Manual review** — A maintainer reviews the code for correctness, style, and architecture fit
3. **Feedback** — Address review comments by pushing additional commits (do not force-push during review)
4. **Merge** — Once approved and CI passes, a maintainer merges the PR

## Reporting Issues

Report bugs and request features at [GitHub Issues](https://github.com/Krilliac/SparkEngine/issues).

### Bug Reports

Include:
- **Platform and compiler version** (e.g., Windows 11, MSVC v143)
- **Steps to reproduce** — Minimal, numbered steps to trigger the bug
- **Expected behavior** — What you expected to happen
- **Actual behavior** — What actually happened (include error messages, stack traces)
- **Build configuration** — CMake flags used (`ENABLE_EDITOR`, `ENABLE_GRAPHICS`, etc.)
- **Screenshots/logs** — If applicable, include console output or screenshots

### Feature Requests

Include:
- **Use case** — Why the feature is needed
- **Proposed solution** — How you envision it working
- **Alternatives considered** — Other approaches you've thought of
- **Scope** — Is this a small addition or a major subsystem?

## Development Workflow

### Branch Naming

| Branch Type | Pattern | Example |
|-------------|---------|---------|
| Feature | `feature/description` | `feature/cloth-simulation` |
| Bug fix | `fix/description` | `fix/physics-crash` |
| Refactor | `refactor/description` | `refactor/ecs-views` |
| Docs | `docs/description` | `docs/update-wiki` |

### Testing Your Changes

Before submitting a PR, verify your changes thoroughly:

```bash
# 1. Run the full test suite
cmake -B build -DBUILD_TESTS=ON
cmake --build build
cd build && ctest --output-on-failure --no-tests=error && cd ..

# 2. Run with AddressSanitizer (catches memory bugs)
cmake -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTS=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-asan
cd build-asan && ctest --output-on-failure --no-tests=error && cd ..

# 3. Verify formatting
find SparkEngine/Source SparkEditor/Source SparkConsole/src SparkShaderCompiler/src GameModules/SparkGame/Source \
  -name '*.h' -o -name '*.cpp' | xargs clang-format --dry-run --Werror

# 4. Run clang-tidy (optional but recommended)
cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
clang-tidy -p build SparkEngine/Source/**/*.cpp
```

### Documentation Requirements

Any code change that affects user-facing features **must** include documentation updates:

1. **Wiki pages** — Update the relevant wiki page in `wiki/`. Create a new page if adding a new subsystem
2. **API docs** — Add Doxygen-style comments (`@brief`, `@param`, `@return`) to public headers
3. **Code comments** — Add inline comments for non-obvious logic
4. **CLAUDE.md** — Update if the change affects architecture, build flags, or execution order

### Wiki Authoring Standard

Use `wiki/_Template.md` when adding new wiki pages. New subsystem or workflow pages should include these sections:

- `## Overview`
- `## When to Use`
- `## Threading Model`
- `## Platform and Backend Support`
- `## Key APIs and Types`
- `## Performance Notes`
- `## Troubleshooting`
- `## Related Pages`

The quality checker (`tools/check-wiki-quality.sh`) validates template presence, stale metric guardrails, and this standards section.

### Adding a New Subsystem Checklist

When contributing a new engine subsystem:

- [ ] Create directory under `SparkEngine/Source/Engine/YourSystem/`
- [ ] Add CMake toggle: `option(ENABLE_YOUR_SYSTEM "..." ON)`
- [ ] Guard code with `#ifdef SPARK_ENABLE_YOUR_SYSTEM`
- [ ] Implement singleton pattern with `GetInstance()` if appropriate
- [ ] Register console commands if the system has debug controls
- [ ] Add unit tests in Tests/Test**YourSystem**.cpp (e.g., `Tests/TestPhysicsSystem.cpp`)
- [ ] Create wiki page in `wiki/Your-System.md`
- [ ] Add to `wiki/_Sidebar.md` navigation
- [ ] Ensure `wiki/Home.md` still points to `_Sidebar.md` as canonical navigation
- [ ] Run `docs/generate-api-docs.sh check` and `docs/sync-wiki.sh sync`

---

## See Also

- [Architecture Overview](../getting-started/Architecture-Overview.md) — Engine design and subsystem patterns
- [Build System and CMake Modules](Build-System-and-CMake-Modules.md) — Build configuration, CI/CD, and CMake flags
- [Testing](Testing.md) — Adding and running tests, test framework macros
- [Getting Started](../getting-started/Getting-Started.md) — Building and running the project
- [Entity Component System](../subsystems/Entity-Component-System.md) — Component architecture and ECS patterns
- [Profiler and Debugging](Profiler-and-Debugging.md) — Performance profiling and debug tools
- [Troubleshooting](Troubleshooting.md) — Common build and runtime issues
