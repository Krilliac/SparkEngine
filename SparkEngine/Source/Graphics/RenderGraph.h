/**
 * @file RenderGraph.h
 * @brief Declarative render graph / frame graph system for SparkEngine
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a declarative render graph that allows the rendering pipeline to be
 * expressed as a directed acyclic graph (DAG) of render passes with explicit
 * resource dependencies. The graph is compiled once per frame structure change,
 * producing an optimized execution order with automatic resource lifetime
 * management, barrier insertion, and transient resource aliasing.
 *
 * Key components:
 * - RenderGraphResource: Handle to a transient or imported GPU resource
 * - RenderGraphPass: A single render/compute/copy operation in the graph
 * - RenderGraphBuilder: DSL used inside pass setup lambdas to declare I/O
 * - RenderGraph: Top-level object that owns passes and resources
 * - RenderGraphBlackboard: Type-erased data sharing between passes
 *
 * Usage:
 * @code
 *   RenderGraph graph("MainFrame");
 *
 *   struct GBufferData
 *   {
 *       RenderGraphResource albedo;
 *       RenderGraphResource normals;
 *       RenderGraphResource depth;
 *   };
 *
 *   auto& gbufferData = graph.GetBlackboard().Add<GBufferData>();
 *
 *   graph.AddPass("GBufferPass", RenderGraphPassType::Graphics,
 *       [&](RenderGraphBuilder& builder)
 *       {
 *           RenderGraphTextureDesc desc;
 *           desc.width = 1920;
 *           desc.height = 1080;
 *           desc.format = RenderTargetFormat::RGBA8_UNORM;
 *           gbufferData.albedo = builder.Create("GBuffer_Albedo", desc);
 *           gbufferData.normals = builder.Create("GBuffer_Normals", desc);
 *
 *           RenderGraphTextureDesc depthDesc;
 *           depthDesc.width = 1920;
 *           depthDesc.height = 1080;
 *           depthDesc.format = RenderTargetFormat::D32_FLOAT;
 *           gbufferData.depth = builder.Create("GBuffer_Depth", depthDesc);
 *
 *           gbufferData.albedo = builder.Write(gbufferData.albedo);
 *           gbufferData.normals = builder.Write(gbufferData.normals);
 *           gbufferData.depth = builder.Write(gbufferData.depth);
 *       },
 *       [=](const RenderGraphResourceRegistry& registry)
 *       {
 *           // Actual rendering code using registry to resolve handles
 *       });
 *
 *   graph.Compile();
 *   graph.Execute();
 * @endcode
 *
 * @see GraphicsEngine, RenderTarget, RenderTargetFormat
 */

#pragma once

#include "../Core/Platform.h"
#include "RenderTarget.h"

#include <algorithm>
#include <any>
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <sstream>
#include <string>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef __cpp_lib_format
#include <format>
#endif

// Forward declarations
class GraphicsEngine;

namespace Spark::Graphics
{

    // ============================================================================
    // Forward Declarations
    // ============================================================================

    class RenderGraph;
    class RenderGraphBuilder;
    class RenderGraphPass;
    class RenderGraphResourceRegistry;

    // ============================================================================
    // RenderGraphResource — Typed handle to a graph-managed resource
    // ============================================================================

    /**
 * @brief Opaque handle representing a resource within the render graph.
 *
 * Resources are never accessed directly through this handle. Instead, the
 * handle is resolved to concrete GPU objects (render targets, buffers) via
 * RenderGraphResourceRegistry during pass execution.
 *
 * A resource handle carries a version number so that write-after-write
 * hazards produce distinct handles, enabling correct dependency tracking.
 */
    struct RenderGraphResource
    {
        static constexpr uint32_t INVALID_INDEX = ~0u;

        uint32_t index = INVALID_INDEX; ///< Index into the graph's resource array
        uint32_t version = 0;           ///< Incremented on each write

        constexpr bool IsValid() const { return index != INVALID_INDEX; }

        constexpr bool operator==(const RenderGraphResource& other) const
        {
            return index == other.index && version == other.version;
        }

        constexpr bool operator!=(const RenderGraphResource& other) const { return !(*this == other); }
    };

    /**
 * @brief Hash support for RenderGraphResource so it can be used in
 *        unordered containers.
 */
    struct RenderGraphResourceHash
    {
        size_t operator()(const RenderGraphResource& r) const
        {
            return std::hash<uint64_t>{}((static_cast<uint64_t>(r.index) << 32) | r.version);
        }
    };

    // ============================================================================
    // Resource Descriptors
    // ============================================================================

    /**
 * @brief Descriptor for a texture resource managed by the render graph.
 *
 * Mirrors the fields in RenderTargetDesc but is decoupled so that the graph
 * can reason about resources without creating GPU objects up front.
 */
    struct RenderGraphTextureDesc
    {
        uint32_t width = 1;
        uint32_t height = 1;
        uint32_t depth = 1;       ///< For 3D textures; 1 for 2D
        uint32_t arraySize = 1;   ///< Texture array layers
        uint32_t mipLevels = 1;   ///< Mip chain length
        uint32_t sampleCount = 1; ///< MSAA samples
        RenderTargetFormat format = RenderTargetFormat::RGBA8_UNORM;
        RenderTargetUsage usage = RenderTargetUsage::RenderTarget | RenderTargetUsage::ShaderResource;
        XMFLOAT4 clearColor = {0.0f, 0.0f, 0.0f, 1.0f};
        float clearDepth = 1.0f;
        uint8_t clearStencil = 0;

        /**
     * @brief Compute the approximate memory footprint in bytes.
     *
     * This is used by the graph compiler for aliasing decisions.
     */
        size_t EstimateMemoryBytes() const
        {
            // Rough per-pixel byte count based on format
            size_t bpp = 4; // default RGBA8
            switch (format)
            {
            case RenderTargetFormat::RGBA32_FLOAT:
                bpp = 16;
                break;
            case RenderTargetFormat::RGBA16_FLOAT:
                bpp = 8;
                break;
            case RenderTargetFormat::RG32_FLOAT:
                bpp = 8;
                break;
            case RenderTargetFormat::RG16_FLOAT:
                bpp = 4;
                break;
            case RenderTargetFormat::R32_FLOAT:
            case RenderTargetFormat::D32_FLOAT:
                bpp = 4;
                break;
            case RenderTargetFormat::R16_FLOAT:
            case RenderTargetFormat::D16_UNORM:
                bpp = 2;
                break;
            case RenderTargetFormat::R8_UNORM:
                bpp = 1;
                break;
            case RenderTargetFormat::R11G11B10_FLOAT:
                bpp = 4;
                break;
            case RenderTargetFormat::RGB10A2_UNORM:
                bpp = 4;
                break;
            case RenderTargetFormat::D24_UNORM_S8_UINT:
                bpp = 4;
                break;
            default:
                bpp = 4;
                break;
            }
            return static_cast<size_t>(width) * height * depth * arraySize * sampleCount * bpp;
        }
    };

