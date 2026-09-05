# Shader Pipeline

SparkEngine authors shaders in HLSL. On Windows the `SparkShaderCompiler` tool and the runtime compile HLSL to DXBC for real through `d3dcompiler_47` (`D3DCompile`); DXIL, SPIR-V, GLSL, and MSL targets are explicit not-integrated failures (no DXC or SPIRV-Cross is linked), not success-with-empty-bytecode. The runtime `Shader` class (loading, variants, constant buffers, hot reload) is not instantiated by the shipped engine: the `GraphicsEngine`-side member was deleted, and `ShaderHotReload` is never enabled in production, so runtime shader hot reload is **not active in `stable-v1`**. The `ShaderCacheWarming` system precompiles shader permutations on background threads.

**Source:** `SparkEngine/Source/Graphics/Shader.h`, `SparkEngine/Source/Graphics/ShaderVariantSystem.h`, `SparkShaderCompiler/src/main.cpp`

---

## Architecture Overview

```
                    +---------------------------+
                    |   HLSL / GLSL Source       |
                    |   (Shaders/HLSL/, .glsl)   |
                    +-------------+-------------+
                                  |
              +-------------------+-------------------+
              |                   |                   |
     Offline Compilation   Runtime Loading      RHI Cross-Compile
              |                   |                   |
    +---------v--------+ +-------v--------+ +---------v--------+
    | SparkShader      | | Shader class   | | RHIFactory::     |
    | Compiler (CLI)   | | (runtime)      | | CompileShader()  |
    +--------+---------+ +-------+--------+ +---------+--------+
             |                   |                    |
             v                   v                    v
    +--------+---------+ +-------+--------+ +---------+--------+
    | .cso (D3D11)     | | ID3D11*Shader  | | .spv (Vulkan)    |
    | .spv (Vulkan)    | | ComPtr<>       | | .glsl (OpenGL)   |
    | .glsl (OpenGL)   | | Constant Bufs  | |                  |
    +------------------+ +-------+--------+ +------------------+
                                 |
                    +------------+-------------+
                    |                          |
           +--------v--------+      +---------v---------+
           | ShaderCache     |      | ShaderVariants    |
           | Warming         |      | (defines-based    |
           | (background     |      |  permutations)    |
           |  precompile)    |      |                   |
           +-----------------+      +-------------------+
```

---

## Shader Languages

| Language | Backend              | File Extension | Compilation Target |
|----------|----------------------|----------------|--------------------|
| HLSL     | DirectX 11 / D3D12   | `.hlsl`        | `.cso` bytecode    |
| GLSL     | OpenGL               | `.glsl`        | `.glsl.spv`        |
| SPIR-V   | Vulkan (cross-compiled from HLSL) | `.spv` | Binary SPIR-V |

## Directory Structure

```
Shaders/
+-- HLSL/           # DirectX shader source files
+-- GLSL/           # OpenGL shader source files
+-- Compiled/       # Generated DXR/foliage bytecode (.cso); no prebuilt Basic*.cso is shipped any more
```

---

## Shader Stages

| Stage     | HLSL Target | Description                            |
|-----------|-------------|----------------------------------------|
| `vertex`  | `vs_5_0`    | Vertex shader -- transforms vertices   |
| `pixel`   | `ps_5_0`    | Pixel/fragment shader -- pixel color   |
| `geometry` | `gs_5_0`   | Geometry shader -- processes primitives|
| `hull`    | `hs_5_0`    | Hull shader -- tessellation control    |
| `domain`  | `ds_5_0`    | Domain shader -- tessellation eval     |
| `compute` | `cs_5_0`    | Compute shader -- general-purpose GPU  |

### `ShaderType` Enum (Runtime)

```cpp
enum class ShaderType
{
    VERTEX_SHADER   = 0,
    PIXEL_SHADER    = 1,
    GEOMETRY_SHADER = 2,
    HULL_SHADER     = 3,
    DOMAIN_SHADER   = 4,
    COMPUTE_SHADER  = 5
};
```

### `ShaderStage` Enum (Cache Warming)

```cpp
namespace Spark::Graphics {
    enum class ShaderStage
    {
        Vertex,
        Pixel,
        Compute,
        Geometry,
        Hull,
        Domain
    };
}
```

---

## Constant Buffers

The shader system defines five standard constant buffer structures bound to sequential register slots. All are updated through the `Shader` class API.

