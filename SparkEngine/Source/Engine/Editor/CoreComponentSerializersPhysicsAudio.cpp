/**
 * @file CoreComponentSerializersPhysicsAudio.cpp
 * @brief Snapshot serializers for the physics and audio core ECS component types
 *
 * Split from CoreComponentSerializers.cpp to keep each translation unit small.
 * Covers AudioSourceComponent, RigidBodyComponent and ColliderComponent, and
 * defines RegisterPhysicsAudioComponentSerializers(), which is called only by
 * RegisterCoreComponentSerializers() so the overall registration order matches
 * the pre-split single file.
 */

#include "CoreComponentSerializersInternal.h"
#include "SceneSnapshotSerializer.h"

#include "../ECS/Components/PhysicsComponents.h"
#include "../ECS/Components/AudioComponents.h"

#include <entt/entt.hpp>

#include <string>
#include <utility>

namespace Spark::Editor
{

    // ========================================================================
    // Per-component serializer helpers
    // ========================================================================

    // --- AudioSourceComponent ---
    static uint32_t SerializeAudioSource(SnapshotWriter& w, const void* reg)
    {
        auto& registry = MutableReg(reg);
        auto view = registry.view<AudioSourceComponent>();

        // Count first so the record count is written correctly (append-only writer).
        uint32_t count = 0;
        for (auto entity : view)
        {
            if (registry.try_get<AudioSourceComponent>(entity))
                ++count;
        }
        w.WriteU32(count);

        for (auto entity : view)
        {
            const auto* aPtr = registry.try_get<AudioSourceComponent>(entity);
            if (!aPtr)
                continue;
            const auto& a = *aPtr;
            w.WriteU32(static_cast<uint32_t>(entity));
            w.WriteString(a.soundName);
            w.WriteFloat(a.volume);
            w.WriteFloat(a.pitch);
            w.WriteFloat(a.minDistance);
            w.WriteFloat(a.maxDistance);
            w.WriteBool(a.is3D);
            w.WriteBool(a.loop);
            w.WriteBool(a.playOnAwake);
        }
        return count;
    }

    static bool DeserializeAudioSource(SnapshotReader& r, void* reg, uint32_t /*entityCount*/)
    {
        auto& registry = MutableReg(reg);
        uint32_t count = r.ReadU32();
        for (uint32_t i = 0; i < count; ++i)
        {
            auto entity = static_cast<entt::entity>(r.ReadU32());
            AudioSourceComponent a;
            a.soundName = r.ReadString();
            a.volume = r.ReadFloat();
            a.pitch = r.ReadFloat();
            a.minDistance = r.ReadFloat();
            a.maxDistance = r.ReadFloat();
            a.is3D = r.ReadBool();
            a.loop = r.ReadBool();
            a.playOnAwake = r.ReadBool();

            entity = EnsureEntity(registry, entity);
            EmplaceOrReplace(registry, entity, std::move(a));
        }
        return r.IsValid();
    }

    // --- RigidBodyComponent (config only, not physics state) ---
    static uint32_t SerializeRigidBody(SnapshotWriter& w, const void* reg)
    {
        auto& registry = MutableReg(reg);
        auto view = registry.view<RigidBodyComponent>();

        // Count first so the record count is written correctly (append-only writer).
        uint32_t count = 0;
        for (auto entity : view)
        {
            if (registry.try_get<RigidBodyComponent>(entity))
                ++count;
        }
        w.WriteU32(count);

        for (auto entity : view)
        {
            const auto* rbPtr = registry.try_get<RigidBodyComponent>(entity);
            if (!rbPtr)
                continue;
            const auto& rb = *rbPtr;
            w.WriteU32(static_cast<uint32_t>(entity));
            w.WriteU32(static_cast<uint32_t>(rb.type));
            w.WriteFloat(rb.mass);
            w.WriteFloat(rb.friction);
            w.WriteFloat(rb.restitution);
            w.WriteFloat(rb.linearDamping);
            w.WriteFloat(rb.angularDamping);
            w.WriteFloat(rb.gravityFactor);
            w.WriteBool(rb.isTrigger);
        }
        return count;
    }

    static bool DeserializeRigidBody(SnapshotReader& r, void* reg, uint32_t /*entityCount*/)
    {
        auto& registry = MutableReg(reg);
        uint32_t count = r.ReadU32();
        for (uint32_t i = 0; i < count; ++i)
        {
            auto entity = static_cast<entt::entity>(r.ReadU32());
            RigidBodyComponent rb;
            rb.type = static_cast<RigidBodyComponent::Type>(r.ReadU32());
            rb.mass = r.ReadFloat();
            rb.friction = r.ReadFloat();
            rb.restitution = r.ReadFloat();
            rb.linearDamping = r.ReadFloat();
            rb.angularDamping = r.ReadFloat();
            rb.gravityFactor = r.ReadFloat();
            rb.isTrigger = r.ReadBool();

            entity = EnsureEntity(registry, entity);
            EmplaceOrReplace(registry, entity, rb);
        }
        return r.IsValid();
    }

    // --- ColliderComponent ---
    static uint32_t SerializeCollider(SnapshotWriter& w, const void* reg)
    {
        auto& registry = MutableReg(reg);
        auto view = registry.view<ColliderComponent>();

        // Count first so the record count is written correctly (append-only writer).
        uint32_t count = 0;
        for (auto entity : view)
        {
            if (registry.try_get<ColliderComponent>(entity))
                ++count;
        }
        w.WriteU32(count);

        for (auto entity : view)
        {
            const auto* colPtr = registry.try_get<ColliderComponent>(entity);
            if (!colPtr)
                continue;
            const auto& col = *colPtr;
            w.WriteU32(static_cast<uint32_t>(entity));
            w.WriteU32(static_cast<uint32_t>(col.shape));
            w.WriteFloat3(col.halfExtents.x, col.halfExtents.y, col.halfExtents.z);
            w.WriteFloat(col.radius);
            w.WriteFloat(col.height);
            w.WriteFloat3(col.offset.x, col.offset.y, col.offset.z);
        }
        return count;
    }

    static bool DeserializeCollider(SnapshotReader& r, void* reg, uint32_t /*entityCount*/)
    {
        auto& registry = MutableReg(reg);
        uint32_t count = r.ReadU32();
        for (uint32_t i = 0; i < count; ++i)
        {
            auto entity = static_cast<entt::entity>(r.ReadU32());
            ColliderComponent col;
            col.shape = static_cast<ColliderComponent::Shape>(r.ReadU32());
            r.ReadFloat3(col.halfExtents.x, col.halfExtents.y, col.halfExtents.z);
            col.radius = r.ReadFloat();
            col.height = r.ReadFloat();
            r.ReadFloat3(col.offset.x, col.offset.y, col.offset.z);

            entity = EnsureEntity(registry, entity);
            EmplaceOrReplace(registry, entity, col);
        }
        return r.IsValid();
    }

    // ========================================================================
    // Registration
    // ========================================================================

    void RegisterPhysicsAudioComponentSerializers()
    {
        auto& reg = ComponentSerializerRegistry::Instance();

        reg.Register(
            {"AudioSourceComponent", TypeId<AudioSourceComponent>(), SerializeAudioSource, DeserializeAudioSource});
        reg.Register({"RigidBodyComponent", TypeId<RigidBodyComponent>(), SerializeRigidBody, DeserializeRigidBody});
        reg.Register({"ColliderComponent", TypeId<ColliderComponent>(), SerializeCollider, DeserializeCollider});
    }

} // namespace Spark::Editor