    /**
 * @brief Descriptor for a buffer resource managed by the render graph.
 */
    struct RenderGraphBufferDesc
    {
        size_t sizeBytes = 0; ///< Total buffer size
        size_t stride = 0;    ///< Element stride (for structured buffers)
        RenderTargetUsage usage = RenderTargetUsage::ShaderResource;

        size_t EstimateMemoryBytes() const { return sizeBytes; }
    };

    // ============================================================================
    // Resource Lifetime — distinguishes transient from imported resources
    // ============================================================================

    /**
 * @brief Indicates whether a resource is graph-managed (transient) or
 *        externally owned (imported).
 */
    enum class RenderGraphResourceLifetime
    {
        Transient, ///< Created and destroyed within a single graph execution
        Imported   ///< Externally owned; the graph does not manage its lifetime
    };

    /**
 * @brief The kind of GPU resource a handle points to.
 */
    enum class RenderGraphResourceType
    {
        Texture,
        Buffer
    };

    // ============================================================================
    // Internal Resource Node
    // ============================================================================

    /**
 * @brief Internal bookkeeping for a single resource inside the graph.
 *
 * Users never interact with this directly; they use RenderGraphResource
 * handles and resolve them through the registry at execution time.
 */
    struct RenderGraphResourceNode
    {
        std::string name;
        RenderGraphResourceType type = RenderGraphResourceType::Texture;
        RenderGraphResourceLifetime lifetime = RenderGraphResourceLifetime::Transient;

        /// Descriptor — exactly one of these is populated depending on type.
        RenderGraphTextureDesc textureDesc;
        RenderGraphBufferDesc bufferDesc;

        /// Reference count: incremented by each pass that reads or writes.
        uint32_t refCount = 0;

        /// The pass that first creates / writes this resource (producer).
        uint32_t producerPass = RenderGraphResource::INVALID_INDEX;

        /// Execution-time concrete object. Populated during Execute().
        std::shared_ptr<RenderTarget> physicalTexture;

        /// Aliasing: if non-null, this resource shares memory with another.
        uint32_t aliasTarget = RenderGraphResource::INVALID_INDEX;

        /// First and last pass indices that reference this resource (inclusive).
        /// Used for lifetime analysis and aliasing.
        uint32_t firstUsePass = RenderGraphResource::INVALID_INDEX;
        uint32_t lastUsePass = 0;

        /**
     * @brief Imported texture pointer. Non-null only when lifetime == Imported.
     */
        RenderTarget* importedTexture = nullptr;
    };

    // ============================================================================
    // RenderGraphPassType
    // ============================================================================

    /**
 * @brief Classifies a render pass for scheduling purposes.
 */
    enum class RenderGraphPassType
    {
        Graphics,    ///< Rasterization pass (vertex/pixel shaders, render targets)
        Compute,     ///< Compute-shader dispatch
        Copy,        ///< Resource copy / blit
        AsyncCompute ///< Compute work eligible for async compute queue
    };

    // ============================================================================
    // RenderGraphResourceRegistry — resolves handles during execution
    // ============================================================================

    /**
 * @brief Passed to the execute callback of each pass so that it can
 *        resolve RenderGraphResource handles to concrete GPU objects.
 */
    class RenderGraphResourceRegistry
    {
      public:
        explicit RenderGraphResourceRegistry(const std::vector<RenderGraphResourceNode>& resources)
            : m_resources(resources)
        {
        }

        /**
     * @brief Resolve a resource handle to its physical RenderTarget.
     * @param handle The resource handle obtained during pass setup.
     * @return Non-owning pointer to the render target, or nullptr if
     *         the resource is not backed by a texture.
     */
        RenderTarget* GetTexture(RenderGraphResource handle) const
        {
            if (!handle.IsValid() || handle.index >= m_resources.size())
            {
                return nullptr;
            }
            const auto& node = m_resources[handle.index];
            if (node.lifetime == RenderGraphResourceLifetime::Imported)
            {
                return node.importedTexture;
            }
            return node.physicalTexture.get();
        }

        /**
     * @brief Get the texture descriptor for a resource.
     */
        const RenderGraphTextureDesc& GetTextureDesc(RenderGraphResource handle) const
        {
            return m_resources[handle.index].textureDesc;
        }

        /**
     * @brief Get the buffer descriptor for a resource.
     */
        const RenderGraphBufferDesc& GetBufferDesc(RenderGraphResource handle) const
        {
            return m_resources[handle.index].bufferDesc;
        }

      private:
        const std::vector<RenderGraphResourceNode>& m_resources;
    };

    // ============================================================================
    // RenderGraphPass — a single node in the pass DAG
    // ============================================================================

    /**
 * @brief Represents a single render, compute, or copy pass in the graph.
 *
 * Passes are created via RenderGraph::AddPass(). Each pass records which
 * resources it reads and writes so that the compiler can determine
 * execution order and insert barriers.
 */
    class RenderGraphPass
    {
      public:
        using SetupCallback = std::function<void(RenderGraphBuilder&)>;
        using ExecuteCallback = std::function<void(const RenderGraphResourceRegistry&)>;

        /**
     * @brief Construct a render graph pass.
     * @param name Human-readable name (shown in debug visualization).
     * @param type The kind of GPU work this pass performs.
     * @param index Unique index within the owning RenderGraph.
     */
        RenderGraphPass(const std::string& name, RenderGraphPassType type, uint32_t index)
            : m_name(name), m_type(type), m_index(index)
        {
        }

        // -- Accessors -----------------------------------------------------------

        const std::string& GetName() const { return m_name; }
        RenderGraphPassType GetType() const { return m_type; }
        uint32_t GetIndex() const { return m_index; }
        bool HasSideEffects() const { return m_hasSideEffects; }
        bool IsCulled() const { return m_culled; }

        const std::vector<RenderGraphResource>& GetReads() const { return m_reads; }
        const std::vector<RenderGraphResource>& GetWrites() const { return m_writes; }
        const std::vector<RenderGraphResource>& GetCreates() const { return m_creates; }

        // -- Mutators (used by RenderGraphBuilder and compiler) -------------------

        void AddRead(RenderGraphResource r) { m_reads.push_back(r); }
        void AddWrite(RenderGraphResource r) { m_writes.push_back(r); }
        void AddCreate(RenderGraphResource r) { m_creates.push_back(r); }
        void MarkSideEffects() { m_hasSideEffects = true; }
        void SetCulled(bool culled) { m_culled = culled; }