### Per-Frame Constants (b0)

Updated once per frame with camera, time, and lighting data:

```cpp
struct PerFrameConstants
{
    DirectX::XMMATRIX ViewMatrix;
    DirectX::XMMATRIX ProjectionMatrix;
    DirectX::XMMATRIX ViewProjectionMatrix;
    DirectX::XMFLOAT3 CameraPosition;
    float Time;
    DirectX::XMFLOAT3 CameraDirection;
    float DeltaTime;
    DirectX::XMFLOAT2 ScreenResolution;
    DirectX::XMFLOAT2 InvScreenResolution;

    // Lighting
    DirectX::XMFLOAT3 DirectionalLightDir;
    float DirectionalLightIntensity;
    DirectX::XMFLOAT3 DirectionalLightColor;
    float AmbientIntensity;
    DirectX::XMFLOAT3 AmbientColor;
    float _padding1;
};
```

| Field                      | Type        | Description                              |
|----------------------------|-------------|------------------------------------------|
| `ViewMatrix`               | `XMMATRIX`  | Camera view matrix                       |
| `ProjectionMatrix`         | `XMMATRIX`  | Camera projection matrix                 |
| `ViewProjectionMatrix`     | `XMMATRIX`  | Combined view-projection                 |
| `CameraPosition`           | `XMFLOAT3`  | World-space camera position              |
| `Time`                     | `float`      | Total elapsed time in seconds            |
| `CameraDirection`          | `XMFLOAT3`  | Camera forward direction                 |
| `DeltaTime`                | `float`      | Frame delta time                         |
| `ScreenResolution`         | `XMFLOAT2`  | Viewport width and height in pixels      |
| `InvScreenResolution`      | `XMFLOAT2`  | 1/width and 1/height                     |
| `DirectionalLightDir`      | `XMFLOAT3`  | Sun/directional light direction          |
| `DirectionalLightIntensity`| `float`      | Directional light strength               |
| `DirectionalLightColor`    | `XMFLOAT3`  | Directional light RGB color              |
| `AmbientIntensity`         | `float`      | Ambient light strength                   |
| `AmbientColor`             | `XMFLOAT3`  | Ambient light RGB color                  |

### Per-Object Constants (b1)

Updated for each rendered object:

```cpp
struct PerObjectConstants
{
    DirectX::XMMATRIX WorldMatrix;
    DirectX::XMMATRIX WorldViewProjectionMatrix;
    DirectX::XMMATRIX WorldInverseTransposeMatrix;
    DirectX::XMMATRIX PreviousWorldMatrix;
    DirectX::XMFLOAT3 ObjectPosition;
    float ObjectScale;
    DirectX::XMFLOAT4 ObjectColor;
    DirectX::XMFLOAT4 MaterialProperties;  // x: metallic, y: roughness, z: emissive, w: alpha
    DirectX::XMFLOAT4 UVTiling;            // xy: tiling, zw: offset
};
```

| Field                        | Description                                         |
|------------------------------|-----------------------------------------------------|
| `WorldMatrix`                | Object's world transform                            |
| `WorldViewProjectionMatrix`  | Combined WVP for vertex transformation              |
| `WorldInverseTransposeMatrix`| For correct normal transformation                   |
| `PreviousWorldMatrix`        | Last frame's world matrix (motion vectors)          |
| `ObjectPosition`             | World-space object origin                           |
| `ObjectScale`                | Uniform scale factor                                |
| `ObjectColor`                | Per-object color tint (RGBA)                        |
| `MaterialProperties`         | Packed: metallic, roughness, emissive, alpha        |
| `UVTiling`                   | UV tiling (xy) and offset (zw)                      |

### Per-Material Constants (b2)

PBR material parameters:

```cpp
struct PerMaterialConstants
{
    DirectX::XMFLOAT4 AlbedoColor;
    float MetallicFactor;
    float RoughnessFactor;
    float NormalScale;
    float OcclusionStrength;
    float EmissiveFactor;
    float AlphaCutoff;
    DirectX::XMFLOAT2 _materialPadding;
};
```

### Lighting Data (b3)

Advanced multi-light support:

