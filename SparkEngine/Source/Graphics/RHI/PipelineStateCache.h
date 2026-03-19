/**
 * @file PipelineStateCache.h
 * @brief Immutable flyweight cache for pipeline state objects
 *
 * Inspired by Panda3D's immutable flyweight TransformState/RenderState pattern.
 * Identical PSO descriptions share a single pointer, enabling fast pointer
 * comparison during render sorting instead of deep equality checks.
 */

#pragma once

#include "../../Core/Platform.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace Spark::Graphics
{

    /// @brief Pipeline state object description
    struct PipelineStateDesc
    {
        uint64_t shaderHash = 0;        ///< Combined vertex+pixel shader hash
        uint32_t blendState = 0;        ///< Encoded blend state
        uint32_t depthStencilState = 0; ///< Encoded depth/stencil state
        uint32_t rasterizerState = 0;   ///< Encoded rasterizer state
        uint32_t vertexFormat = 0;      ///< Vertex input layout hash

        bool operator==(const PipelineStateDesc&) const = default;
    };

    /// @brief Hash functor for PipelineStateDesc
    struct PipelineStateDescHash
    {
        size_t operator()(const PipelineStateDesc& desc) const
        {
            // FNV-1a over the raw bytes
            size_t hash = 14695981039346656037ULL;
            const auto* bytes = reinterpret_cast<const uint8_t*>(&desc);
            for (size_t i = 0; i < sizeof(PipelineStateDesc); ++i)
            {
                hash ^= bytes[i];
                hash *= 1099511628211ULL;
            }
            return hash;
        }
    };

    /// @brief Flyweight cache — identical PSO descriptions share a single allocation
    class PipelineStateCache
    {
      public:
        static PipelineStateCache& GetInstance()
        {
            static PipelineStateCache instance;
            return instance;
        }

        /// @brief Initialize the cache
        bool Initialize();

        /// @brief Get or create a cached PSO description (thread-safe)
        std::shared_ptr<PipelineStateDesc> GetOrCreate(const PipelineStateDesc& desc);

        /// @brief Invalidate all PSOs using a specific shader
        void Invalidate(uint64_t shaderHash);

        /// @brief Get number of cached states
        uint32_t GetCachedStateCount() const;

        /// @brief Clear entire cache
        void Clear();

        /// @brief Console status
        std::string Console_GetStatus() const;

        /// @brief Shutdown
        void Shutdown();

      private:
        PipelineStateCache() = default;
        ~PipelineStateCache() = default;
        PipelineStateCache(const PipelineStateCache&) = delete;
        PipelineStateCache& operator=(const PipelineStateCache&) = delete;

        mutable std::mutex m_mutex;
        std::unordered_map<PipelineStateDesc, std::shared_ptr<PipelineStateDesc>, PipelineStateDescHash> m_cache;
    };

} // namespace Spark::Graphics