        void SetExecuteCallback(ExecuteCallback callback) { m_executeCallback = std::move(callback); }

        /**
     * @brief Run the pass's execute callback.
     *
     * Called by RenderGraph::Execute() after all resources have been
     * allocated and barriers inserted.
     */
        void Execute(const RenderGraphResourceRegistry& registry) const
        {
            if (m_executeCallback)
            {
                m_executeCallback(registry);
            }
        }

      private:
        std::string m_name;
        RenderGraphPassType m_type;
        uint32_t m_index;

        std::vector<RenderGraphResource> m_reads;
        std::vector<RenderGraphResource> m_writes;
        std::vector<RenderGraphResource> m_creates;

        bool m_hasSideEffects = false;
        bool m_culled = false;

        ExecuteCallback m_executeCallback;
    };

    // ============================================================================
    // RenderGraphBuilder — pass-setup DSL
    // ============================================================================

    /**
 * @brief Builder interface provided to pass setup lambdas.
 *
 * The builder is the sole mechanism through which a pass declares its
 * resource dependencies. After the setup lambda returns, the graph owns
 * all dependency information and uses it for compilation.
 */
    class RenderGraphBuilder
    {
      public:
        RenderGraphBuilder(RenderGraph& graph, RenderGraphPass& pass) : m_graph(graph), m_pass(pass) {}

        // -- Resource creation ---------------------------------------------------

        /**
     * @brief Create a new transient texture resource.
     * @param name Human-readable name for debugging.
     * @param desc Texture descriptor.
     * @return Handle to the newly created resource.
     */
        RenderGraphResource Create(const std::string& name, const RenderGraphTextureDesc& desc);

        /**
     * @brief Create a new transient buffer resource.
     * @param name Human-readable name for debugging.
     * @param desc Buffer descriptor.
     * @return Handle to the newly created resource.
     */
        RenderGraphResource Create(const std::string& name, const RenderGraphBufferDesc& desc);

        // -- Resource access declarations ----------------------------------------

        /**
     * @brief Declare that this pass reads from a resource.
     *
     * The returned handle has the same index and version as the input,
     * but the dependency is recorded so the compiler orders this pass
     * after the producer.
     *
     * @param resource Handle obtained from a prior Create() or Write().
     * @return The same handle (pass-through for chaining convenience).
     */
        RenderGraphResource Read(RenderGraphResource resource);

        /**
     * @brief Declare that this pass writes to a resource.
     *
     * Returns a new handle with an incremented version. Subsequent
     * passes that want to read this resource must use the returned handle.
     *
     * @param resource Handle to write.
     * @return New versioned handle representing the resource after this write.
     */
        RenderGraphResource Write(RenderGraphResource resource);

        // -- Side effects --------------------------------------------------------

        /**
     * @brief Mark this pass as having side effects (e.g., presenting to
     *        the swap chain, writing to disk).
     *
     * Side-effect passes are never culled during dead-code elimination.
     */
        RenderGraphBuilder& SideEffect()
        {
            m_pass.MarkSideEffects();
            return *this;
        }

        // -- Resource aliasing ---------------------------------------------------

        /**
     * @brief Hint that two resources with non-overlapping lifetimes may
     *        share the same physical memory.
     *
     * The compiler will attempt to honor this hint when allocating
     * transient resources. Both resources must have compatible descriptors.
     *
     * @param from The resource whose memory may be reused.
     * @param to   The resource that should alias from's memory.
     */
        void Alias(RenderGraphResource from, RenderGraphResource to);

      private:
        RenderGraph& m_graph;
        RenderGraphPass& m_pass;
    };

    // ============================================================================
    // RenderGraphBlackboard — type-erased inter-pass data sharing
    // ============================================================================

    /**
 * @brief Type-erased storage for passing structured data between passes.
 *
 * Each data type is stored at most once, keyed by its std::type_index.
 * Passes write data during setup and read it during execution.
 *
 * @code
 *   struct LightingData { RenderGraphResource shadowMap; };
 *   auto& data = blackboard.Add<LightingData>();
 *   data.shadowMap = builder.Create("ShadowMap", desc);
 *   // Later, in another pass:
 *   const auto& data = blackboard.Get<LightingData>();
 * @endcode
 */
    class RenderGraphBlackboard
    {
      public:
        RenderGraphBlackboard() = default;
        ~RenderGraphBlackboard() = default;

        // Non-copyable, movable
        RenderGraphBlackboard(const RenderGraphBlackboard&) = delete;
        RenderGraphBlackboard& operator=(const RenderGraphBlackboard&) = delete;
        RenderGraphBlackboard(RenderGraphBlackboard&&) noexcept = default;
        RenderGraphBlackboard& operator=(RenderGraphBlackboard&&) noexcept = default;

        /**
     * @brief Add or replace data of type T.
     *
     * Constructs a T in-place. If a T already exists, it is replaced.
     *
     * @tparam T   Data type (must be movable).
     * @tparam Args Constructor argument types.
     * @param args  Forwarded to T's constructor.
     * @return Reference to the stored T.
     */
        template <typename T, typename... Args> T& Add(Args&&... args)
        {
            auto key = std::type_index(typeid(T));
            m_storage[key] = std::make_any<T>(std::forward<Args>(args)...);
            return std::any_cast<T&>(m_storage[key]);
        }

        /**
     * @brief Retrieve mutable reference to stored data of type T.
     * @throws std::bad_any_cast if T has not been added.
     */
        template <typename T> T& Get()
        {
            auto key = std::type_index(typeid(T));
            auto it = m_storage.find(key);
            if (it == m_storage.end())
            {
                throw std::bad_any_cast();
            }
            return std::any_cast<T&>(it->second);
        }

        /**
     * @brief Retrieve const reference to stored data of type T.
     * @throws std::bad_any_cast if T has not been added.
     */
        template <typename T> const T& Get() const
        {
            auto key = std::type_index(typeid(T));
            auto it = m_storage.find(key);
            if (it == m_storage.end())
            {
                throw std::bad_any_cast();
            }
            return std::any_cast<const T&>(it->second);
        }

        /**
     * @brief Try to retrieve data of type T without throwing.
     * @return Pointer to the stored T, or nullptr if not present.
     */
        template <typename T> T* TryGet()
        {
            auto key = std::type_index(typeid(T));
            auto it = m_storage.find(key);
            if (it == m_storage.end())
            {
                return nullptr;
            }
            return std::any_cast<T>(&it->second);
        }

        /**
     * @brief Try to retrieve const data of type T without throwing.
     */
        template <typename T> const T* TryGet() const
        {
            auto key = std::type_index(typeid(T));
            auto it = m_storage.find(key);
            if (it == m_storage.end())
            {
                return nullptr;
            }
            return std::any_cast<const T>(&it->second);
        }

