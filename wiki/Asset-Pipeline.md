# Asset Pipeline

SparkEngine's asset pipeline handles loading, streaming, and management of game assets including 3D models, textures, [[Audio|audio]], and [[Scene Management|scenes]].

**Source:** `SparkEngine/Source/Graphics/AssetPipeline.h`

## Supported Formats

### 3D Models

| Format | Description | Library |
|--------|-------------|---------|
| `.obj` | Wavefront OBJ | tinyobjloader |
| `.fbx` | Autodesk FBX (meshes, skeletons, animations) | Assimp |
| `.gltf` / `.glb` | glTF 2.0 (PBR materials, animations) | Assimp |

### Textures

| Format | Description | Library |
|--------|-------------|---------|
| `.png` | Lossless compressed | stb_image |
| `.jpg` | Lossy compressed | stb_image |
| `.tga` | Targa | stb_image |
| `.bmp` | Bitmap | stb_image |
| `.hdr` | High Dynamic Range | stb_image |

### Audio

| Format | Description | Library |
|--------|-------------|---------|
| `.wav` | Waveform audio | XAudio2 / miniaudio |

### Scenes

| Format | Description |
|--------|-------------|
| `.scene` / `.json` | JSON scene files |
| `.prefab` | Prefab templates |
| `.snav` | Binary NavMesh data |

### Shaders

| Format | Description |
|--------|-------------|
| `.hlsl` | HLSL shader source |
| `.glsl` | GLSL shader source |
| `.cso` | Compiled DirectX bytecode |
| `.spv` | SPIR-V bytecode |

## Directory Conventions

```
Assets/
├── Models/          # 3D model files (.obj, .fbx, .gltf)
├── Scenes/          # Scene files (.scene, .json)
├── Scripts/         # AngelScript files (.as)
├── Textures/        # Texture files (.png, .jpg, .tga)
├── Audio/           # Sound files (.wav)
├── NavMeshes/       # Navigation mesh files (.snav)
├── Prefabs/         # Prefab templates (.prefab)
└── Cinematics/      # Cinematic sequences (.seq)

Shaders/
├── HLSL/            # DirectX shaders
├── GLSL/            # OpenGL shaders
└── Compiled/        # Pre-compiled bytecode
```

## Model Loading

### OBJ Files

```cpp
// tinyobjloader for simple static meshes
Model model;
model.LoadOBJ("Assets/Models/crate.obj");
```

### FBX / glTF Files (via Assimp)

```cpp
AssetPipeline pipeline;

// Import a model with skeletons and animations
auto result = pipeline.ImportModel("Assets/Models/character.fbx");
// result contains: meshes, materials, skeleton, animation clips
```

Assimp imports:
- Mesh geometry (vertices, normals, UVs, tangents)
- Material definitions (mapped to PBR properties)
- Bone hierarchies and skinning data
- Animation clips with keyframes

## Texture Loading

```cpp
TextureSystem textures;

// Load a texture
auto texture = textures.LoadTexture("Assets/Textures/brick_albedo.png");

// Async loading
textures.LoadTextureAsync("Assets/Textures/large_texture.png",
    [](Texture* tex) { /* callback when loaded */ });
```

### Texture Quality Levels

| Level | Description |
|-------|-------------|
| Low | Quarter resolution, basic filtering |
| Medium | Half resolution, bilinear filtering |
| High | Full resolution, anisotropic filtering |
| Ultra | Full resolution, max anisotropic filtering |

## Asset Streaming

`ENABLE_ASSET_STREAMING=ON`

Assets can be streamed in the background to avoid loading hitches:
- Texture mip-map streaming based on camera distance
- Model LOD loading on demand
- Background thread loading with callbacks

## Hot Reloading

`ENABLE_HOT_RELOAD=ON`

During development, modified assets are automatically reloaded:
- Texture files
- [[Shader Pipeline|Shader source files]]
- [[Scripting with AngelScript|AngelScript files]]
- Scene files

---

## See Also

- [[Rendering and Graphics]] — Material and texture systems
- [[Animation]] — Importing animated models
- [[Shader Pipeline]] — Shader compilation
- [[Scene Management]] — Scene file loading
- [[Audio]] — Audio asset formats and loading
- [[Terrain and Procedural Generation]] — Terrain asset streaming
- [[Entity Component System]] — Component-based asset references