```cpp
struct LightingData
{
    // Directional lights (4 max)
    DirectX::XMFLOAT4 DirectionalLights[4];       // xyz: direction, w: intensity
    DirectX::XMFLOAT4 DirectionalLightColors[4];  // rgb: color, a: shadow index

    // Point lights (32 max)
    DirectX::XMFLOAT4 PointLightPositions[32];    // xyz: position, w: range
    DirectX::XMFLOAT4 PointLightColors[32];       // rgb: color, a: intensity

    // Spot lights (16 max)
    DirectX::XMFLOAT4 SpotLightPositions[16];     // xyz: position, w: range
    DirectX::XMFLOAT4 SpotLightDirections[16];    // xyz: direction, w: inner cone
    DirectX::XMFLOAT4 SpotLightColors[16];        // rgb: color, a: outer cone

    int NumDirectionalLights;
    int NumPointLights;
    int NumSpotLights;
    float LightingScale;

    // Image-Based Lighting
    float IBLIntensity;
    float IBLRotation;
    float MaxReflectionLOD;
    float _lightingPadding;
};
```

| Light Type    | Max Count | Data Layout                                  |
|---------------|-----------|----------------------------------------------|
| Directional   | 4         | Direction + intensity, color + shadow index  |
| Point         | 32        | Position + range, color + intensity          |
| Spot          | 16        | Position + range, direction + cones, color   |

### Post-Processing Constants (b4)

```cpp
struct PostProcessingConstants
{
    float Exposure;
    float Gamma;
    float Contrast;
    float Saturation;
    float Brightness;
    float Vignette;
    float FilmGrain;
    float ChromaticAberration;
    DirectX::XMFLOAT4 ColorGrading;       // xyz: shadows/midtones/highlights, w: temperature
    DirectX::XMFLOAT4 TonemappingParams;   // ACES, Reinhard, etc.
};
```

### Legacy Constant Buffer

For backward compatibility with older code:

```cpp
struct ConstantBuffer
{
    DirectX::XMMATRIX World;
    DirectX::XMMATRIX View;
    DirectX::XMMATRIX Projection;
};
```

---

## Runtime Shader Class

### `ShaderCompilationFlags` Struct

Controls how shaders are compiled at runtime:

```cpp
struct ShaderCompilationFlags
{
    bool enableDebug = false;
    bool enableOptimization = true;
    bool enableValidation = true;
    bool treatWarningsAsErrors = false;
    std::string entryPoint = "main";
    std::string target = "";                   // Auto-detected if empty
    std::vector<std::string> defines;
    std::vector<std::string> includePaths;
};
```

| Field                   | Default   | Description                                    |
|-------------------------|-----------|------------------------------------------------|
| `enableDebug`           | `false`   | Include debug symbols in compiled shader       |
| `enableOptimization`    | `true`    | Enable shader compiler optimizations           |
| `enableValidation`      | `true`    | Validate shader after compilation              |
| `treatWarningsAsErrors` | `false`   | Promote warnings to errors                     |
| `entryPoint`            | `"main"`  | Entry point function name                      |
| `target`                | `""`      | Shader model (e.g., `"vs_5_0"`); auto if empty|
| `defines`               | empty     | Preprocessor defines (e.g., `"HAS_NORMAL_MAP"`) |
| `includePaths`          | empty     | Additional include search directories          |

### `ShaderVariant` Struct

Tracks a shader variant (a base shader with specific preprocessor defines):

```cpp
struct ShaderVariant
{
    int id;
    std::string name;
    std::string baseName;
    std::vector<std::string> defines;
    bool isCompiled;
    FILETIME lastModified;
};
```

### `Shader` Class API

