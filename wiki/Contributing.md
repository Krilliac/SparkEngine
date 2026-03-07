# Contributing

SparkEngine welcomes contributions of all kinds. This guide covers code style, architecture conventions, and the contribution workflow.

## License

SparkEngine is licensed under the **MIT License**. All contributions are subject to the same license.

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
   ctest --test-dir build --output-on-failure
   ```
6. Push and open a Pull Request

## Code Style

### Language Standard

- **C++20** — All code must compile with C++20 enabled
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

### Adding a New Component (see [Entity Component System](Entity-Component-System))

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

## Reporting Issues

Report bugs and request features at [GitHub Issues](https://github.com/Krilliac/SparkEngine/issues).

Include:
- Platform and compiler version
- Steps to reproduce
- Expected vs. actual behavior
- Build configuration and CMake flags used

---

## See Also

- [Architecture Overview](Architecture-Overview) — Engine design
- [Build System and CMake Modules](Build-System-and-CMake-Modules) — Build configuration
- [Testing](Testing) — Adding and running tests
- [Getting Started](Getting-Started) — Building and running the project
- [Entity Component System](Entity-Component-System) — Component architecture and ECS patterns