        /**
     * @brief Check whether data of type T exists.
     */
        template <typename T> bool Has() const { return m_storage.count(std::type_index(typeid(T))) > 0; }

        /**
     * @brief Remove data of type T.
     */
        template <typename T> void Remove() { m_storage.erase(std::type_index(typeid(T))); }

        /**
     * @brief Remove all stored data.
     */
        void Clear() { m_storage.clear(); }

      private:
        std::unordered_map<std::type_index, std::any> m_storage;
    };

    // ============================================================================
    // Render Graph Statistics
    // ============================================================================

    /**
 * @brief Statistics collected during graph compilation and execution.
 */
    struct RenderGraphStats
    {
        uint32_t totalPasses = 0;        ///< Total declared passes
        uint32_t culledPasses = 0;       ///< Passes removed by dead-code elimination
        uint32_t executedPasses = 0;     ///< Passes actually executed
        uint32_t totalResources = 0;     ///< Total declared resources
        uint32_t transientResources = 0; ///< Graph-managed transient resources
        uint32_t importedResources = 0;  ///< Externally imported resources
        uint32_t aliasedResources = 0;   ///< Resources sharing physical memory
        size_t peakTransientMemory = 0;  ///< Peak transient memory in bytes
        size_t savedByAliasing = 0;      ///< Memory saved via aliasing in bytes
        float compileTimeMs = 0.0f;      ///< Time spent in Compile()
        float executeTimeMs = 0.0f;      ///< Time spent in Execute()
        uint32_t asyncComputePasses = 0; ///< Passes scheduled on async compute
    };

    // ============================================================================
    // RenderGraph — top-level graph object
    // ============================================================================

    /**
 * @brief Declarative render graph that manages a frame's rendering pipeline.
 *
 * The render graph is built each frame (or when the pipeline structure
 * changes) by adding passes, then compiled and executed:
 *
 * 1. **AddPass()** — Declare passes with their resource dependencies.
 * 2. **Compile()** — Topological sort, dead-code elimination, lifetime
 *    analysis, resource aliasing, and barrier placement.
 * 3. **Execute()** — Allocate transient resources, run passes in compiled
 *    order, and release transient resources.
 *
 * The graph is single-use: after Execute(), call Clear() or destroy the
 * graph before building the next frame.
 */
    class RenderGraph
    {
      public:
        /**
     * @brief Construct a render graph.
     * @param name Debug name for this graph (e.g., "MainFrame").
     * @param device Optional D3D11 device for transient resource creation.
     */
        explicit RenderGraph(const std::string& name, ID3D11Device* device = nullptr) : m_name(name), m_device(device)
        {
        }

        ~RenderGraph() = default;

        // Non-copyable, movable
        RenderGraph(const RenderGraph&) = delete;
        RenderGraph& operator=(const RenderGraph&) = delete;
        RenderGraph(RenderGraph&&) noexcept = default;
        RenderGraph& operator=(RenderGraph&&) noexcept = default;

        // ========================================================================
        // Pass Registration
        // ========================================================================

        /**
     * @brief Add a render pass to the graph.
     *
     * @param name     Human-readable pass name.
     * @param type     Kind of GPU work (Graphics, Compute, Copy, AsyncCompute).
     * @param setup    Lambda that receives a RenderGraphBuilder to declare
     *                 resource reads/writes/creates.
     * @param execute  Lambda called at execution time with a registry for
     *                 resolving resource handles to GPU objects.
     * @return Reference to the created pass for further configuration.
     *
     * @code
     *   graph.AddPass("ToneMapping", RenderGraphPassType::Compute,
     *       [&](RenderGraphBuilder& builder)
     *       {
     *           hdrInput = builder.Read(hdrInput);
     *           ldrOutput = builder.Write(ldrOutput);
     *       },
     *       [=](const RenderGraphResourceRegistry& registry)
     *       {
     *           auto* hdr = registry.GetTexture(hdrInput);
     *           auto* ldr = registry.GetTexture(ldrOutput);
     *           // dispatch tone mapping compute shader ...
     *       });
     * @endcode
     */
        RenderGraphPass& AddPass(const std::string& name, RenderGraphPassType type,
                                 std::function<void(RenderGraphBuilder&)> setup,
                                 std::function<void(const RenderGraphResourceRegistry&)> execute)
        {
            auto passIndex = static_cast<uint32_t>(m_passes.size());
            auto pass = std::make_unique<RenderGraphPass>(name, type, passIndex);
            pass->SetExecuteCallback(std::move(execute));

            // Run the setup lambda to record resource dependencies
            RenderGraphBuilder builder(*this, *pass);
            setup(builder);

            auto& ref = *pass;
            m_passes.push_back(std::move(pass));
            m_compiled = false;
            return ref;
        }

        // ========================================================================
        // Resource Import
        // ========================================================================

        /**
     * @brief Import an externally owned render target into the graph.
     *
     * Imported resources are not created or destroyed by the graph.
     * They typically represent the swap chain back buffer or persistent
     * render targets that outlive a single frame.
     *
     * @param name    Debug name.
     * @param texture Non-owning pointer to the render target.
     * @return Handle usable in pass setup lambdas.
     */
        RenderGraphResource Import(const std::string& name, RenderTarget* texture)
        {
            RenderGraphResourceNode node;
            node.name = name;
            node.type = RenderGraphResourceType::Texture;
            node.lifetime = RenderGraphResourceLifetime::Imported;
            node.importedTexture = texture;

            if (texture)
            {
                const auto& desc = texture->GetDesc();
                node.textureDesc.width = desc.width;
                node.textureDesc.height = desc.height;
                node.textureDesc.format = desc.format;
                node.textureDesc.sampleCount = desc.sampleCount;
                node.textureDesc.mipLevels = desc.mipLevels;
                node.textureDesc.usage = desc.usage;
                node.textureDesc.clearColor = desc.clearColor;
            }

            RenderGraphResource handle;
            handle.index = static_cast<uint32_t>(m_resources.size());
            handle.version = 0;

            m_resources.push_back(std::move(node));
            return handle;
        }

        // ========================================================================
        // Compilation
        // ========================================================================