```cpp
class Shader
{
public:
    Shader();
    ~Shader();

    // --- Initialization ---
    HRESULT Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    void Shutdown();

    // --- Loading ---
    HRESULT LoadVertexShader(const std::wstring& filename,
                             const ShaderCompilationFlags& flags = {});
    HRESULT LoadPixelShader(const std::wstring& filename,
                            const ShaderCompilationFlags& flags = {});
    HRESULT LoadShaderFromSource(const std::string& source, ShaderType type,
                                 const ShaderCompilationFlags& flags = {});
    HRESULT LoadFromFile(const std::string& filePath, ShaderType type,
                         const ShaderCompilationFlags& flags = {});

    // --- Variants ---
    int CreateShaderVariant(const std::string& baseName,
                            const std::vector<std::string>& defines);
    void SetActiveVariant(int variantId);

    // --- Hot reload ---
    int HotReloadShaders();     // Returns number reloaded
    void SetFileCache(Spark::LocalFileCache* cache);

    // --- Binding ---
    void SetShaders();          // Bind to pipeline
    void UnbindShaders();       // Unbind all
    bool IsValid() const;       // Ready for rendering?

    // --- Constant buffer updates ---
    void UpdatePerFrameConstants(const PerFrameConstants& constants);
    void UpdatePerObjectConstants(const PerObjectConstants& constants);
    void UpdatePerMaterialConstants(const PerMaterialConstants& constants);
    void UpdateLightingData(const LightingData& lightingData);
    void UpdatePostProcessingConstants(const PostProcessingConstants& constants);

    // --- RHI cross-platform ---
    bool CompileWithRHI(const std::string& sourceFile, ShaderType type,
                        int targetBackend = 0);

    // --- Legacy ---
    void UpdateConstantBuffer(const ConstantBuffer& cb);
    static HRESULT CompileShaderFromFile(const std::wstring& filename,
                                         const std::string& entryPoint,
                                         const std::string& shaderModel,
                                         ID3DBlob** blobOut);
};
```

### Shader Resource Classes

The `Shader` class uses polymorphic resource wrappers for individual shader stages:

```cpp
class ShaderResource
{
public:
    explicit ShaderResource(ShaderType type);
    virtual ~ShaderResource() = default;

    ShaderType GetType() const;
    virtual void Bind(ID3D11DeviceContext* context) = 0;
    virtual void Unbind(ID3D11DeviceContext* context) = 0;
    virtual bool IsValid() const = 0;
};

class VertexShaderResource : public ShaderResource
{
public:
    void Bind(ID3D11DeviceContext* context) override;
    void Unbind(ID3D11DeviceContext* context) override;
    bool IsValid() const override;  // true if m_vertexShader && m_inputLayout

    ComPtr<ID3D11VertexShader> m_vertexShader;
    ComPtr<ID3D11InputLayout> m_inputLayout;
    ComPtr<ID3DBlob> m_shaderBlob;
};

class PixelShaderResource : public ShaderResource
{
public:
    void Bind(ID3D11DeviceContext* context) override;
    void Unbind(ID3D11DeviceContext* context) override;
    bool IsValid() const override;  // true if m_pixelShader

    ComPtr<ID3D11PixelShader> m_pixelShader;
};
```

### `ShaderMetrics` Struct

Performance and status metrics accessible via console:

```cpp
struct ShaderMetrics
{
    int compiledShaders = 0;
    int failedCompilations = 0;
    int activeVariants = 0;
    int hotReloadCount = 0;
    float lastCompileTime = 0.0f;     // milliseconds
    float totalCompileTime = 0.0f;    // milliseconds
    size_t shaderMemoryUsage = 0;     // bytes
    bool hotReloadEnabled = false;
};
```

### Usage Example

```cpp
Shader shader;
shader.Initialize(device, context);

// Load shaders
ShaderCompilationFlags flags;
flags.entryPoint = "VSMain";
flags.enableOptimization = true;
shader.LoadVertexShader(L"Shaders/HLSL/PBR_VS.hlsl", flags);

flags.entryPoint = "PSMain";
shader.LoadPixelShader(L"Shaders/HLSL/PBR_PS.hlsl", flags);

// Create variants for different material configurations
int normalMapVariant = shader.CreateShaderVariant("PBR", {"HAS_NORMAL_MAP"});
int fullPBRVariant = shader.CreateShaderVariant("PBR",
    {"HAS_NORMAL_MAP", "HAS_METALLIC_ROUGHNESS", "HAS_AO"});

// In render loop
PerFrameConstants frameData;
frameData.ViewMatrix = camera.GetViewMatrix();
frameData.ProjectionMatrix = camera.GetProjectionMatrix();
frameData.Time = totalTime;
shader.UpdatePerFrameConstants(frameData);

for (const auto& object : renderables)
{
    // Select variant based on material features
    if (object.material.hasNormalMap)
        shader.SetActiveVariant(normalMapVariant);

    PerObjectConstants objData;
    objData.WorldMatrix = object.GetWorldMatrix();
    shader.UpdatePerObjectConstants(objData);

    shader.SetShaders();
    object.Draw(context);
}

shader.UnbindShaders();
```

---

## Console Integration

The `Shader` class provides extensive console integration for debugging and profiling:

