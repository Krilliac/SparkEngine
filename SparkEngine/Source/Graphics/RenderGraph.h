/**
 * @file RenderGraph.h
 * @brief Declarative render graph / frame graph system for SparkEngine (umbrella header)
 * @author Spark Engine Team
 * @date 2026
 *
 * This is the umbrella header for the render graph subsystem. It includes:
 * - RenderGraphTypes.h   — Resource handles, descriptors, enums, registry, stats
 * - RenderGraphPass.h    — Pass and builder classes
 * - RenderGraphBlackboard.h — Type-erased inter-pass data sharing
 * - RenderGraph class    — Top-level graph object (defined below)
 *
 * @see GraphicsEngine, RenderTarget, RenderTargetFormat
 */

#pragma once

// Subsystem headers
#include "RenderGraphTypes.h"
#include "RenderGraphPass.h"
#include "RenderGraphBlackboard.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <functional>
#include <memory>
#include <queue>
#include <stdexcept>
#include <sstream>
#include <string>
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
            struct ResourceMutationSnapshot
            {
                uint32_t refCount;
                size_t versionProducerCount;
                bool hasVersionConflict;
            };

            const size_t originalResourceCount = m_resources.size();
            const size_t originalAliasCount = m_aliasPairs.size();
            std::vector<ResourceMutationSnapshot> originalResources;
            originalResources.reserve(originalResourceCount);
            for (const auto& resource : m_resources)
            {
                originalResources.push_back(
                    {resource.refCount, resource.versionProducers.size(), resource.hasVersionConflict});
            }

            // A setup callback is allowed to mutate graph-owned resource metadata.
            // Invalidate the prior schedule before invoking user code, then restore
            // every mutation if setup fails so the graph remains structurally sound.
            m_compiled = false;
            auto passIndex = static_cast<uint32_t>(m_passes.size());
            auto pass = std::make_unique<RenderGraphPass>(name, type, passIndex);
            pass->SetExecuteCallback(std::move(execute));

            try
            {
                RenderGraphBuilder builder(*this, *pass);
                setup(builder);
                m_passes.push_back(std::move(pass));
            }
            catch (...)
            {
                m_aliasPairs.resize(originalAliasCount);
                m_resources.resize(originalResourceCount);
                for (size_t i = 0; i < originalResourceCount; ++i)
                {
                    m_resources[i].refCount = originalResources[i].refCount;
                    m_resources[i].versionProducers.resize(originalResources[i].versionProducerCount);
                    m_resources[i].hasVersionConflict = originalResources[i].hasVersionConflict;
                }
                throw;
            }

            return *m_passes.back();
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
            node.versionProducers.push_back(RenderGraphResource::INVALID_INDEX);

            if (texture)
            {
                const auto& desc = texture->GetDesc();
                node.textureDesc.width = desc.width;
                node.textureDesc.height = desc.height;
                node.textureDesc.arraySize = desc.arraySize;
                node.textureDesc.format = desc.format;
                node.textureDesc.sampleCount = desc.sampleCount;
                node.textureDesc.mipLevels = desc.mipLevels;
                node.textureDesc.usage = desc.usage;
                node.textureDesc.clearColor = desc.clearColor;
                node.textureDesc.clearDepth = desc.clearDepth;
                node.textureDesc.clearStencil = desc.clearStencil;
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

            m_compiled = false;
            try
            {
                ValidateResourceVersions();
                CullUnreferencedPasses();
                TopologicalSort();
                ComputeResourceLifetimes();
                AnalyzeResourceAliasing();
                ScheduleAsyncCompute();

                auto compileEnd = std::chrono::high_resolution_clock::now();
                m_stats.compileTimeMs = std::chrono::duration<float, std::milli>(compileEnd - compileStart).count();
                UpdateStats();
                m_compiled = true;
            }
            catch (...)
            {
                m_executionOrder.clear();
                for (auto& resource : m_resources)
                {
                    resource.aliasTarget = RenderGraphResource::INVALID_INDEX;
                }
                m_stats.aliasedResources = 0;
                m_stats.savedByAliasing = 0;
                throw;
            }
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
     * Requires a prior successful call to Compile().
     */
        void Execute()
        {
            if (!m_compiled)
            {
                throw std::logic_error("RenderGraph::Execute() called before a successful Compile()");
            }

            auto executeStart = std::chrono::high_resolution_clock::now();
            m_stats.executedPasses = 0;

            // Clear any allocation retained by an earlier failed execution.
            try
            {
                ReleaseTransientResources();
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
            }
            catch (...)
            {
                ReleaseTransientResources();
                throw;
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
                    auto passIdx = m_executionOrder[i];
                    ss << (passIdx < m_passes.size() ? m_passes[passIdx]->GetName() : "?");
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
            node.versionProducers.push_back(producerPass);

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
            node.versionProducers.push_back(producerPass);

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
        RenderGraphResource IncrementVersion(RenderGraphResource handle, uint32_t producerPass)
        {
            RenderGraphResource newHandle = handle;
            newHandle.version++;

            if (!handle.IsValid() || handle.index >= m_resources.size())
            {
                return newHandle;
            }

            auto& resource = m_resources[handle.index];
            if (newHandle.version < resource.versionProducers.size())
            {
                resource.hasVersionConflict = true;
            }
            else
            {
                resource.versionProducers.resize(static_cast<size_t>(newHandle.version) + 1,
                                                 RenderGraphResource::INVALID_INDEX);
                resource.versionProducers[newHandle.version] = producerPass;
            }
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

        uint32_t GetVersionProducer(RenderGraphResource handle) const
        {
            if (!handle.IsValid() || handle.index >= m_resources.size())
            {
                return RenderGraphResource::INVALID_INDEX;
            }
            const auto& producers = m_resources[handle.index].versionProducers;
            return handle.version < producers.size() ? producers[handle.version] : RenderGraphResource::INVALID_INDEX;
        }

        void ValidateResourceVersions() const
        {
            for (const auto& resource : m_resources)
            {
                if (resource.hasVersionConflict)
                {
                    throw std::logic_error("RenderGraph resource version has multiple producers: " + resource.name);
                }
                for (size_t version = 1; version < resource.versionProducers.size(); ++version)
                {
                    if (resource.versionProducers[version] == RenderGraphResource::INVALID_INDEX)
                    {
                        throw std::logic_error("RenderGraph resource has a missing version producer: " + resource.name);
                    }
                }
            }

            auto validateHandle = [&](RenderGraphResource handle, const char* operation)
            {
                if (!handle.IsValid() || handle.index >= m_resources.size())
                {
                    throw std::logic_error(std::string("RenderGraph ") + operation + " uses an invalid resource");
                }
                const auto& resource = m_resources[handle.index];
                if (handle.version >= resource.versionProducers.size())
                {
                    throw std::logic_error(std::string("RenderGraph ") + operation + " uses an unknown version of " +
                                           resource.name);
                }
                if (handle.version > 0 &&
                    resource.versionProducers[handle.version] == RenderGraphResource::INVALID_INDEX)
                {
                    throw std::logic_error(std::string("RenderGraph ") + operation + " uses an unproduced version of " +
                                           resource.name);
                }
            };

            for (const auto& pass : m_passes)
            {
                for (const auto& handle : pass->GetReads())
                {
                    validateHandle(handle, "read");
                }
                for (const auto& handle : pass->GetWrites())
                {
                    validateHandle(handle, "write");
                    if (handle.version == 0 || GetVersionProducer(handle) != pass->GetIndex())
                    {
                        throw std::logic_error("RenderGraph write is not the unique producer of its version");
                    }
                }
                for (const auto& handle : pass->GetCreates())
                {
                    validateHandle(handle, "create");
                    if (handle.version != 0 || GetVersionProducer(handle) != pass->GetIndex())
                    {
                        throw std::logic_error("RenderGraph create has an invalid producer");
                    }
                }
            }
        }

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

            std::vector<uint32_t> executionPosition(m_passes.size(), RenderGraphResource::INVALID_INDEX);
            for (uint32_t position = 0; position < m_executionOrder.size(); ++position)
            {
                executionPosition[m_executionOrder[position]] = position;
            }

            for (const auto& pass : m_passes)
            {
                if (pass->IsCulled())
                {
                    continue;
                }
                uint32_t pi = executionPosition[pass->GetIndex()];

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
                    // Find the producer of the exact version consumed.
                    uint32_t producer = GetVersionProducer(r);
                    if (producer != RenderGraphResource::INVALID_INDEX && producer < m_passes.size() &&
                        m_passes[producer]->IsCulled())
                    {
                        m_passes[producer]->SetCulled(false);
                        workQueue.push(producer);
                    }
                }

                // Writes also depend on the previous version. Preserve that
                // producer even if this is a write-only pass.
                for (const auto& w : pass.GetWrites())
                {
                    if (w.version == 0)
                    {
                        continue;
                    }
                    uint32_t producer = GetVersionProducer({w.index, w.version - 1});
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

            std::vector<std::unordered_set<uint32_t>> edges(passCount);
            std::unordered_map<RenderGraphResource, std::vector<uint32_t>, RenderGraphResourceHash> versionReaders;
            for (const auto& pass : m_passes)
            {
                if (pass->IsCulled())
                {
                    continue;
                }
                for (const auto& read : pass->GetReads())
                {
                    versionReaders[read].push_back(pass->GetIndex());
                }
            }

            auto addDependency = [&](uint32_t producer, uint32_t consumer)
            {
                if (producer == RenderGraphResource::INVALID_INDEX || producer == consumer || producer >= passCount ||
                    m_passes[producer]->IsCulled())
                {
                    return;
                }
                if (edges[producer].insert(consumer).second)
                {
                    inDegree[consumer]++;
                }
            };

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
                    addDependency(GetVersionProducer(r), pass->GetIndex());
                }
                for (const auto& w : pass->GetWrites())
                {
                    if (w.version > 0)
                    {
                        const RenderGraphResource previous{w.index, w.version - 1};
                        addDependency(GetVersionProducer(previous), pass->GetIndex());
                        const auto readers = versionReaders.find(previous);
                        if (readers != versionReaders.end())
                        {
                            for (uint32_t reader : readers->second)
                            {
                                addDependency(reader, pass->GetIndex());
                            }
                        }
                    }
                }
            }

            for (uint32_t producer = 0; producer < passCount; ++producer)
            {
                adjacency[producer].assign(edges[producer].begin(), edges[producer].end());
                std::sort(adjacency[producer].begin(), adjacency[producer].end());
            }

            // Kahn's algorithm. Lowest registration index wins ties so the
            // schedule is stable across builds and standard library versions.
            std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<>> ready;
            for (const auto& pass : m_passes)
            {
                if (!pass->IsCulled() && inDegree[pass->GetIndex()] == 0)
                {
                    ready.push(pass->GetIndex());
                }
            }

            while (!ready.empty())
            {
                uint32_t current = ready.top();
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

            const auto activePasses = static_cast<size_t>(
                std::count_if(m_passes.begin(), m_passes.end(), [](const auto& pass) { return !pass->IsCulled(); }));
            if (m_executionOrder.size() != activePasses)
            {
                m_executionOrder.clear();
                throw std::logic_error("RenderGraph contains a dependency cycle");
            }
        }

        /**
     * @brief Identify resources with non-overlapping lifetimes that can
     *        share the same physical GPU memory.
     */
        void AnalyzeResourceAliasing()
        {
            m_stats.aliasedResources = 0;
            m_stats.savedByAliasing = 0;
            for (auto& resource : m_resources)
            {
                resource.aliasTarget = RenderGraphResource::INVALID_INDEX;
            }

            auto compatible = [](const RenderGraphResourceNode& a, const RenderGraphResourceNode& b)
            {
                if (a.type != b.type)
                {
                    return false;
                }
                if (a.type == RenderGraphResourceType::Buffer)
                {
                    return a.bufferDesc.sizeBytes == b.bufferDesc.sizeBytes &&
                           a.bufferDesc.stride == b.bufferDesc.stride && a.bufferDesc.usage == b.bufferDesc.usage;
                }
                const auto& x = a.textureDesc;
                const auto& y = b.textureDesc;
                return x.width == y.width && x.height == y.height && x.depth == y.depth && x.arraySize == y.arraySize &&
                       x.mipLevels == y.mipLevels && x.sampleCount == y.sampleCount && x.format == y.format &&
                       x.usage == y.usage;
            };
            auto overlaps = [](const RenderGraphResourceNode& a, const RenderGraphResourceNode& b)
            { return !(a.lastUsePass < b.firstUsePass || b.lastUsePass < a.firstUsePass); };
            auto resourceSize = [](const RenderGraphResourceNode& resource)
            {
                return resource.type == RenderGraphResourceType::Texture ? resource.textureDesc.EstimateMemoryBytes()
                                                                         : resource.bufferDesc.EstimateMemoryBytes();
            };

            // Honor explicit alias hints, but reject unsafe hints instead of
            // silently constructing an overlapping or incompatible allocation.
            for (const auto& [from, to] : m_aliasPairs)
            {
                if (!from.IsValid() || !to.IsValid() || from.index >= m_resources.size() ||
                    to.index >= m_resources.size() || from.index == to.index)
                {
                    throw std::logic_error("RenderGraph has an invalid explicit alias");
                }
                if (from.version >= m_resources[from.index].versionProducers.size() ||
                    to.version >= m_resources[to.index].versionProducers.size())
                {
                    throw std::logic_error("RenderGraph alias references an unknown resource version");
                }

                uint32_t root = from.index;
                for (size_t hop = 0; m_resources[root].aliasTarget != RenderGraphResource::INVALID_INDEX; ++hop)
                {
                    if (hop >= m_resources.size())
                    {
                        throw std::logic_error("RenderGraph explicit aliases contain a cycle");
                    }
                    root = m_resources[root].aliasTarget;
                }

                auto& srcRes = m_resources[root];
                auto& dstRes = m_resources[to.index];
                const bool destinationOwnsAliases =
                    std::any_of(m_resources.begin(), m_resources.end(),
                                [&](const auto& resource) { return resource.aliasTarget == to.index; });
                if (srcRes.lifetime != RenderGraphResourceLifetime::Transient ||
                    dstRes.lifetime != RenderGraphResourceLifetime::Transient ||
                    srcRes.firstUsePass == RenderGraphResource::INVALID_INDEX ||
                    dstRes.firstUsePass == RenderGraphResource::INVALID_INDEX || !compatible(srcRes, dstRes) ||
                    dstRes.aliasTarget != RenderGraphResource::INVALID_INDEX || destinationOwnsAliases)
                {
                    throw std::logic_error("RenderGraph explicit alias resources are incompatible");
                }

                for (size_t i = 0; i < m_resources.size(); ++i)
                {
                    if ((i == root || m_resources[i].aliasTarget == root) && overlaps(m_resources[i], dstRes))
                    {
                        throw std::logic_error("RenderGraph explicit alias lifetimes overlap");
                    }
                }
                dstRes.aliasTarget = root;
            }

            // Build allocation groups and only add a resource when it is
            // compatible and disjoint from every existing group member.
            std::vector<uint32_t> transients;
            for (size_t i = 0; i < m_resources.size(); ++i)
            {
                const auto& res = m_resources[i];
                if (res.lifetime == RenderGraphResourceLifetime::Transient &&
                    res.firstUsePass != RenderGraphResource::INVALID_INDEX)
                {
                    transients.push_back(static_cast<uint32_t>(i));
                }
            }

            std::sort(transients.begin(), transients.end(),
                      [&](uint32_t a, uint32_t b)
                      {
                          if (m_resources[a].firstUsePass != m_resources[b].firstUsePass)
                          {
                              return m_resources[a].firstUsePass < m_resources[b].firstUsePass;
                          }
                          return a < b;
                      });

            std::vector<std::vector<uint32_t>> groups;
            for (uint32_t root : transients)
            {
                if (m_resources[root].aliasTarget != RenderGraphResource::INVALID_INDEX)
                {
                    continue;
                }
                std::vector<uint32_t> explicitGroup{root};
                for (uint32_t member : transients)
                {
                    if (m_resources[member].aliasTarget == root)
                    {
                        explicitGroup.push_back(member);
                    }
                }
                if (explicitGroup.size() > 1)
                {
                    groups.push_back(std::move(explicitGroup));
                }
            }

            for (uint32_t resourceIndex : transients)
            {
                auto& resource = m_resources[resourceIndex];
                if (resource.aliasTarget != RenderGraphResource::INVALID_INDEX)
                {
                    continue;
                }

                // Explicit roots already owning members stay roots. Avoid
                // creating alias chains that depend on allocation order.
                auto ownGroup = std::find_if(groups.begin(), groups.end(), [&](const auto& members)
                                             { return !members.empty() && members.front() == resourceIndex; });
                if (ownGroup != groups.end())
                {
                    continue;
                }

                auto destination =
                    std::find_if(groups.begin(), groups.end(),
                                 [&](const auto& members)
                                 {
                                     if (members.empty() || !compatible(m_resources[members.front()], resource))
                                     {
                                         return false;
                                     }
                                     return std::none_of(members.begin(), members.end(), [&](uint32_t member)
                                                         { return overlaps(m_resources[member], resource); });
                                 });
                if (destination != groups.end())
                {
                    resource.aliasTarget = destination->front();
                    destination->push_back(resourceIndex);
                }
                else
                {
                    groups.push_back({resourceIndex});
                }
            }

            for (const auto& resource : m_resources)
            {
                if (resource.aliasTarget != RenderGraphResource::INVALID_INDEX)
                {
                    m_stats.aliasedResources++;
                    m_stats.savedByAliasing += resourceSize(resource);
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
                if (res.aliasTarget != RenderGraphResource::INVALID_INDEX)
                {
                    continue; // Allocate roots first; aliases are bound below.
                }
                if (res.type == RenderGraphResourceType::Buffer)
                {
#ifdef SPARK_PLATFORM_WINDOWS
                    // Allocate transient buffer via D3D11
                    if (res.bufferDesc.sizeBytes == 0)
                    {
                        continue;
                    }

                    D3D11_BUFFER_DESC bufDesc{};
                    bufDesc.ByteWidth = static_cast<UINT>(res.bufferDesc.sizeBytes);
                    bufDesc.StructureByteStride = static_cast<UINT>(res.bufferDesc.stride);

                    // Map RenderTargetUsage bitmask to D3D11 bind flags
                    UINT bindFlags = 0;
                    auto usageBits = std::to_underlying(res.bufferDesc.usage);
                    if (usageBits & std::to_underlying(RenderTargetUsage::ShaderResource))
                    {
                        bindFlags |= D3D11_BIND_SHADER_RESOURCE;
                    }
                    if (usageBits & std::to_underlying(RenderTargetUsage::UnorderedAccess))
                    {
                        bindFlags |= D3D11_BIND_UNORDERED_ACCESS;
                    }
                    if (bindFlags == 0)
                    {
                        bindFlags = D3D11_BIND_SHADER_RESOURCE;
                    }
                    bufDesc.BindFlags = bindFlags;

                    if (res.bufferDesc.stride > 0)
                    {
                        bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
                    }

                    bufDesc.Usage = D3D11_USAGE_DEFAULT;

                    m_device->CreateBuffer(&bufDesc, nullptr, res.physicalBuffer.ReleaseAndGetAddressOf());
#endif // SPARK_PLATFORM_WINDOWS
                    continue;
                }

                if (res.type != RenderGraphResourceType::Texture)
                {
                    continue;
                }

                ::RenderTargetDesc rtDesc;
                rtDesc.name = res.name;
                rtDesc.width = res.textureDesc.width;
                rtDesc.height = res.textureDesc.height;
                rtDesc.arraySize = res.textureDesc.arraySize;
                rtDesc.format = res.textureDesc.format;
                rtDesc.sampleCount = res.textureDesc.sampleCount;
                rtDesc.mipLevels = res.textureDesc.mipLevels;
                rtDesc.usage = res.textureDesc.usage;
                rtDesc.clearColor = res.textureDesc.clearColor;
                rtDesc.clearDepth = res.textureDesc.clearDepth;
                rtDesc.clearStencil = res.textureDesc.clearStencil;

                auto rt = std::make_shared<::RenderTarget>(rtDesc);
                if (SUCCEEDED(rt->Create(m_device)))
                {
                    res.physicalTexture = std::move(rt);
                }
            }

            for (auto& res : m_resources)
            {
                if (res.lifetime != RenderGraphResourceLifetime::Transient ||
                    res.aliasTarget == RenderGraphResource::INVALID_INDEX)
                {
                    continue;
                }
                const auto& root = m_resources[res.aliasTarget];
                if (res.type == RenderGraphResourceType::Texture)
                {
                    res.physicalTexture = root.physicalTexture;
                }
                else
                {
                    res.physicalBuffer = root.physicalBuffer;
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
                    res.physicalBuffer.Reset();
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
        auto newHandle = m_graph.IncrementVersion(resource, m_pass.GetIndex());
        m_pass.AddWrite(newHandle);
        m_graph.AddResourceRef(newHandle);
        return newHandle;
    }

    inline void RenderGraphBuilder::Alias(RenderGraphResource from, RenderGraphResource to)
    {
        m_graph.RecordAlias(from, to);
    }

} // namespace Spark::Graphics