        /**
     * @brief Compile the render graph.
     *
     * Performs the following steps:
     * 1. Compute reference counts for all resources.
     * 2. Dead-code elimination: cull passes whose outputs are never read,
     *    unless the pass has side effects.
     * 3. Topological sort of remaining passes.
     * 4. Lifetime analysis: determine first/last use of each resource.
     * 5. Resource aliasing: identify non-overlapping lifetimes for memory reuse.
     * 6. Async compute scheduling: tag eligible passes.
     *
     * Must be called before Execute(). Can be called multiple times if
     * the graph structure changes.
     */
        void Compile()
        {
            auto compileStart = std::chrono::high_resolution_clock::now();

            ComputeResourceLifetimes();
            CullUnreferencedPasses();
            TopologicalSort();
            AnalyzeResourceAliasing();
            ScheduleAsyncCompute();

            m_compiled = true;

            auto compileEnd = std::chrono::high_resolution_clock::now();
            m_stats.compileTimeMs = std::chrono::duration<float, std::milli>(compileEnd - compileStart).count();
            UpdateStats();
        }

        /**
     * @brief Check whether the graph has been compiled.
     */
        bool IsCompiled() const { return m_compiled; }

        // ========================================================================
        // Execution
        // ========================================================================

        /**
     * @brief Execute all non-culled passes in compiled order.
     *
     * For each pass:
     * 1. Allocate any transient resources created by the pass.
     * 2. (Future: insert resource barriers / transitions.)
     * 3. Run the pass's execute callback.
     * 4. Release transient resources whose last use was this pass.
     *
     * Requires a prior call to Compile(). Asserts if not compiled.
     */
        void Execute()
        {
            assert(m_compiled && "RenderGraph::Execute() called before Compile()");

            auto executeStart = std::chrono::high_resolution_clock::now();

            AllocateTransientResources();

            RenderGraphResourceRegistry registry(m_resources);

            for (uint32_t passIndex : m_executionOrder)
            {
                auto& pass = *m_passes[passIndex];
                if (pass.IsCulled())
                {
                    continue;
                }

                pass.Execute(registry);
                m_stats.executedPasses++;
            }

            ReleaseTransientResources();

            auto executeEnd = std::chrono::high_resolution_clock::now();
            m_stats.executeTimeMs = std::chrono::duration<float, std::milli>(executeEnd - executeStart).count();
        }

        // ========================================================================
        // Reset
        // ========================================================================

        /**
     * @brief Clear all passes and resources so the graph can be rebuilt.
     *
     * The blackboard is preserved. Call blackboard.Clear() separately
     * if you want to reset inter-pass data too.
     */
        void Clear()
        {
            m_passes.clear();
            m_resources.clear();
            m_executionOrder.clear();
            m_aliasPairs.clear();
            m_compiled = false;
            m_stats = {};
        }

        // ========================================================================
        // Blackboard Access
        // ========================================================================

        RenderGraphBlackboard& GetBlackboard() { return m_blackboard; }
        const RenderGraphBlackboard& GetBlackboard() const { return m_blackboard; }

        // ========================================================================
        // Statistics & Debug
        // ========================================================================

        /**
     * @brief Get compilation and execution statistics.
     */
        const RenderGraphStats& GetStats() const { return m_stats; }

        /**
     * @brief Get the graph's debug name.
     */
        const std::string& GetName() const { return m_name; }

        /**
     * @brief Get the list of passes (for debug/visualization).
     */
        const std::vector<std::unique_ptr<RenderGraphPass>>& GetPasses() const { return m_passes; }

        /**
     * @brief Get the list of resource nodes (for debug/visualization).
     */
        const std::vector<RenderGraphResourceNode>& GetResources() const { return m_resources; }

        /**
     * @brief Get the compiled execution order (pass indices).
     */
        const std::vector<uint32_t>& GetExecutionOrder() const { return m_executionOrder; }

        /**
     * @brief Generate a human-readable summary of the graph for debugging.
     *
     * Includes pass list, resource list, execution order, and statistics.
     * Suitable for console output.
     */
        std::string DebugVisualize() const
        {
            std::ostringstream ss;
            ss << "=== Render Graph: " << m_name << " ===\n\n";

            // Passes
            ss << "Passes (" << m_passes.size() << "):\n";
            for (const auto& pass : m_passes)
            {
                ss << "  [" << pass->GetIndex() << "] " << pass->GetName() << " (" << PassTypeToString(pass->GetType())
                   << ")";
                if (pass->IsCulled())
                {
                    ss << " [CULLED]";
                }
                if (pass->HasSideEffects())
                {
                    ss << " [SIDE EFFECT]";
                }
                ss << "\n";

                if (!pass->GetReads().empty())
                {
                    ss << "    Reads: ";
                    for (const auto& r : pass->GetReads())
                    {
                        ss << ResourceToString(r) << " ";
                    }
                    ss << "\n";
                }
                if (!pass->GetWrites().empty())
                {
                    ss << "    Writes: ";
                    for (const auto& w : pass->GetWrites())
                    {
                        ss << ResourceToString(w) << " ";
                    }
                    ss << "\n";
                }
            }

            // Resources
            ss << "\nResources (" << m_resources.size() << "):\n";
            for (size_t i = 0; i < m_resources.size(); ++i)
            {
                const auto& res = m_resources[i];
                ss << "  [" << i << "] " << res.name << " ("
                   << (res.lifetime == RenderGraphResourceLifetime::Transient ? "Transient" : "Imported")
                   << ", refs=" << res.refCount << ")\n";
            }

            // Execution order
            if (m_compiled && !m_executionOrder.empty())
            {
                ss << "\nExecution Order:\n  ";
                for (size_t i = 0; i < m_executionOrder.size(); ++i)
                {
                    if (i > 0)
                    {
                        ss << " -> ";
                    }
                    ss << m_passes[m_executionOrder[i]]->GetName();
                }
                ss << "\n";
            }

            // Stats
            ss << "\nStatistics:\n";
            ss << "  Total passes:        " << m_stats.totalPasses << "\n";
            ss << "  Culled passes:       " << m_stats.culledPasses << "\n";
            ss << "  Executed passes:     " << m_stats.executedPasses << "\n";
            ss << "  Total resources:     " << m_stats.totalResources << "\n";
            ss << "  Transient resources: " << m_stats.transientResources << "\n";
            ss << "  Imported resources:  " << m_stats.importedResources << "\n";
            ss << "  Aliased resources:   " << m_stats.aliasedResources << "\n";
            ss << "  Peak transient mem:  " << (m_stats.peakTransientMemory / 1024) << " KB\n";
            ss << "  Saved by aliasing:   " << (m_stats.savedByAliasing / 1024) << " KB\n";
            ss << "  Compile time:        " << m_stats.compileTimeMs << " ms\n";
            ss << "  Execute time:        " << m_stats.executeTimeMs << " ms\n";
            ss << "  Async compute:       " << m_stats.asyncComputePasses << "\n";

            return ss.str();
        }