```cpp
// Get compilation metrics
ShaderMetrics Console_GetMetrics() const;

// Force recompile all loaded shaders
void Console_RecompileAll();

// Toggle hot reload
void Console_SetHotReload(bool enabled);

// Set debug/optimization flags
void Console_SetCompilationFlags(bool enableDebug, bool enableOptimization);

// List all loaded shaders
std::string Console_ListShaders() const;

// Get detailed info for a specific shader
std::string Console_GetShaderInfo(const std::string& shaderName) const;

// Register callback for state changes
void Console_RegisterStateCallback(std::function<void()> callback);

// Validate all loaded shaders
int Console_ValidateShaders();

// Clear the shader cache
void Console_ClearCache();

// Set search paths for shader files
void Console_SetSearchPaths(const std::vector<std::string>& paths);
```

### Thread Safety

The `Shader` class uses a `std::mutex` (`m_metricsMutex`) to protect metrics access. The metrics can be safely queried from the console thread while shaders compile on the main thread.

---

## Shader Cache Warming

### Overview

The `ShaderCacheWarming` system (`Spark::Graphics` namespace) precompiles shader permutations at startup or during loading screens to avoid runtime compilation hitches. It supports background thread compilation, incremental per-frame warming, and disk serialization.

### `ShaderPermutation` Struct

```cpp
struct ShaderPermutation
{
    std::string sourcePath;
    std::string entryPoint = "main";
    ShaderStage stage = ShaderStage::Pixel;
    std::vector<std::pair<std::string, std::string>> defines;
    uint64_t hash = 0;  // FNV-1a hash for cache lookup
};
```

### `ShaderCacheEntry` Struct

```cpp
struct ShaderCacheEntry
{
    ComPtr<ID3DBlob> bytecode;
    ShaderStage stage;
    uint64_t sourceHash;
    bool compiled = false;

    // One of these will be valid based on stage:
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11ComputeShader> cs;
    ComPtr<ID3D11GeometryShader> gs;
    ComPtr<ID3D11HullShader> hs;
    ComPtr<ID3D11DomainShader> ds;
};
```

### `ShaderCacheMetrics` Struct

```cpp
struct ShaderCacheMetrics
{
    uint32_t totalPermutations = 0;
    uint32_t compiledPermutations = 0;
    uint32_t failedPermutations = 0;
    uint32_t cacheHits = 0;
    uint32_t cacheMisses = 0;
    float warmingProgress = 0.0f;  // 0.0 to 1.0
    bool isWarming = false;
};
```

### `ShaderCacheWarming` Class API

```cpp
namespace Spark::Graphics {
    class ShaderCacheWarming
    {
    public:
        using ProgressCallback = std::function<void(float progress, const std::string& currentShader)>;

        ShaderCacheWarming() = default;
        ~ShaderCacheWarming();  // Calls StopWarming()

        bool Initialize(ID3D11Device* device);
        void Shutdown();

        // --- Registration ---
        void RegisterPermutation(const std::string& source,
                                 const std::string& entryPoint,
                                 ShaderStage stage,
                                 const std::vector<std::pair<std::string, std::string>>& defines = {});
        void RegisterPBRPermutations(const std::string& pbrShaderSource);

        // --- Warming ---
        void StartWarming(ProgressCallback callback = nullptr);  // Background thread
        bool WarmOne();                                           // Single permutation (idle-frame)
        void StopWarming();

        // --- Cache lookup ---
        const ShaderCacheEntry* GetCachedShader(uint64_t hash) const;
        static uint64_t ComputeHash(const ShaderPermutation& perm);

        // --- Disk persistence ---
        bool SaveCacheToDisk(const std::string& path) const;
        bool LoadCacheFromDisk(const std::string& path);

        // --- Metrics ---
        const ShaderCacheMetrics& GetMetrics() const;
        std::string Console_GetStatus() const;
    };
}
```

### PBR Permutation Registration

The `RegisterPBRPermutations()` method registers 16 common PBR material variants automatically:

