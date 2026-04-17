# Neural Rendering

SparkEngine's neural rendering subsystem uses small multi-layer perceptrons (MLPs) evaluated via GPU compute shaders to accelerate and enhance several rendering tasks: indirect lighting caching, texture compression, denoising, and super-resolution. The entire subsystem is self-contained with **zero external ML framework dependencies** -- all inference runs through a custom compute shader dispatch pipeline backed by D3D11 structured buffers, with an SSE2/AVX2 CPU fallback for headless and NullRHI modes.

**Source:** `SparkEngine/Source/Graphics/Neural/`
**Namespace:** `Spark::Graphics::Neural`
**CMake toggle:** `ENABLE_NEURAL_RENDERING=ON` (defines `SPARK_NEURAL_RENDERING=1`)

---

## Table of Contents

- [Architecture Overview](#architecture-overview)
- [Neural Inference Engine](#neural-inference-engine)
  - [GPU Path](#gpu-path)
  - [CPU Fallback](#cpu-fallback)
  - [Network Lifecycle](#network-lifecycle)
- [Neural Radiance Cache](#neural-radiance-cache)
  - [Hash Grid Encoding](#hash-grid-encoding)
  - [Training Loop](#training-loop)
  - [Querying Radiance](#querying-radiance)
- [Neural Texture Compression (NTC)](#neural-texture-compression-ntc)
  - [Compression Pipeline](#compression-pipeline)
  - [Quality Levels](#quality-levels)
  - [.ntex File Format](#ntex-file-format)
- [Neural Post-Processing](#neural-post-processing)
  - [Neural Denoiser](#neural-denoiser)
  - [Neural Super-Resolution](#neural-super-resolution)
- [Weight Serialization (.nnw)](#weight-serialization-nnw)
- [Console Commands](#console-commands)
- [Source Files](#source-files)
- [See Also](#see-also)

---

## Architecture Overview

All neural rendering components share a common inference backbone. Networks are described by `NetworkDesc` / `LayerDesc` structs, uploaded once, and evaluated in batches via the singleton `NeuralInferenceEngine`.

```
┌──────────────────────────────────────────────────────────────────────────┐
│                        Neural Rendering Subsystem                        │
├──────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  ┌─────────────────────┐  ┌──────────────────────┐  ┌────────────────┐  │
│  │ NeuralRadianceCache  │  │ NeuralTextureCompr.  │  │ NeuralPostProc │  │
│  │                      │  │                      │  │                │  │
│  │  Hash Grid (16 lvl)  │  │  Per-block MLPs      │  │  Denoiser      │  │
│  │  + MLP decoder       │  │  (u,v) -> RGBA       │  │  Super-Res     │  │
│  └──────────┬───────────┘  └──────────┬───────────┘  └───────┬────────┘  │
│             │                         │                      │           │
│             └─────────────┬───────────┘──────────────────────┘           │
│                           ▼                                              │
│              ┌─────────────────────────────┐                             │
│              │   NeuralInferenceEngine     │                             │
│              │   (Singleton)               │                             │
│              ├─────────────────────────────┤                             │
│              │  CreateNetwork()            │                             │
│              │  UploadWeights()            │                             │
│              │  Evaluate()   ─── GPU ──▶  Compute Shader (D3D11 CS)     │
│              │  EvaluateCPU() ── CPU ──▶  CpuNeuralInference (SIMD)     │
│              └─────────────────────────────┘                             │
│                           │                                              │
│              ┌────────────┴────────────┐                                 │
│              │     NeuralWeights       │                                 │
│              │  SaveWeights / Load     │                                 │
│              │  (.nnw binary format)   │                                 │
│              └─────────────────────────┘                                 │
│                                                                          │
│  Shared types: NeuralTypes.h (ActivationType, LayerDesc, NetworkDesc,   │
│                NetworkHandle, NeuralInferenceCB)                          │
└──────────────────────────────────────────────────────────────────────────┘
```

**Key design constraints:**

| Constant | Value | Purpose |
|----------|-------|---------|
| `kMaxNetworkLayers` | 8 | Max layers per network (shader constant buffer) |
| `kMaxNeuronsPerLayer` | 256 | Shared memory limit in compute shader |
| `kNeuralThreadGroupSize` | 64 | Thread group size for inference dispatch |

---

## Neural Inference Engine

`NeuralInferenceEngine` is the core singleton that owns all GPU-resident networks and dispatches MLP evaluation. It is initialized at engine startup (see `SparkEngine.cpp`) and registered with `EngineContext`.

### GPU Path

On D3D11, the engine compiles an HLSL compute shader at initialization. Each `Evaluate()` call:

1. Binds the network's weight structured buffer as an SRV
2. Fills a `NeuralInferenceCB` constant buffer with layer sizes, activations, and weight offsets
3. Dispatches `ceil(batchSize / 64)` thread groups
4. Writes results to the output UAV

### CPU Fallback

When no GPU is available (NullRHI, headless mode), inference falls back to `CpuNeuralInference`, which provides:

- **SIMD acceleration:** Weights are repacked into `AlignedWeightLayout` with rows padded to 8-float alignment for aligned AVX2/SSE2 loads
- **Multi-threaded batching:** Batches of 16+ samples are split across JobSystem workers
- **ISA auto-detection:** `CpuNeuralInference::Initialize()` probes the CPU and binds the fastest kernel (SSE2 or AVX2)

### Network Lifecycle

```cpp
// 1. Get the singleton (initialized at engine startup)
auto& engine = NeuralInferenceEngine::GetInstance();

// 2. Define architecture
NetworkDesc desc;
desc.name = "MyNetwork";
desc.layers = {
    {32, 64, ActivationType::ReLU},
    {64, 64, ActivationType::ReLU},
    {64, 3,  ActivationType::Sigmoid}
};

// 3. Create, upload weights, evaluate
NetworkHandle handle = engine.CreateNetwork(desc);
engine.UploadWeights(handle, trainedWeights);
engine.Evaluate(handle, inputSRV, outputUAV, batchSize);

// 4. Cleanup
engine.DestroyNetwork(handle);
```

**Activation functions** supported by the shader:

| Enum | Formula |
|------|---------|
| `ReLU` | `max(0, x)` |
| `LeakyReLU` | `x > 0 ? x : 0.01 * x` |
| `Sigmoid` | `1 / (1 + exp(-x))` |
| `Tanh` | `tanh(x)` |
| `None` | Identity (linear output) |

---

## Neural Radiance Cache

`NeuralRadianceCache` implements a multi-resolution hash grid inspired by Instant NGP for caching indirect lighting. Instead of storing full irradiance probe grids, it learns a compact neural representation that maps (position, direction) to RGB radiance.

### Hash Grid Encoding

The cache uses **16 resolution levels** (`kHashGridLevels`), each with **64K entries** (`kDefaultHashTableSize`). Each entry stores a 2-float feature vector (`kFeaturesPerEntry`). A 3D position is hashed at each level, yielding a concatenated feature vector of 32 floats (16 levels x 2 features) that is fed through a small MLP decoder.

| Parameter | Default | Description |
|-----------|---------|-------------|
| `hashTableSize` | 65536 | Entries per resolution level |
| `mlpHiddenSize` | 64 | Neurons per hidden layer |
| `mlpHiddenLayers` | 2 | Number of hidden layers |
| `learningRate` | 0.01 | SGD learning rate |
| `minResolution` | 1.0 | Finest grid cell size (world units) |
| `maxResolution` | 1024.0 | Coarsest grid cell size |
| `temporalBlend` | 0.9 | EMA blend factor with previous frame |

### Training Loop

Each frame, new radiance samples (from path tracing, screen-space GI, or other sources) are fed to `Update()`. The cache performs one round of stochastic gradient descent on the hash grid entries and MLP weights:

```cpp
NeuralRadianceCache cache;
cache.Initialize({.hashTableSize = 65536, .learningRate = 0.01f});

// Each frame: feed samples from your GI solution
std::vector<RadianceSample> samples = GatherRadianceSamples();
cache.Update(samples.data(), static_cast<uint32_t>(samples.size()), deltaTime);
```

A `RadianceSample` contains:

```cpp
struct RadianceSample
{
    float position[3];   // World-space position
    float direction[3];  // View direction (normalized)
    float radiance[3];   // RGB radiance value
};
```

### Querying Radiance

```cpp
float position[3] = {10.0f, 2.0f, -5.0f};
float direction[3] = {0.0f, 1.0f, 0.0f};
float radiance[3];

// Single query
cache.QueryCPU(position, direction, radiance);

// Batch query (more efficient)
cache.QueryBatchCPU(positions, directions, outRadiance, batchSize);
```

---

## Neural Texture Compression (NTC)

`NeuralTextureCompressor` compresses RGBA textures by training a tiny MLP per block. Each block MLP learns the mapping `(u, v) -> (r, g, b, a)` using positional encoding, and the network weights become the compressed representation. Decompression evaluates the MLP to reconstruct pixels.

### Compression Pipeline

1. **Divide** the texture into blocks (default 16x16 pixels)
2. **Encode** UV coordinates with positional encoding (sin/cos frequency bands)
3. **Train** a per-block MLP via SGD to fit the block's pixels
4. **Store** the trained weights as the compressed output

```cpp
NeuralTextureCompressor ntc;
ntc.Initialize();

NTCOptions opts;
opts.qualityLevel = 2;          // 0=fast, 1=medium, 2=high, 3=best
opts.blockSize = 16;            // 16x16 pixel blocks
opts.positionalFrequencies = 4; // sin/cos frequency bands

auto compressed = ntc.Compress(rgbaPixels, 512, 512, opts);

// Save to disk
ntc.SaveNTEX(compressed, "texture.ntex");

// Load and decompress
auto loaded = ntc.LoadNTEX("texture.ntex");
auto decoded = ntc.DecompressCPU(loaded);
```

### Quality Levels

| Level | Name | Description |
|-------|------|-------------|
| 0 | Fast | Fewest training iterations, smallest networks |
| 1 | Medium | Balanced quality/speed |
| 2 | High | Default -- good quality, moderate compression time |
| 3 | Best | Maximum training iterations, highest fidelity |

### .ntex File Format

The `.ntex` binary format stores one complete neural-compressed texture:

```
┌─────────────────────────────────────────────────┐
│ NTEXHeader (magic 'SNTX', version 1)           │
│   - width, height, channels                     │
│   - blockSize, blocksX, blocksY                 │
│   - hiddenLayers, neuronsPerLayer               │
│   - inputSize, outputSize                       │
│   - positionalFrequencies                       │
│   - weightsPerBlock, totalBlocks                │
│   - qualityLevel                                │
│   - reserved[3]                                 │
├─────────────────────────────────────────────────┤
│ Block 0 weights  [float32 x weightsPerBlock]    │
│ Block 1 weights  [float32 x weightsPerBlock]    │
│ ...                                             │
│ Block N weights  [float32 x weightsPerBlock]    │
└─────────────────────────────────────────────────┘
```

All fields are little-endian `uint32_t`. The magic number is `0x58544E53` (`"SNTX"`). All blocks share the same MLP architecture (described in the header), so only the weight data varies per block.

Compression metrics are available via `NeuralCompressedTexture`:

```cpp
size_t compressed = tex.GetCompressedSize();  // Total weight bytes
size_t original   = tex.GetOriginalSize();    // width * height * channels
float ratio       = tex.GetCompressionRatio(); // compressed / original
```

---

## Neural Post-Processing

### Neural Denoiser

`NeuralDenoiser` implements the `IDenoiser` interface with a learned MLP that processes **8x8 pixel patches** (`kDenoisePatchSize`). It accepts color input plus optional albedo and normal guide buffers.

**MLP architecture:**
- Input: noisy color (3 x 8 x 8 = 192 floats) + optional albedo (192) + normal (192)
- Hidden: 3 layers of 128 neurons, ReLU activation
- Output: denoised color (3 x 8 x 8 = 192 floats), Sigmoid activation

```cpp
NeuralDenoiser denoiser;
denoiser.Initialize(settings);
denoiser.LoadWeights(pretrainedWeights);

denoiser.SetColorInput(colorBuffer);
denoiser.SetAlbedoGuide(albedoBuffer);  // optional
denoiser.SetNormalGuide(normalBuffer);  // optional
denoiser.Execute();

const float* result = denoiser.GetOutput();
```

Falls back to pass-through if no weights are loaded (`HasWeights()` returns false).

### Neural Super-Resolution

`NeuralSuperResolution` upscales **8x8 patches to 16x16** (`kSRScaleFactor = 2`). Designed as an optional refinement pass after SparkSR temporal accumulation.

**MLP architecture:**
- Input: low-res color (3 x 8 x 8 = 192 floats) + optional depth (8 x 8 = 64 floats)
- Hidden: configurable (default 3 layers of 128 neurons)
- Output: high-res color (3 x 16 x 16 = 768 floats)

```cpp
NeuralSuperResolution sr;
sr.Initialize({.hiddenSize = 128, .hiddenLayers = 3, .useDepthInput = true});
sr.LoadWeights(pretrainedWeights);

auto upscaled = sr.UpscaleCPU(lowResInput, depthBuffer, width, height);
// upscaled is (width * 2) x (height * 2) x 3 floats
```

---

## Weight Serialization (.nnw)

The `.nnw` (Neural Network Weights) format stores a `NetworkDesc` plus all float32 weights/biases in a single binary blob:

```
┌─────────────────────────────────────────┐
│ NNWHeader                               │
│   magic:  0x574E4E53 ('SNNW')          │
│   version: 1                            │
│   layerCount                            │
│   totalParameters                       │
├─────────────────────────────────────────┤
│ LayerDesc[0] .. LayerDesc[layerCount-1] │
│   (inputSize, outputSize, activation)   │
├─────────────────────────────────────────┤
│ float32[totalParameters]                │
│   weights + biases, layer by layer      │
└─────────────────────────────────────────┘
```

```cpp
// Save
TrainedNetwork network{desc, weights};
SaveWeights(network, "model.nnw");

// Load
TrainedNetwork loaded = LoadWeights("model.nnw");
engine.CreateNetwork(loaded.desc);
engine.UploadWeights(handle, loaded.weights);
```

---

## Console Commands

Each neural rendering component exposes a `Console_GetStatus()` method that returns a diagnostic string. These are accessible through the engine's subsystem status commands:

| Command | Description |
|---------|-------------|
| `r_neural_status` | Print NeuralInferenceEngine status (GPU availability, network count, dispatch count) |
| `r_neural_cache_status` | Print NeuralRadianceCache status (hash table occupancy, sample count, training loss, memory) |
| `r_neural_ntc_status` | Print NeuralTextureCompressor status (textures compressed, compression ratio) |
| `r_neural_sr_status` | Print NeuralSuperResolution status (weights loaded, patches processed) |
| `r_neural_denoise_status` | Print NeuralDenoiser status (backend type, execution time) |

---

## Source Files

| File | Description |
|------|-------------|
| `Graphics/Neural/NeuralTypes.h` | Shared types: `ActivationType`, `LayerDesc`, `NetworkDesc`, `NetworkHandle`, `NeuralInferenceCB` |
| `Graphics/Neural/NeuralInference.h/.cpp` | `NeuralInferenceEngine` -- GPU compute shader MLP evaluation singleton |
| `Graphics/Neural/CpuNeuralInference.h/.cpp` | `CpuNeuralInference` -- SIMD-optimized CPU fallback with multi-threaded batching |
| `Graphics/Neural/NeuralRadianceCache.h/.cpp` | `NeuralRadianceCache` -- multi-resolution hash grid + MLP for indirect lighting |
| `Graphics/Neural/NeuralTextureCompressor.h/.cpp` | `NeuralTextureCompressor` -- per-block MLP texture compression |
| `Graphics/Neural/NeuralTextureFormat.h` | `.ntex` binary format header (`NTEXHeader`, constants) |
| `Graphics/Neural/NeuralPostProcessing.h/.cpp` | `NeuralDenoiser` (IDenoiser impl) and `NeuralSuperResolution` |
| `Graphics/Neural/NeuralWeights.h/.cpp` | `.nnw` weight serialization (`SaveWeights`, `LoadWeights`) |

All source paths are relative to `SparkEngine/Source/`.

---

## See Also

- [Global Illumination](Global-Illumination.md) -- DDGI and Adaptive Probe Volumes (complementary to neural radiance cache)
- [Virtual Texturing](Virtual-Texturing.md) -- Streaming virtual texture system (NTC is an alternative compression approach)
- [Post-Processing](Post-Processing.md) -- Traditional post-processing pipeline (neural denoiser/SR integrate here)
- [Upscaling (DLSS/FSR)](Upscaling-System.md) -- Temporal upscaling (neural SR is an optional enhancement pass)
- [Rendering and Graphics](../subsystems/Rendering-and-Graphics.md) -- Top-level graphics overview
- [RHI Abstraction Layer](RHI-Abstraction-Layer.md) -- Compute shader dispatch details
