/**
 * @file ReactiveSystem.h
 * @brief EnTT-based reactive ECS system using observers (R2.4 — Architecture Analysis)
 *
 * Provides a base class for systems that only process entities whose components
 * have changed, rather than iterating all matching entities every frame. Uses
 * EnTT's on_construct, on_update, and on_destroy signals to track changes.
 *
 * ## When to use
 * Use ReactiveSystem for components that change rarely compared to how often
 * they're checked. Examples:
 * - MeshRenderer material changes → only re-upload material constants
 * - LightComponent parameter changes → only rebuild shadow maps for affected lights
 * - Transform parent changes → only rebuild hierarchy
 *
 * ## When NOT to use
 * Do not use for components that change every frame (Transform positions,
 * animation bones). The overhead of change tracking outweighs the savings.
 *
 * ## Usage
 * @code
 *   class MaterialChangeSystem : public ReactiveSystem<MeshRenderer> {
 *   public:
 *       void OnComponentChanged(World& world, EntityID entity,
 *                               MeshRenderer& comp, ChangeType type) override {
 *           if (type == ChangeType::Updated) {
 *               ReUploadMaterialConstants(comp);
 *           }
 *       }
 *       const char* GetName() const override { return "MaterialChangeSystem"; }
 *   };
 * @endcode
 */

#pragma once

#include "Components.h"
#include "Systems/ECSystems.h"
#include <entt/entt.hpp>
#include <vector>

namespace Spark::ECS
{

    /**
     * @brief Type of component change that triggered the reactive system.
     */
    enum class ChangeType
    {
        Constructed, ///< Component was just added to an entity
        Updated,     ///< Component was modified (requires patching)
        Destroyed    ///< Component is about to be removed from an entity
    };

    /**
     * @brief Base class for reactive ECS systems that respond to component changes.
     *
     * @tparam Component  The component type to observe for changes.
     */
    template <typename Component> class ReactiveSystem : public ISystem
    {
      public:
        virtual ~ReactiveSystem() { Disconnect(); }

        /**
         * @brief Connect to a registry to start observing component changes.
         *
         * Must be called once during initialization, before the first Update().
         *
         * @param registry  The EnTT registry to observe.
         */
        void Connect(entt::registry& registry)
        {
            m_registry = &registry;

            // Connect to EnTT signals
            registry.on_construct<Component>().template connect<&ReactiveSystem::OnConstruct>(this);
            registry.on_update<Component>().template connect<&ReactiveSystem::OnUpdate>(this);
            registry.on_destroy<Component>().template connect<&ReactiveSystem::OnDestroy>(this);
        }

        /**
         * @brief Disconnect from the registry and stop observing.
         */
        void Disconnect()
        {
            if (m_registry)
            {
                m_registry->on_construct<Component>().template disconnect<&ReactiveSystem::OnConstruct>(this);
                m_registry->on_update<Component>().template disconnect<&ReactiveSystem::OnUpdate>(this);
                m_registry->on_destroy<Component>().template disconnect<&ReactiveSystem::OnDestroy>(this);
                m_registry = nullptr;
            }
        }

        /**
         * @brief Process all accumulated changes since the last Update().
         *
         * Iterates the change queue and calls OnComponentChanged() for each.
         * Clears the queue after processing.
         */
        void Update(World& world, float deltaTime) override
        {
            for (const auto& change : m_changes)
            {
                if (!m_registry)
                    break;

                if (change.type == ChangeType::Destroyed)
                {
                    // Entity may already be invalid (registry.destroy()) and the
                    // component already removed — notify without reference
                    OnComponentDestroyed(world, change.entity);
                    continue;
                }

                // Skip destroyed entities
                if (!m_registry->valid(change.entity))
                    continue;

                if (m_registry->template all_of<Component>(change.entity))
                {
                    auto& comp = m_registry->template get<Component>(change.entity);
                    OnComponentChanged(world, change.entity, comp, change.type);
                }
            }

            m_changes.clear();
        }

        /**
         * @brief Override this to handle component changes.
         *
         * Called once per changed entity per frame, with the component reference
         * and the type of change.
         */
        virtual void OnComponentChanged(World& world, EntityID entity, Component& comp, ChangeType type) = 0;

        /**
         * @brief Override this to handle component destruction (optional).
         *
         * Called when a component is destroyed. The component may already be
         * removed from the entity when this is called.
         */
        virtual void OnComponentDestroyed(World& /*world*/, EntityID /*entity*/) {}

        /**
         * @brief Get the number of pending changes.
         */
        size_t GetPendingChangeCount() const { return m_changes.size(); }

      private:
        struct ChangeEntry
        {
            EntityID entity;
            ChangeType type;
        };

        void OnConstruct(entt::registry& /*registry*/, entt::entity entity)
        {
            m_changes.push_back({entity, ChangeType::Constructed});
        }

        void OnUpdate(entt::registry& /*registry*/, entt::entity entity)
        {
            m_changes.push_back({entity, ChangeType::Updated});
        }

        void OnDestroy(entt::registry& /*registry*/, entt::entity entity)
        {
            m_changes.push_back({entity, ChangeType::Destroyed});
        }

        entt::registry* m_registry = nullptr;
        std::vector<ChangeEntry> m_changes;
    };

    /**
     * @brief Concrete reactive system for MeshRenderer material changes.
     *
     * Only processes entities whose MeshRenderer was modified, avoiding
     * full iteration of all renderable entities every frame.
     */
    class MaterialChangeReactiveSystem : public ReactiveSystem<MeshRenderer>
    {
      public:
        void OnComponentChanged(World& world, EntityID entity, MeshRenderer& renderer, ChangeType type) override
        {
            if (type == ChangeType::Updated || type == ChangeType::Constructed)
            {
                // Mark the world matrix as dirty so it's recalculated
                renderer.worldMatrixDirty = true;
            }
        }

        const char* GetName() const override { return "MaterialChangeReactiveSystem"; }
    };

    /**
     * @brief Reactive system for LightComponent changes.
     *
     * Only rebuilds shadow maps and light data for lights that actually changed.
     */
    class LightChangeReactiveSystem : public ReactiveSystem<LightComponent>
    {
      public:
        void OnComponentChanged(World& world, EntityID entity, LightComponent& light, ChangeType type) override
        {
            if (type == ChangeType::Updated || type == ChangeType::Constructed)
            {
                m_dirtyLightCount++;
            }
        }

        const char* GetName() const override { return "LightChangeReactiveSystem"; }

        uint32_t GetDirtyLightCount() const { return m_dirtyLightCount; }
        void ResetDirtyCount() { m_dirtyLightCount = 0; }

      private:
        uint32_t m_dirtyLightCount = 0;
    };

} // namespace Spark::ECS