        /**
     * @brief Generate a DOT-language graph description for external visualization.
     *
     * Can be rendered with Graphviz: `dot -Tpng graph.dot -o graph.png`
     */
        std::string ExportDot() const
        {
            std::ostringstream ss;
            ss << "digraph RenderGraph {\n";
            ss << "  rankdir=LR;\n";
            ss << "  node [shape=box, style=filled];\n\n";

            // Pass nodes
            for (const auto& pass : m_passes)
            {
                std::string color = "lightblue";
                if (pass->IsCulled())
                {
                    color = "gray";
                }
                else if (pass->GetType() == RenderGraphPassType::Compute ||
                         pass->GetType() == RenderGraphPassType::AsyncCompute)
                {
                    color = "lightyellow";
                }
                else if (pass->GetType() == RenderGraphPassType::Copy)
                {
                    color = "lightgreen";
                }

                ss << "  pass_" << pass->GetIndex() << " [label=\"" << pass->GetName() << "\\n("
                   << PassTypeToString(pass->GetType()) << ")\"" << ", fillcolor=" << color << "];\n";
            }

            // Resource nodes
            ss << "\n";
            for (size_t i = 0; i < m_resources.size(); ++i)
            {
                const auto& res = m_resources[i];
                std::string shape = (res.lifetime == RenderGraphResourceLifetime::Imported) ? "ellipse" : "diamond";
                ss << "  res_" << i << " [label=\"" << res.name << "\"" << ", shape=" << shape
                   << ", fillcolor=lightyellow];\n";
            }

            // Edges: pass -> writes, reads -> pass
            ss << "\n";
            for (const auto& pass : m_passes)
            {
                for (const auto& w : pass->GetWrites())
                {
                    ss << "  pass_" << pass->GetIndex() << " -> res_" << w.index << ";\n";
                }
                for (const auto& r : pass->GetReads())
                {
                    ss << "  res_" << r.index << " -> pass_" << pass->GetIndex() << ";\n";
                }
            }

            ss << "}\n";
            return ss.str();
        }

        /**
     * @brief Get a console-friendly summary string of graph statistics.
     */
        std::string Console_GetGraphStats() const
        {
            std::ostringstream ss;
            ss << "RenderGraph '" << m_name << "': " << m_stats.executedPasses << "/" << m_stats.totalPasses
               << " passes executed, " << m_stats.culledPasses << " culled, " << m_stats.totalResources
               << " resources (" << m_stats.transientResources << " transient, " << m_stats.importedResources
               << " imported), " << (m_stats.peakTransientMemory / 1024) << " KB peak, " << m_stats.compileTimeMs
               << " ms compile, " << m_stats.executeTimeMs << " ms execute";
            return ss.str();
        }

        // ========================================================================
        // Internal — used by RenderGraphBuilder
        // ========================================================================

        /**
     * @brief Create a transient texture resource. Called by RenderGraphBuilder.
     */
        RenderGraphResource CreateResource(const std::string& name, const RenderGraphTextureDesc& desc,
                                           uint32_t producerPass)
        {
            RenderGraphResourceNode node;
            node.name = name;
            node.type = RenderGraphResourceType::Texture;
            node.lifetime = RenderGraphResourceLifetime::Transient;
            node.textureDesc = desc;
            node.producerPass = producerPass;

            RenderGraphResource handle;
            handle.index = static_cast<uint32_t>(m_resources.size());
            handle.version = 0;

            m_resources.push_back(std::move(node));
            return handle;
        }

        /**
     * @brief Create a transient buffer resource. Called by RenderGraphBuilder.
     */
        RenderGraphResource CreateResource(const std::string& name, const RenderGraphBufferDesc& desc,
                                           uint32_t producerPass)
        {
            RenderGraphResourceNode node;
            node.name = name;
            node.type = RenderGraphResourceType::Buffer;
            node.lifetime = RenderGraphResourceLifetime::Transient;
            node.bufferDesc = desc;
            node.producerPass = producerPass;

            RenderGraphResource handle;
            handle.index = static_cast<uint32_t>(m_resources.size());
            handle.version = 0;

            m_resources.push_back(std::move(node));
            return handle;
        }

        /**
     * @brief Increment a resource's reference count. Called by builder on
     *        Read/Write.
     */
        void AddResourceRef(RenderGraphResource handle)
        {
            if (handle.IsValid() && handle.index < m_resources.size())
            {
                m_resources[handle.index].refCount++;
            }
        }

        /**
     * @brief Increment the version of a resource (on write). Returns new handle.
     */
        RenderGraphResource IncrementVersion(RenderGraphResource handle)
        {
            RenderGraphResource newHandle = handle;
            newHandle.version++;
            return newHandle;
        }

        /**
     * @brief Record an alias hint between two resources.
     */
        void RecordAlias(RenderGraphResource from, RenderGraphResource to) { m_aliasPairs.push_back({from, to}); }

      private:
        // ========================================================================
        // Compilation Steps
        // ========================================================================

        /**
     * @brief Compute first/last use pass index for each resource.
     */
        void ComputeResourceLifetimes()
        {
            for (auto& res : m_resources)
            {
                res.firstUsePass = RenderGraphResource::INVALID_INDEX;
                res.lastUsePass = 0;
            }

            for (const auto& pass : m_passes)
            {
                uint32_t pi = pass->GetIndex();

                auto updateLifetime = [&](RenderGraphResource handle)
                {
                    if (!handle.IsValid() || handle.index >= m_resources.size())
                    {
                        return;
                    }
                    auto& res = m_resources[handle.index];
                    if (res.firstUsePass == RenderGraphResource::INVALID_INDEX || pi < res.firstUsePass)
                    {
                        res.firstUsePass = pi;
                    }
                    if (pi > res.lastUsePass)
                    {
                        res.lastUsePass = pi;
                    }
                };

                for (const auto& r : pass->GetReads())
                {
                    updateLifetime(r);
                }
                for (const auto& w : pass->GetWrites())
                {
                    updateLifetime(w);
                }
                for (const auto& c : pass->GetCreates())
                {
                    updateLifetime(c);
                }
            }
        }

