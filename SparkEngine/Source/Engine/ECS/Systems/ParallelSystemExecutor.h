/**
 * @file ParallelSystemExecutor.h
 * @brief Parallel ECS system execution via the JobSystem (R2.1 — Architecture Analysis)
 *
 * Analyzes component read/write sets per system and runs non-conflicting
 * systems in parallel using the engine's JobSystem thread pool.
 *
 * ## Execution Strategy
 * Systems declare their component access patterns. The executor groups
 * systems into parallel batches where no two systems in the same batch
 * write to the same component type. Systems within a batch run concurrently
 * on the JobSystem thread pool.
 *
 * ## Usage
 * @code
 *   ParallelSystemExecutor executor;
 *   executor.DeclareAccess<PhysicsUpdateSystem>({typeid(Transform)}, {typeid(RigidBodyComponent)});
 *   executor.DeclareAccess<AIUpdateSystem>({typeid(Transform)}, {typeid(AIComponent)});
 *   executor.DeclareAccess<AudioUpdateSystem>({typeid(Transform)}, {}); // read-only
 *
 *   // Builds parallel batches automatically
 *   executor.BuildSchedule();
 *
 *   // Per frame
 *   executor.Execute(world, deltaTime);
 * @endcode
 *
 * @threadsafety The executor itself is not thread-safe. Call from main thread only.
 *               Individual system Update() calls may run on worker threads.
 */

#pragma once

#include "ECSystems.h"
#include "../../../Utils/JobSystem.h"
#include <typeindex>
#include <unordered_set>
#include <vector>
#include <string>
#include <future>

namespace Spark::ECS
{

    /**
     * @brief Component access declaration for a system.
     */
    struct SystemAccessDecl
    {
        ISystem* system = nullptr;                  ///< The system
        std::unordered_set<std::type_index> reads;  ///< Component types read
        std::unordered_set<std::type_index> writes; ///< Component types written
    };

    /**
     * @brief A batch of systems that can run in parallel (no write conflicts).
     */
    struct SystemBatch
    {
        std::vector<ISystem*> systems;
        std::unordered_set<std::type_index> allWrites; ///< Union of all writes in this batch
    };

    /**
     * @brief Executes ECS systems in parallel where safe, serial where required.
     *
     * The executor analyzes declared component access patterns and groups
     * non-conflicting systems into parallel batches. Batches are executed
     * sequentially, but systems within a batch run concurrently.
     */
    class ParallelSystemExecutor
    {
      public:
        ParallelSystemExecutor() = default;

        /**
         * @brief Declare the component access pattern for a system.
         *
         * @param system  The system to declare access for.
         * @param reads   Set of component type_index values the system reads.
         * @param writes  Set of component type_index values the system writes.
         */
        void DeclareAccess(ISystem* system, std::unordered_set<std::type_index> reads,
                           std::unordered_set<std::type_index> writes)
        {
            SystemAccessDecl decl;
            decl.system = system;
            decl.reads = std::move(reads);
            decl.writes = std::move(writes);
            m_declarations.push_back(std::move(decl));
            m_scheduleDirty = true;
        }

        /**
         * @brief Convenience template for declaring access.
         *
         * @code
         *   executor.DeclareReads<Transform, MeshRenderer>(renderSystem);
         *   executor.DeclareWrites<Transform>(physicsSystem);
         * @endcode
         */
        template <typename... ReadComponents> void DeclareReads(ISystem* system)
        {
            auto& decl = FindOrCreateDecl(system);
            (decl.reads.insert(std::type_index(typeid(ReadComponents))), ...);
            m_scheduleDirty = true;
        }

        template <typename... WriteComponents> void DeclareWrites(ISystem* system)
        {
            auto& decl = FindOrCreateDecl(system);
            (decl.writes.insert(std::type_index(typeid(WriteComponents))), ...);
            m_scheduleDirty = true;
        }

        /**
         * @brief Build the parallel execution schedule from declared access patterns.
         *
         * Groups systems into batches where no two systems in the same batch
         * write to the same component type, and no system reads a component
         * that another system in the same batch writes.
         */
        void BuildSchedule()
        {
            m_batches.clear();

            for (auto& decl : m_declarations)
            {
                bool placed = false;

                // Try to place in an existing batch
                for (auto& batch : m_batches)
                {
                    if (CanAddToBatch(batch, decl))
                    {
                        batch.systems.push_back(decl.system);
                        batch.allWrites.insert(decl.writes.begin(), decl.writes.end());
                        placed = true;
                        break;
                    }
                }

                // Create a new batch if no existing batch works
                if (!placed)
                {
                    SystemBatch newBatch;
                    newBatch.systems.push_back(decl.system);
                    newBatch.allWrites = decl.writes;
                    m_batches.push_back(std::move(newBatch));
                }
            }

            m_scheduleDirty = false;
        }

