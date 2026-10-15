# Legacy actor public SDK consumer

This standalone consumer stages only the twelve canonical actor, mesh and native
type headers from an installed SDK. Its DLL links the real installed
`Spark::SparkEngineLib`; the host calls the DLL through the native `GameObject`
virtual interface and destroys its objects before unloading it. On Windows it
uses a real D3D11 WARP device to initialize the actor meshes and verify GPU buffers.

```powershell
cmake -S Tests/LegacyActorPublicSDK -B <consumer-build> -G "Ninja Multi-Config" -DSparkEngine_DIR=<installed-sdk>/lib/cmake/SparkEngine
cmake --build <consumer-build> --config Release
ctest --test-dir <consumer-build> -C Release --output-on-failure
```

Use the supported MSVC environment and the same build configuration as the
installed SDK. The parameterized `Vertex` constructor is supplied by the engine
archive, so an older archive cannot validate this consumer. No test-side actor
or mesh implementation is compiled. The in-tree test route links the real
`SparkEngineLib` build target and remains separate evidence from an installed run.

The consumer covers six actor/model declarations, real native mesh value
construction, per-header self-containment, field and transform observations,
derived mesh creation, virtual callbacks, destruction, and two load/unload cycles.
The boundary tests reject engine-private header references and include paths;
POSIX builds may also consume the producer's SDL public dependency headers.
POSIX runs exercise construction and dispatch without claiming D3D11 initialization.

Historical declaration/layout comparisons belong to the integration evidence;
this consumer verifies the installed implementation rather than a mirrored
`GameObject` implementation. It does not claim that the full FPS module is
independent of engine-private headers.