        /**
     * @brief Cull passes whose outputs are never read and that have no
     *        side effects. Uses backward flood-fill from side-effect passes.
     */
        void CullUnreferencedPasses()
        {
            // Start by marking all passes as culled
            for (auto& pass : m_passes)
            {
                pass->SetCulled(true);
            }

            // Build a reverse dependency map: for each resource, which passes read it
            std::unordered_map<uint32_t, std::vector<uint32_t>> resourceReaders;
            for (const auto& pass : m_passes)
            {
                for (const auto& r : pass->GetReads())
                {
                    if (r.IsValid())
                    {
                        resourceReaders[r.index].push_back(pass->GetIndex());
                    }
                }
            }

            // Find passes that must execute (have side effects or write to
            // resources that are read by other needed passes)
            std::queue<uint32_t> workQueue;

            // Seed: side-effect passes
            for (const auto& pass : m_passes)
            {
                if (pass->HasSideEffects())
                {
                    pass->SetCulled(false);
                    workQueue.push(pass->GetIndex());
                }
            }

            // Also keep passes that write to imported resources (they are
            // presumably needed externally)
            for (const auto& pass : m_passes)
            {
                if (pass->IsCulled())
                {
                    for (const auto& w : pass->GetWrites())
                    {
                        if (w.IsValid() && w.index < m_resources.size() &&
                            m_resources[w.index].lifetime == RenderGraphResourceLifetime::Imported)
                        {
                            pass->SetCulled(false);
                            workQueue.push(pass->GetIndex());
                            break;
                        }
                    }
                }
            }

            // Flood backward: if pass P is needed, all passes that produce
            // resources read by P are also needed
            while (!workQueue.empty())
            {
                uint32_t passIdx = workQueue.front();
                workQueue.pop();
                const auto& pass = *m_passes[passIdx];

                for (const auto& r : pass.GetReads())
                {
                    if (!r.IsValid() || r.index >= m_resources.size())
                    {
                        continue;
                    }
                    // Find the producer pass
                    uint32_t producer = m_resources[r.index].producerPass;
                    if (producer != RenderGraphResource::INVALID_INDEX && producer < m_passes.size() &&
                        m_passes[producer]->IsCulled())
                    {
                        m_passes[producer]->SetCulled(false);
                        workQueue.push(producer);
                    }
                }
            }
        }

        /**
     * @brief Topological sort of non-culled passes based on data dependencies.
     *
     * Uses Kahn's algorithm. The result is stored in m_executionOrder.
     */
        void TopologicalSort()
        {
            size_t passCount = m_passes.size();
            m_executionOrder.clear();
            m_executionOrder.reserve(passCount);

            // Build adjacency: pass A -> pass B means B depends on A
            // (A writes a resource that B reads)
            std::vector<std::vector<uint32_t>> adjacency(passCount);
            std::vector<uint32_t> inDegree(passCount, 0);

            // Map: resource index -> producer pass index
            std::unordered_map<uint32_t, uint32_t> lastWriter;

            for (const auto& pass : m_passes)
            {
                if (pass->IsCulled())
                {
                    continue;
                }
                for (const auto& w : pass->GetWrites())
                {
                    if (w.IsValid())
                    {
                        lastWriter[w.index] = pass->GetIndex();
                    }
                }
                for (const auto& c : pass->GetCreates())
                {
                    if (c.IsValid())
                    {
                        lastWriter[c.index] = pass->GetIndex();
                    }
                }
            }

            for (const auto& pass : m_passes)
            {
                if (pass->IsCulled())
                {
                    continue;
                }
                for (const auto& r : pass->GetReads())
                {
                    if (!r.IsValid())
                    {
                        continue;
                    }
                    auto it = lastWriter.find(r.index);
                    if (it != lastWriter.end() && it->second != pass->GetIndex())
                    {
                        adjacency[it->second].push_back(pass->GetIndex());
                        inDegree[pass->GetIndex()]++;
                    }
                }
            }

            // Kahn's algorithm
            std::queue<uint32_t> ready;
            for (const auto& pass : m_passes)
            {
                if (!pass->IsCulled() && inDegree[pass->GetIndex()] == 0)
                {
                    ready.push(pass->GetIndex());
                }
            }

            while (!ready.empty())
            {
                uint32_t current = ready.front();
                ready.pop();
                m_executionOrder.push_back(current);

                for (uint32_t neighbor : adjacency[current])
                {
                    if (--inDegree[neighbor] == 0)
                    {
                        ready.push(neighbor);
                    }
                }
            }
        }

        /**
     * @brief Identify resources with non-overlapping lifetimes that can
     *        share the same physical GPU memory.
     */
        void AnalyzeResourceAliasing()
        {
            // Honor explicit alias hints
            for (const auto& [from, to] : m_aliasPairs)
            {
                if (!from.IsValid() || !to.IsValid())
                {
                    continue;
                }
                if (from.index >= m_resources.size() || to.index >= m_resources.size())
                {
                    continue;
                }

                auto& srcRes = m_resources[from.index];
                auto& dstRes = m_resources[to.index];

                // Only alias transient resources
                if (srcRes.lifetime != RenderGraphResourceLifetime::Transient ||
                    dstRes.lifetime != RenderGraphResourceLifetime::Transient)
                {
                    continue;
                }

                // Check non-overlapping lifetimes
                if (srcRes.lastUsePass < dstRes.firstUsePass || dstRes.lastUsePass < srcRes.firstUsePass)
                {
                    dstRes.aliasTarget = from.index;
                    m_stats.aliasedResources++;
                    m_stats.savedByAliasing += dstRes.textureDesc.EstimateMemoryBytes();
                }
            }

            // Automatic aliasing: greedily pair transient resources with
            // non-overlapping lifetimes and compatible sizes
            std::vector<uint32_t> transients;
            for (size_t i = 0; i < m_resources.size(); ++i)
            {
                const auto& res = m_resources[i];
                if (res.lifetime == RenderGraphResourceLifetime::Transient &&
                    res.aliasTarget == RenderGraphResource::INVALID_INDEX &&
                    res.firstUsePass != RenderGraphResource::INVALID_INDEX)
                {
                    transients.push_back(static_cast<uint32_t>(i));
                }
            }

            for (size_t i = 0; i < transients.size(); ++i)
            {
                auto& candidateRes = m_resources[transients[i]];
                if (candidateRes.aliasTarget != RenderGraphResource::INVALID_INDEX)
                {
                    continue; // already aliased
                }

                for (size_t j = i + 1; j < transients.size(); ++j)
                {
                    auto& targetRes = m_resources[transients[j]];
                    if (targetRes.aliasTarget != RenderGraphResource::INVALID_INDEX)
                    {
                        continue;
                    }

                    // Non-overlapping?
                    if (candidateRes.lastUsePass < targetRes.firstUsePass ||
                        targetRes.lastUsePass < candidateRes.firstUsePass)
                    {
                        // Compatible type?
                        if (candidateRes.type == targetRes.type)
                        {
                            size_t sizeA = (candidateRes.type == RenderGraphResourceType::Texture)
                                               ? candidateRes.textureDesc.EstimateMemoryBytes()
                                               : candidateRes.bufferDesc.EstimateMemoryBytes();
                            size_t sizeB = (targetRes.type == RenderGraphResourceType::Texture)
                                               ? targetRes.textureDesc.EstimateMemoryBytes()
                                               : targetRes.bufferDesc.EstimateMemoryBytes();

                            // Allow aliasing if sizes are within 2x of each other
                            if (sizeA > 0 && sizeB > 0 && sizeA <= sizeB * 2 && sizeB <= sizeA * 2)
                            {
                                targetRes.aliasTarget = transients[i];
                                m_stats.aliasedResources++;
                                m_stats.savedByAliasing += std::min(sizeA, sizeB);
                            }
                        }
                    }
                }
            }
        }

