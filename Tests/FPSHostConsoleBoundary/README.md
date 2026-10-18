# FPS commands owned by the host

`SparkFPSHostConsoleBoundaryTests` links the production `SparkEngineLib` and
`AdvancedConsoleCommands.cpp`. It calls the real graphics/subsystem registrars,
dispatches text through `SimpleConsole`, and registers FPS callbacks through
`EngineContext::GetConsole()` and its production `EngineSdkConsole` adapter.
Windows also links the existing FPS import library for the exported `Game`
accessor used by the production registrar, and requires the `SparkGameFPS` target.

The test checks material lookup with quoted arguments, lighting state changes,
shared `pp_exposure`/`exposure` state and strict parsing, FPS argument offsets,
host ownership, rejection of module replacement/removal of host commands, and
FPS cleanup without removing the original host `pp_list`. It then loads the
existing registry fixture through `ModuleManager` and checks host commands after
normal unload and thrown `OnLoad` cleanup.

The separate process keeps console callbacks scoped to its graphics instance.
Windows uses the real graphics constructor's CPU-side systems without creating
a device or window. Other hosts initialize the real NullRHI path. This is console
dispatch and lifecycle evidence, not rendered-frame or complete FPS DLL proof.

Root test integration, after `SparkRegistryLifecycleModuleFixture` is declared:

```cmake
add_subdirectory(FPSHostConsoleBoundary)
```

Build and run from a configured engine tree:

```powershell
cmake --build <build> --config Release --target SparkFPSHostConsoleBoundaryTests --parallel 1
ctest --test-dir <build> -C Release -R "^SparkFPSHostConsoleBoundary$" --output-on-failure
```