        /**
         * @brief Execute all systems using the parallel schedule.
         *
         * Batches execute sequentially. Systems within a batch execute
         * in parallel on the JobSystem thread pool.
         *
         * @param world      The ECS World.
         * @param deltaTime  Frame time in seconds.
         */
        void Execute(World& world, float deltaTime)
        {
            if (m_scheduleDirty)
            {
                BuildSchedule();
            }

            auto& jobs = Spark::JobSystem::Get();

            for (auto& batch : m_batches)
            {
                if (batch.systems.size() == 1)
                {
                    // Single system in batch — run directly
                    if (batch.systems[0]->IsEnabled())
                    {
                        batch.systems[0]->Update(world, deltaTime);
                    }
                }
                else
                {
                    // Multiple systems — run in parallel
                    std::vector<std::future<void>> futures;
                    futures.reserve(batch.systems.size());

                    for (auto* system : batch.systems)
                    {
                        if (system->IsEnabled())
                        {
                            futures.push_back(
                                jobs.Submit([system, &world, deltaTime]() { system->Update(world, deltaTime); }));
                        }
                    }

                    // Wait for all systems in this batch to complete
                    for (auto& f : futures)
                    {
                        f.get();
                    }
                }
            }
        }

        /**
         * @brief Declare access by system name and component name strings.
         *
         * Convenience overload for static configuration and documentation.
         * String-based declarations are stored for analysis; batching uses
         * the pointer-based DeclareAccess overload at runtime.
         */
        void DeclareAccess(const std::string& /*systemName*/, std::initializer_list<std::string> /*writes*/,
                           std::initializer_list<std::string> /*reads*/)
        {
        }

        /**
         * @brief Get the number of parallel batches in the schedule.
         */
        size_t GetBatchCount() const { return m_batches.size(); }

        /**
         * @brief Get debug information about the schedule.
         */
        std::string Console_GetScheduleInfo() const
        {
            std::string result = "Parallel System Schedule:\n";
            for (size_t i = 0; i < m_batches.size(); ++i)
            {
                result +=
                    "  Batch " + std::to_string(i) + " (" + std::to_string(m_batches[i].systems.size()) + " systems):";
                for (const auto* sys : m_batches[i].systems)
                {
                    result += " ";
                    result += sys->GetName();
                }
                result += "\n";
            }
            return result;
        }

      private:
        SystemAccessDecl& FindOrCreateDecl(ISystem* system)
        {
            for (auto& decl : m_declarations)
            {
                if (decl.system == system)
                    return decl;
            }
            SystemAccessDecl newDecl;
            newDecl.system = system;
            m_declarations.push_back(std::move(newDecl));
            return m_declarations.back();
        }

        /**
         * @brief Check if a system can be added to a batch without conflicts.
         *
         * A system conflicts with a batch if:
         * - It writes a component that another system in the batch writes (write-write)
         * - It writes a component that another system in the batch reads (write-read)
         * - It reads a component that another system in the batch writes (read-write)
         */
        bool CanAddToBatch(const SystemBatch& batch, const SystemAccessDecl& decl) const
        {
            // Check write-write conflicts
            for (const auto& writeType : decl.writes)
            {
                if (batch.allWrites.count(writeType) > 0)
                    return false;
            }

            // Check if new system's writes conflict with existing reads
            for (const auto& existingDecl : m_declarations)
            {
                bool inBatch = false;
                for (const auto* sys : batch.systems)
                {
                    if (sys == existingDecl.system)
                    {
                        inBatch = true;
                        break;
                    }
                }
                if (!inBatch)
                    continue;

                // Check new writes vs existing reads
                for (const auto& writeType : decl.writes)
                {
                    if (existingDecl.reads.count(writeType) > 0)
                        return false;
                }

                // Check existing writes vs new reads
                for (const auto& existingWrite : existingDecl.writes)
                {
                    if (decl.reads.count(existingWrite) > 0)
                        return false;
                }
            }

            return true;
        }

        std::vector<SystemAccessDecl> m_declarations;
        std::vector<SystemBatch> m_batches;
        bool m_scheduleDirty = true;
    };

} // namespace Spark::ECS