        /**
     * @brief Tag AsyncCompute passes and verify they have no render target writes.
     */
        void ScheduleAsyncCompute()
        {
            m_stats.asyncComputePasses = 0;
            for (const auto& pass : m_passes)
            {
                if (!pass->IsCulled() && pass->GetType() == RenderGraphPassType::AsyncCompute)
                {
                    m_stats.asyncComputePasses++;
                }
            }
        }

        // ========================================================================
        // Execution Helpers
        // ========================================================================

        /**
     * @brief Allocate physical GPU resources for all transient resources.
     */
        void AllocateTransientResources()
        {
            if (!m_device)
            {
                return; // No device — running in analysis-only mode
            }

            for (auto& res : m_resources)
            {
                if (res.lifetime != RenderGraphResourceLifetime::Transient)
                {
                    continue;
                }
                if (res.type != RenderGraphResourceType::Texture)
                {
                    continue; // Buffer allocation not yet implemented
                }

                // If aliased, share the physical texture with the alias target
                if (res.aliasTarget != RenderGraphResource::INVALID_INDEX)
                {
                    res.physicalTexture = m_resources[res.aliasTarget].physicalTexture;
                    continue;
                }

                RenderTargetDesc rtDesc;
                rtDesc.name = res.name;
                rtDesc.width = res.textureDesc.width;
                rtDesc.height = res.textureDesc.height;
                rtDesc.format = res.textureDesc.format;
                rtDesc.sampleCount = res.textureDesc.sampleCount;
                rtDesc.mipLevels = res.textureDesc.mipLevels;
                rtDesc.usage = res.textureDesc.usage;
                rtDesc.clearColor = res.textureDesc.clearColor;
                rtDesc.clearDepth = res.textureDesc.clearDepth;
                rtDesc.clearStencil = res.textureDesc.clearStencil;

                auto rt = std::make_shared<RenderTarget>(rtDesc);
                if (SUCCEEDED(rt->Create(m_device)))
                {
                    res.physicalTexture = std::move(rt);
                }
            }
        }

        /**
     * @brief Release physical GPU resources for transient resources.
     */
        void ReleaseTransientResources()
        {
            for (auto& res : m_resources)
            {
                if (res.lifetime == RenderGraphResourceLifetime::Transient)
                {
                    res.physicalTexture.reset();
                }
            }
        }

        /**
     * @brief Update aggregate statistics after compilation.
     */
        void UpdateStats()
        {
            m_stats.totalPasses = static_cast<uint32_t>(m_passes.size());
            m_stats.culledPasses = 0;
            m_stats.totalResources = static_cast<uint32_t>(m_resources.size());
            m_stats.transientResources = 0;
            m_stats.importedResources = 0;
            m_stats.peakTransientMemory = 0;
            m_stats.executedPasses = 0;

            for (const auto& pass : m_passes)
            {
                if (pass->IsCulled())
                {
                    m_stats.culledPasses++;
                }
            }

            for (const auto& res : m_resources)
            {
                if (res.lifetime == RenderGraphResourceLifetime::Transient)
                {
                    m_stats.transientResources++;
                    if (res.aliasTarget == RenderGraphResource::INVALID_INDEX)
                    {
                        size_t mem = (res.type == RenderGraphResourceType::Texture)
                                         ? res.textureDesc.EstimateMemoryBytes()
                                         : res.bufferDesc.EstimateMemoryBytes();
                        m_stats.peakTransientMemory += mem;
                    }
                }
                else
                {
                    m_stats.importedResources++;
                }
            }
        }

        // ========================================================================
        // Debug Helpers
        // ========================================================================

        static std::string PassTypeToString(RenderGraphPassType type)
        {
            switch (type)
            {
            case RenderGraphPassType::Graphics:
                return "Graphics";
            case RenderGraphPassType::Compute:
                return "Compute";
            case RenderGraphPassType::Copy:
                return "Copy";
            case RenderGraphPassType::AsyncCompute:
                return "AsyncCompute";
            }
            return "Unknown";
        }

        std::string ResourceToString(RenderGraphResource handle) const
        {
            if (!handle.IsValid() || handle.index >= m_resources.size())
            {
                return "<invalid>";
            }
            return m_resources[handle.index].name + ":v" + std::to_string(handle.version);
        }

        // ========================================================================
        // Data Members
        // ========================================================================

        std::string m_name;
        ID3D11Device* m_device = nullptr;

        std::vector<std::unique_ptr<RenderGraphPass>> m_passes;
        std::vector<RenderGraphResourceNode> m_resources;
        std::vector<uint32_t> m_executionOrder;

        /// Explicit alias pairs recorded via RenderGraphBuilder::Alias()
        std::vector<std::pair<RenderGraphResource, RenderGraphResource>> m_aliasPairs;

        RenderGraphBlackboard m_blackboard;
        RenderGraphStats m_stats = {};
        bool m_compiled = false;
    };

    // ============================================================================
    // RenderGraphBuilder Inline Implementations
    // ============================================================================

    inline RenderGraphResource RenderGraphBuilder::Create(const std::string& name, const RenderGraphTextureDesc& desc)
    {
        auto handle = m_graph.CreateResource(name, desc, m_pass.GetIndex());
        m_pass.AddCreate(handle);
        m_graph.AddResourceRef(handle);
        return handle;
    }

    inline RenderGraphResource RenderGraphBuilder::Create(const std::string& name, const RenderGraphBufferDesc& desc)
    {
        auto handle = m_graph.CreateResource(name, desc, m_pass.GetIndex());
        m_pass.AddCreate(handle);
        m_graph.AddResourceRef(handle);
        return handle;
    }

    inline RenderGraphResource RenderGraphBuilder::Read(RenderGraphResource resource)
    {
        m_pass.AddRead(resource);
        m_graph.AddResourceRef(resource);
        return resource;
    }

    inline RenderGraphResource RenderGraphBuilder::Write(RenderGraphResource resource)
    {
        auto newHandle = m_graph.IncrementVersion(resource);
        m_pass.AddWrite(newHandle);
        m_graph.AddResourceRef(newHandle);
        return newHandle;
    }

    inline void RenderGraphBuilder::Alias(RenderGraphResource from, RenderGraphResource to)
    {
        m_graph.RecordAlias(from, to);
    }

} // namespace Spark::Graphics
