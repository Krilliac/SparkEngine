# Contributing to SparkEngine

Thank you for your interest in contributing to SparkEngine! This document provides
guidelines and information for contributors.

## Getting Started

1. **Fork** the repository and clone your fork
2. **Create a branch** from `Working` (not `main`):
   ```bash
   git checkout -b feature/my-feature origin/Working
   ```
3. **Build** the project:
   ```bash
   cmake --preset linux-gcc-release
   cmake --build build --config Release
   ```
4. **Run tests**:
   ```bash
   cd build && ctest --output-on-failure --no-tests=error
   ```

## Code Standards

- **C++23** with C++26 forward-compatibility macros
- **Allman braces**, 4-space indent, 120-column limit
- **PascalCase** for classes/methods, **camelCase** for locals, `m_` prefix for members
- `#pragma once` in all headers
- `const` on all non-mutating methods and parameters
- Zero warnings under `/W4` (MSVC) or `-Wall -Wextra` (GCC/Clang)
- Use `EngineContext` service locator, not deprecated globals

See `.clang-format` for the full style configuration.

## Pull Request Process

1. **Rebase** onto latest `Working` before submitting
2. **Run pre-commit checks**:
   ```bash
   # Format check
   find SparkEngine/Source SparkEditor/Source -name '*.h' -o -name '*.cpp' | \
     head -50 | xargs clang-format --dry-run --Werror

   # Build + test
   cmake --build build --config Release
   cd build && ctest --output-on-failure --no-tests=error

   # Documentation
   docs/update-all-docs.sh
   ```
3. **Update CHANGELOG.md** with your changes under `[Unreleased]`
4. **Add tests** for new functionality
5. **Keep PRs focused** — one feature or fix per PR
6. **Write clear commit messages** explaining the "why"

## Architecture Guidelines

- **Wire things in.** Every system with `Initialize()` must be called in the startup path.
  Every `Update()` must appear in the main loop. Don't build systems that aren't connected.
- **Don't over-engineer.** Write only what is needed today. No speculative abstractions.
- **Check before adding.** Search for existing implementations before writing new code.
- **Keep files small.** Guidelines: ~500 lines for `.cpp`, ~300 lines for `.h`.

## Adding New Systems

1. Create the header in the appropriate directory under `SparkEngine/Source/`
2. Register with `EngineContext` using `RegisterSystem<T>()` or `RegisterSubsystem<T>()`
3. Wire into `SparkEngine.cpp` initialization and update loops
4. Add tests in `Tests/`
5. Add Doxygen comments (`@file`, `@brief`, `@param`, `@return`)
6. Update the wiki with a new page if adding a subsystem

## Reporting Issues

- Use [GitHub Issues](https://github.com/krilliac/sparkengine/issues)
- Include steps to reproduce, expected vs actual behavior
- Include build configuration (compiler, OS, CMake preset)

## Code of Conduct

This project follows the [Contributor Covenant Code of Conduct](CODE_OF_CONDUCT.md).