| Variant | Defines                                                    |
|---------|------------------------------------------------------------|
| Base PBR | (none)                                                   |
| Normal mapped | `HAS_NORMAL_MAP`                                   |
| Normal + emissive | `HAS_NORMAL_MAP`, `HAS_EMISSIVE`              |
| Normal + metallic/roughness | `HAS_NORMAL_MAP`, `HAS_METALLIC_ROUGHNESS` |
| Full PBR | `HAS_NORMAL_MAP`, `HAS_METALLIC_ROUGHNESS`, `HAS_EMISSIVE` |
| Full PBR + AO | `HAS_NORMAL_MAP`, `HAS_METALLIC_ROUGHNESS`, `HAS_AO` |
| Full PBR + AO + emissive | All four defines                      |
| Alpha test | `ALPHA_TEST`                                         |
| Alpha + normal | `ALPHA_TEST`, `HAS_NORMAL_MAP`                   |
| Subsurface | `HAS_SUBSURFACE`                                     |
| Clearcoat | `HAS_CLEARCOAT`                                       |
| Anisotropy | `HAS_ANISOTROPY`                                     |
| Skinned | `SKINNED`                                                 |
| Skinned + normal | `SKINNED`, `HAS_NORMAL_MAP`                     |
| Instanced | `INSTANCED`                                             |
| Instanced + normal | `INSTANCED`, `HAS_NORMAL_MAP`                 |

Each variant registers both a vertex and pixel shader permutation.

### Warming Strategies

**Background thread warming** (loading screen):

```cpp
ShaderCacheWarming cache;
cache.Initialize(device);
cache.RegisterPBRPermutations("PBR.hlsl");

cache.StartWarming([](float progress, const std::string& shader) {
    loadingScreen.SetProgress(progress);
    loadingScreen.SetStatus("Compiling: " + shader);
});

// Loading screen loop
while (cache.GetMetrics().isWarming)
{
    loadingScreen.Render();
}
```

**Idle-frame warming** (compile one shader per frame during gameplay):

```cpp
// In game loop, after rendering
if (cache.WarmOne())
{
    // More shaders to compile next frame
}
```

### Disk Cache Persistence

Compiled bytecode can be saved to and loaded from disk to avoid recompilation on subsequent runs:

```cpp
// At startup: try loading cached bytecode
if (!cache.LoadCacheFromDisk("ShaderCache.bin"))
{
    // Cache miss: compile and save
    cache.RegisterPBRPermutations("PBR.hlsl");
    cache.StartWarming();
    // ... wait for completion ...
    cache.SaveCacheToDisk("ShaderCache.bin");
}
```

The disk format stores: entry count, then for each entry: hash (8 bytes), stage (4 bytes), bytecode size (4 bytes), and raw bytecode.

### Hash Algorithm

Permutations are identified by an FNV-1a 64-bit hash computed from the source path, entry point, stage, and all preprocessor defines:

```
hash = FNV_OFFSET_BASIS (14695981039346656037)
for each character c in (sourcePath + entryPoint + stage + defines):
    hash ^= c
    hash *= FNV_PRIME (1099511628211)
```

---

## SparkShaderCompiler (Offline Tool)

### Overview

The standalone offline shader compilation tool compiles HLSL to DXBC for `-backend d3d11` and `-backend d3d12` on Windows through `Spark::RHI::CompileShader` (`d3dcompiler_47`). Every other target fails closed with exit code 1: HLSL->SPIR-V reports `HLSL->SPIR-V cross-compilation requires DXC (dxcompiler.dll); not integrated`, and HLSL->GLSL reports that SPIRV-Cross is not integrated.

### Basic Usage

```bash
# Compile HLSL to DirectX 11 bytecode (real D3DCompile; broken HLSL fails with exit code 1)
SparkShaderCompiler BasicVS.hlsl -stage vertex -backend d3d11 -o BasicVS.cso

# Compile HLSL to DXBC for the D3D12 backend
SparkShaderCompiler PBR.hlsl -stage pixel -backend d3d12 -o PBR.cso

# Not integrated: -backend vulkan / opengl / metal exit 1 with
# "HLSL->SPIR-V cross-compilation requires DXC (dxcompiler.dll); not integrated"

# Validate without producing output
SparkShaderCompiler PBR.hlsl -stage pixel -validate

# Print shader reflection data
SparkShaderCompiler PBR.hlsl -stage pixel -backend vulkan -reflect
```

### Command-Line Options

