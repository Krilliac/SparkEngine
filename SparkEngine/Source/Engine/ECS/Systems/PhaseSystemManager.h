/**
 * @file PhaseSystemManager.h
 * @brief Phase-based ECS system execution (R2.3 — Architecture Analysis)
 *
 * Extends the flat SystemManager with named execution phases for deterministic
 * ordering. Systems assigned to a Phase are grouped and executed in phase order,
 * regardless of the order they were added.
 *
 * ## Execution order
 * ```
 * PrePhysics → Physics → PostPhysics → Animation → AI →
 * Audio → Gameplay → PreRender → Render → PostRender
 * ```
 *
 * ## Migration
 * Replace `SystemManager` with `PhaseSystemManager` and use the Phase overload:
 * @code
 *   PhaseSystemManager mgr;
 *   mgr.AddSystem<PhysicsUpdateSystem>(Phase::Physics, physicsPtr);
 *   mgr.AddSystem<RenderSystem>(Phase::Render, graphicsPtr);
 *   mgr.AddSystem<AIUpdateSystem>(Phase::AI);
 *   mgr.UpdateAll(world, deltaTime);
 * @endcode
 */

#pragma once

#include "ECSystems.h"
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace Spark::ECS
{

    /**
     * @brief Named execution phases for deterministic system ordering.
     */
    enum class Phase
    {
        PrePhysics,  ///< Input handling, scripted kinematic targets
        Physics,     ///< PhysicsUpdateSystem (fixed-timestep)
        PostPhysics, ///< Collision response, constraint solving
        Animation,   ///< Skeletal animation, IK, root motion
        AI,          ///< Perception, behavior trees, pathfinding
        Audio,       ///< 3D audio source positioning
        Gameplay,    ///< Lifecycle, health, game logic
        PreRender,   ///< Visibility, LOD, culling preparation
        Render,      ///< Draw call submission
        PostRender,  ///< Debug overlays, UI, profiling

        _Count ///< Internal: number of phases
    };

    /**
     * @brief Get a human-readable name for a Phase value.
     */
    inline const char* PhaseName(Phase phase)
    {
        static constexpr const char* names[] = {"PrePhysics", "Physics",  "PostPhysics", "Animation", "AI",
                                                "Audio",      "Gameplay", "PreRender",   "Render",    "PostRender"};
        auto idx = static_cast<size_t>(phase);
        return idx < static_cast<size_t>(Phase::_Count) ? names[idx] : "Unknown";
    }

    /**
     * @brief Phase-aware system manager (R2.3).
     *
     * Extends SystemManager by organizing systems into named execution phases.
     * Also supports the legacy flat-list API for backward compatibility.
     */
    class PhaseSystemManager
    {
      public:
        PhaseSystemManager() = default;
        ~PhaseSystemManager() = default;
        PhaseSystemManager(const PhaseSystemManager&) = delete;
        PhaseSystemManager& operator=(const PhaseSystemManager&) = delete;
        PhaseSystemManager(PhaseSystemManager&&) noexcept = default;
        PhaseSystemManager& operator=(PhaseSystemManager&&) noexcept = default;

        /**
         * @brief Construct and register a system in a specific execution phase.
         *
         * Systems in a phase execute in insertion order within that phase.
         * Phases execute in Phase enum order.
         */
        template <typename T, typename... Args> T* AddSystem(Phase phase, Args&&... args)
        {
            auto system = std::make_unique<T>(std::forward<Args>(args)...);
            T* ptr = system.get();
            m_phasedSystems[static_cast<size_t>(phase)].push_back(std::move(system));
            return ptr;
        }

        /**
         * @brief Legacy: register a system in the flat (unphased) list.
         *
         * These execute after all phased systems, in insertion order.
         */
        template <typename T, typename... Args> T* AddSystem(Args&&... args)
        {
            auto system = std::make_unique<T>(std::forward<Args>(args)...);
            T* ptr = system.get();
            m_flatSystems.push_back(std::move(system));
            return ptr;
        }

        /**
         * @brief Update all enabled systems: phased first (in phase order), then flat list.
         */
        void UpdateAll(World& world, float deltaTime)
        {
            for (size_t phase = 0; phase < static_cast<size_t>(Phase::_Count); ++phase)
            {
                for (auto& system : m_phasedSystems[phase])
                {
                    if (system->IsEnabled())
                        system->Update(world, deltaTime);
                }
            }

            for (auto& system : m_flatSystems)
            {
                if (system->IsEnabled())
                    system->Update(world, deltaTime);
            }
        }

        /**
         * @brief Find a system by its name (searches both phased and flat lists).
         */
        ISystem* GetSystem(std::string_view name)
        {
            for (size_t phase = 0; phase < static_cast<size_t>(Phase::_Count); ++phase)
            {
                for (auto& system : m_phasedSystems[phase])
                {
                    if (name == system->GetName())
                        return system.get();
                }
            }
            for (auto& system : m_flatSystems)
            {
                if (name == system->GetName())
                    return system.get();
            }
            return nullptr;
        }

        const ISystem* GetSystem(std::string_view name) const
        {
            for (size_t phase = 0; phase < static_cast<size_t>(Phase::_Count); ++phase)
            {
                for (auto& system : m_phasedSystems[phase])
                {
                    if (name == system->GetName())
                        return system.get();
                }
            }
            for (const auto& system : m_flatSystems)
            {
                if (name == system->GetName())
                    return system.get();
            }
            return nullptr;
        }

        /**
         * @brief Return the total number of registered systems (phased + flat).
         */
        size_t GetSystemCount() const
        {
            size_t count = m_flatSystems.size();
            for (size_t phase = 0; phase < static_cast<size_t>(Phase::_Count); ++phase)
                count += m_phasedSystems[phase].size();
            return count;
        }

        /**
         * @brief List all registered systems, grouped by phase (console integration).
         */
        std::string Console_ListSystems() const
        {
            std::string result;
            for (size_t phase = 0; phase < static_cast<size_t>(Phase::_Count); ++phase)
            {
                if (m_phasedSystems[phase].empty())
                    continue;
                result += "--- ";
                result += PhaseName(static_cast<Phase>(phase));
                result += " ---\n";
                for (const auto& sys : m_phasedSystems[phase])
                {
                    result += "  ";
                    result += sys->GetName();
                    result += ": ";
                    result += sys->IsEnabled() ? "enabled" : "disabled";
                    result += "\n";
                }
            }
            if (!m_flatSystems.empty())
            {
                result += "--- Unphased ---\n";
                for (const auto& sys : m_flatSystems)
                {
                    result += "  ";
                    result += sys->GetName();
                    result += ": ";
                    result += sys->IsEnabled() ? "enabled" : "disabled";
                    result += "\n";
                }
            }
            return result;
        }

      private:
        std::array<std::vector<std::unique_ptr<ISystem>>, static_cast<size_t>(Phase::_Count)> m_phasedSystems;
        std::vector<std::unique_ptr<ISystem>> m_flatSystems;
    };

} // namespace Spark::ECS
