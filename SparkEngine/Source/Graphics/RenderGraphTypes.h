/**
 * @file RenderGraphTypes.h
 * @brief Resource descriptors, enums, and handles for the render graph system
 * @author Spark Engine Team
 * @date 2026
 *
 * Contains the foundational types used by the render graph: resource handles,
 * texture/buffer descriptors, resource lifetime and type enums, and the
 * internal resource node bookkeeping struct.
 *
 * @see RenderGraphPass.h, RenderGraphBlackboard.h, RenderGraph.h
 */

#pragma once

#include "../Core/Platform.h"
#include "RenderTarget.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace Spark::Graphics
{

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
            size_t result = static_cast<size_t>(width);
            constexpr size_t kMaxSize = std::numeric_limits<size_t>::max();
            auto safeMul = [kMaxSize](size_t a, size_t b) -> size_t
            { return (a > 0 && b > kMaxSize / a) ? kMaxSize : a * b; };
            result = safeMul(result, height);
            result = safeMul(result, depth);
            result = safeMul(result, arraySize);
            result = safeMul(result, sampleCount);
            result = safeMul(result, bpp);
            return result;
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

        /// Producer for each logical version. Imported version zero has no
        /// graph producer; versions created by Write() identify their pass.
        std::vector<uint32_t> versionProducers;

        /// Set when more than one pass attempts to produce the same SSA version.
        bool hasVersionConflict = false;

        /// Execution-time concrete object. Populated during Execute().
        std::shared_ptr<RenderTarget> physicalTexture;

        /// Execution-time concrete buffer object. Populated during Execute() for Buffer resources.
        ComPtr<ID3D11Buffer> physicalBuffer;

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
         * @brief Resolve a resource handle to its physical buffer.
         * @param handle The resource handle obtained during pass setup.
         * @return Non-owning pointer to the D3D11 buffer, or nullptr if
         *         the resource is not backed by a buffer.
         */
        ID3D11Buffer* GetBuffer(RenderGraphResource handle) const
        {
            if (!handle.IsValid() || handle.index >= m_resources.size())
            {
                return nullptr;
            }
            return m_resources[handle.index].physicalBuffer.Get();
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

} // namespace Spark::Graphics