| Flag                | Description                                           | Default     |
|---------------------|-------------------------------------------------------|-------------|
| `-stage <type>`     | Shader stage: `vertex`, `pixel`, `geometry`, `hull`, `domain`, `compute` | Inferred from filename |
| `-backend <type>`   | Target backend: `d3d11`, `d3d12`, `vulkan`, `opengl`, `auto` | `auto`    |
| `-entry <name>`     | Entry point function name                             | `main`      |
| `-o <path>`         | Output file path                                      | Inferred    |
| `-D<DEFINE>`        | Preprocessor define (e.g., `-DHAS_NORMAL_MAP`)       | (none)      |
| `-I<path>`          | Include search path                                   | (none)      |
| `-O`                | Enable optimization                                   | on          |
| `-Od`               | Disable optimization (debug builds)                   | off         |
| `-Zi`               | Include debug information                             | off         |
| `-validate`         | Validate without writing output                       | off         |
| `-reflect`          | Print shader reflection data                          | off         |
| `-batch <dir>`      | Compile all shaders in directory (recursive)          | (none)      |
| `-v`                | Verbose output                                        | off         |
| `-h`, `--help`      | Show help                                             |             |

### Stage Aliases

The `-stage` flag accepts multiple aliases for convenience:

| Stage    | Accepted Values                    |
|----------|------------------------------------|
| Vertex   | `vertex`, `vert`, `vs`             |
| Pixel    | `pixel`, `frag`, `ps`              |
| Geometry | `geometry`, `geom`, `gs`           |
| Hull     | `hull`, `hs`                       |
| Domain   | `domain`, `ds`                     |
| Compute  | `compute`, `cs`                    |

### Backend Aliases

| Backend  | Accepted Values                    |
|----------|------------------------------------|
| D3D11    | `d3d11`, `dx11`                    |
| D3D12    | `d3d12`, `dx12`                    |
| Vulkan   | `vulkan`, `vk`                     |
| OpenGL   | `opengl`, `gl`                     |
| Auto     | `auto`                             |

### Stage Inference from Filename

When `-stage` is not specified, the compiler infers the stage from the filename:

| Filename Contains | Inferred Stage |
|-------------------|----------------|
| `vs`, `vert`      | Vertex         |
| `ps`, `pixel`, `frag` | Pixel     |
| `gs`, `geom`      | Geometry       |
| `hs`, `hull`      | Hull           |
| `ds`, `domain`    | Domain         |
| `cs`, `compute`   | Compute        |

### Output Path Inference

When `-o` is not specified, the output path is inferred from the input and backend:

| Backend | Inferred Extension |
|---------|-------------------|
| D3D11   | `.cso`            |
| D3D12   | `.cso`            |
| Vulkan  | `.spv`            |
| OpenGL  | (from `GetShaderExtension()`) |

### Batch Compilation

Compile all shaders in a directory recursively:

```bash
# Compile all HLSL shaders to Vulkan SPIR-V
SparkShaderCompiler -batch Shaders/HLSL -backend vulkan

# Compile with output directory
SparkShaderCompiler -batch Shaders/HLSL -backend d3d11 -o Shaders/Compiled/
```

Recognized shader file extensions for batch mode: `.hlsl`, `.glsl`, `.vert`, `.frag`, `.comp`, `.geom`, `.tesc`, `.tese`, `.vs`, `.ps`, `.gs`, `.cs`.

The batch summary reports total, success, failure counts, and total compilation time.

### Pre-Built Batch Scripts

```bash
# Windows
.\Shaders\compile_shaders.bat

# Linux
./Shaders/compile_shaders.sh
```

### Shader Reflection

The `-reflect` flag prints input attributes and resource bindings:

```
Shader Reflection:
  Inputs: 3
    location=0 POSITION
    location=1 NORMAL
    location=2 TEXCOORD
  Resources: 2
    set=0 binding=0 PerFrameConstants (size=256)
    set=0 binding=1 PerObjectConstants (size=320)
```

---

## RHI Cross-Compilation

The RHI layer handles shader format differences between backends. The `Shader::CompileWithRHI()` method and the `SparkShaderCompiler` both use `Spark::RHI::CompileShader()` internally.

```
HLSL Source (.hlsl)
    |
    +--- D3D11/D3D12: Direct compilation via D3DCompile -> .cso bytecode
    |
    +--- Vulkan: HLSL -> SPIR-V cross-compilation -> .spv
    |
    +--- OpenGL: HLSL -> GLSL transpilation -> .glsl
```

### Supported Backends

