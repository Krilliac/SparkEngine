/**
 * @file RenderGraphPass.h
 * @brief Render graph pass and builder classes
 * @author Spark Engine Team
 * @date 2026
 *
 * Contains the RenderGraphPass (a single node in the pass DAG) and
 * RenderGraphBuilder (DSL for declaring resource dependencies during
 * pass setup).
 *
 * @see RenderGraphTypes.h, RenderGraph.h
 */

#pragma once

#include "RenderGraphTypes.h"

#include <functional>
#include <string>
#include <vector>

namespace Spark::Graphics
{

    // Forward declarations
    class RenderGraph;
    class RenderGraphBuilder;

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

} // namespace Spark::Graphics