```cpp
namespace Spark::RHI {
    enum class GraphicsBackend
    {
        D3D11,
        D3D12,
        Vulkan,
        OpenGL,
        Metal,
        Auto,   // Auto-detect best available
        None
    };
}
```

---

## Embedded Shaders

The engine supports embedded shaders compiled directly into the executable, eliminating external file dependencies for core shaders. This is used for essential shaders (e.g., error/fallback shader, debug visualization) that must always be available regardless of the file system state.

---

## Hot Reloading

**Not active in `stable-v1`.** `ShaderHotReload` registers watch directories during
shader compilation, but `Initialize()`/`SetEnabled()` are never called by the
shipped engine, so its `Update()` is a no-op; the `Shader` class below is likewise
not instantiated by `GraphicsEngine`. When the watcher *is* driven (tests, custom
hosts), a change now compiles for real through `D3DCompile` -- broken HLSL fails
and valid HLSL yields DXBC -- but nothing yet recreates the D3D11 shader object
from the new blob and swaps it into the bound pipeline.

The `Shader` class can monitor loaded shader files for modifications at runtime; this is not controlled by a root CMake option. Call `HotReloadShaders()` periodically (e.g., once per second) to check for changes and recompile:

```cpp
// In editor update loop
static float reloadTimer = 0.0f;
reloadTimer += deltaTime;
if (reloadTimer > 1.0f)
{
    reloadTimer = 0.0f;
    int reloaded = shader.HotReloadShaders();
    if (reloaded > 0)
    {
        console.Log("Hot-reloaded " + std::to_string(reloaded) + " shader(s)");
    }
}
```

The file monitoring system tracks `FILETIME` (Windows) for each watched shader file. When a change is detected, the shader is recompiled with the same flags used for the original compilation.

See [Asset Pipeline](Asset-Pipeline.md) for broader hot-reload details.

---

## Performance Considerations

- **Compilation cost**: HLSL compilation via `D3DCompile` can take 10-500ms per shader depending on complexity. Use `ShaderCacheWarming` to move this off the critical path.
- **Constant buffer updates**: Each `Update*Constants()` call performs a `Map`/`Unmap` on the GPU buffer. Batch updates by only calling when data actually changes.
- **Shader variants**: Each variant is a separate compiled shader. Limit the number of active variants to avoid excessive GPU memory usage and binding overhead.
- **Cache warming thread safety**: The `ShaderCacheWarming` class uses a `std::mutex` for all cache access and an `std::atomic<bool>` for the warming flag. The background thread compiles shaders but must acquire the lock to store results.
- **Disk cache**: Loading bytecode from disk (via `LoadCacheFromDisk`) is significantly faster than recompilation. The Windows lookup path exists and a cache hit no longer re-stores the blob, but `ShaderDiskCache` is only initialized by `Shader::Initialize`, which the shipped engine never reaches -- it is not active in shipping builds.

---

## Troubleshooting

| Problem                              | Cause                                     | Solution                                        |
|--------------------------------------|-------------------------------------------|-------------------------------------------------|
| `LoadVertexShader` returns E_FAIL    | HLSL syntax error or missing include      | Check `D3DCompile` error blob output in logs    |
| Shader appears all black             | Constant buffer not updated               | Ensure `UpdatePerFrameConstants` is called      |
| Hot reload not detecting changes     | Hot reload is not enabled in the shipped engine | Expected in `stable-v1`; see Hot Reloading above |
| Cross-compilation fails              | DXC / SPIRV-Cross are not integrated      | Only `-backend d3d11`/`d3d12` compile on Windows |
| Cache warming stalls                 | Background thread deadlock                | Check that `StopWarming()` is called in destructor |
| Input layout mismatch               | Vertex shader signature changed            | Recreate input layout after recompilation       |
| Batch compile reports failures       | Stage inference wrong for filename         | Use explicit `-stage` flag                      |

---

## See Also

- [Rendering and Graphics](../subsystems/Rendering-and-Graphics.md) — Shader usage in the rendering pipeline
- [Asset Pipeline](Asset-Pipeline.md) — Shader asset management and hot reload
- [Build System and CMake Modules](../advanced/Build-System-and-CMake-Modules.md) — Shader compilation integration
- [SparkEditor](SparkEditor.md) — Shader editing and preview
- [Day-Night-Cycle-and-Weather](Day-Night-Cycle-and-Weather.md) — Lighting parameters fed to shaders
