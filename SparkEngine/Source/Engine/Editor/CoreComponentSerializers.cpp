/**
 * @file CoreComponentSerializers.cpp
 * @brief Snapshot serializers for all core ECS component types
 *
 * Registers ComponentSerializerEntry callbacks so SceneSnapshotSerializer can
 * round-trip every core component through the binary snapshot format used by
 * PlayModeManager's save/restore.
 */

#include "CoreComponentSerializers.h"
#include "SceneSnapshotSerializer.h"

#include "../ECS/Components/CoreComponents.h"
#include "../ECS/Components/GameplayComponents.h"
#include "../ECS/Components/LightComponents.h"
#include "../ECS/Components/PhysicsComponents.h"
#include "../ECS/Components/AudioComponents.h"

#include <entt/entt.hpp>

#include <atomic>
#include <functional>
#include <string>
#include <typeinfo>

namespace Spark::Editor
{

    // Simple compile-time type ID using typeid name hash.
    // The stub EnTT doesn't provide entt::type_hash, so we hash the name.
    template <typename T> static uint32_t TypeId()
    {
        return static_cast<uint32_t>(std::hash<std::string>{}(typeid(T).name()) & 0xFFFFFFFF);
    }

    // Helper: the stub's view() is non-const, so we need a mutable reference.
    // The serialize callbacks receive const void*, but EnTT view requires mutable.
    static entt::registry& MutableReg(const void* reg)
    {
        return *const_cast<entt::registry*>(static_cast<const entt::registry*>(reg));
    }

    static entt::registry& MutableReg(void* reg)
    {
        return *static_cast<entt::registry*>(reg);
    }

    // Helper: emplace-or-replace (stub lacks emplace_or_replace)
    template <typename T> static void EmplaceOrReplace(entt::registry& registry, entt::entity entity, T&& comp)
    {
        if (registry.all_of<std::remove_cvref_t<T>>(entity))
            registry.remove<std::remove_cvref_t<T>>(entity);
        registry.emplace<std::remove_cvref_t<T>>(entity, std::forward<T>(comp));
    }

    // ========================================================================
    // Per-component serializer helpers
    // ========================================================================

    // --- NameComponent ---
    static uint32_t SerializeName(SnapshotWriter& w, const void* reg)
    {
        auto& registry = MutableReg(reg);
        auto view = registry.view<NameComponent>();

        // Count matching instances first so the record count is written correctly.
        // BinaryWriter is append-only (no seek/patch), so a placeholder cannot be
        // back-patched; a stale 0 here would desync the whole snapshot on restore.
        uint32_t count = 0;
        for (auto entity : view)
        {
            if (registry.try_get<NameComponent>(entity))
                ++count;
        }
        w.WriteU32(count);

        for (auto entity : view)
        {
            const auto* compPtr = registry.try_get<NameComponent>(entity);
            if (!compPtr)
                continue;
            const auto& comp = *compPtr;
            w.WriteU32(static_cast<uint32_t>(entity));
            w.WriteString(comp.name);
        }
        return count;
    }

    static bool DeserializeName(SnapshotReader& r, void* reg, uint32_t /*entityCount*/)
    {
        auto& registry = MutableReg(reg);
        uint32_t count = r.ReadU32();
        for (uint32_t i = 0; i < count; ++i)
        {
            auto rawId = r.ReadU32();
            std::string name = r.ReadString();
            auto entity = static_cast<entt::entity>(rawId);
            if (!registry.valid(entity))
                entity = registry.create();
            EmplaceOrReplace(registry, entity, NameComponent{std::move(name)});
        }
        return r.IsValid();
    }

    // --- ActiveComponent ---
    static uint32_t SerializeActive(SnapshotWriter& w, const void* reg)
    {
        auto& registry = MutableReg(reg);
        auto view = registry.view<ActiveComponent>();

        // Count first so the record count is written correctly (append-only writer).
        uint32_t count = 0;
        for (auto entity : view)
        {
            if (registry.try_get<ActiveComponent>(entity))
                ++count;
        }
        w.WriteU32(count);

        for (auto entity : view)
        {
            const auto* compPtr = registry.try_get<ActiveComponent>(entity);
            if (!compPtr)
                continue;
            const auto& comp = *compPtr;
            w.WriteU32(static_cast<uint32_t>(entity));
            w.WriteBool(comp.active);
        }
        return count;
    }

    static bool DeserializeActive(SnapshotReader& r, void* reg, uint32_t /*entityCount*/)
    {
        auto& registry = MutableReg(reg);
        uint32_t count = r.ReadU32();
        for (uint32_t i = 0; i < count; ++i)
        {
            auto entity = static_cast<entt::entity>(r.ReadU32());
            bool active = r.ReadBool();
            if (!registry.valid(entity))
                entity = registry.create();
            EmplaceOrReplace(registry, entity, ActiveComponent{active});
        }
        return r.IsValid();
    }

    // --- Transform ---
    static uint32_t SerializeTransform(SnapshotWriter& w, const void* reg)
    {
        auto& registry = MutableReg(reg);
        auto view = registry.view<Transform>();

        // Count first so the record count is written correctly (append-only writer).
        uint32_t count = 0;
        for (auto entity : view)
        {
            if (registry.try_get<Transform>(entity))
                ++count;
        }
        w.WriteU32(count);

        for (auto entity : view)
        {
            const auto* tPtr = registry.try_get<Transform>(entity);
            if (!tPtr)
                continue;
            const auto& t = *tPtr;
            w.WriteU32(static_cast<uint32_t>(entity));
            w.WriteFloat3(t.position.x, t.position.y, t.position.z);
            w.WriteFloat3(t.rotation.x, t.rotation.y, t.rotation.z);
            w.WriteFloat3(t.scale.x, t.scale.y, t.scale.z);
            w.WriteU32(static_cast<uint32_t>(t.parent));
            w.WriteU32(static_cast<uint32_t>(t.children.size()));
            for (auto child : t.children)
                w.WriteU32(static_cast<uint32_t>(child));
        }
        return count;
    }

    static bool DeserializeTransform(SnapshotReader& r, void* reg, uint32_t /*entityCount*/)
    {
        auto& registry = MutableReg(reg);
        uint32_t count = r.ReadU32();
        for (uint32_t i = 0; i < count; ++i)
        {
            auto entity = static_cast<entt::entity>(r.ReadU32());
            Transform t;
            r.ReadFloat3(t.position.x, t.position.y, t.position.z);
            r.ReadFloat3(t.rotation.x, t.rotation.y, t.rotation.z);
            r.ReadFloat3(t.scale.x, t.scale.y, t.scale.z);
            t.parent = static_cast<entt::entity>(r.ReadU32());
            uint32_t childCount = r.ReadU32();
            t.children.resize(childCount);
            for (uint32_t c = 0; c < childCount; ++c)
                t.children[c] = static_cast<entt::entity>(r.ReadU32());

            if (!registry.valid(entity))
                entity = registry.create();
            EmplaceOrReplace(registry, entity, std::move(t));
        }
        return r.IsValid();
    }

    // --- LightComponent ---
    static uint32_t SerializeLight(SnapshotWriter& w, const void* reg)
    {
        auto& registry = MutableReg(reg);
        auto view = registry.view<LightComponent>();

        // Count first so the record count is written correctly (append-only writer).
        uint32_t count = 0;
        for (auto entity : view)
        {
            if (registry.try_get<LightComponent>(entity))
                ++count;
        }
        w.WriteU32(count);

        for (auto entity : view)
        {
            const auto* lPtr = registry.try_get<LightComponent>(entity);
            if (!lPtr)
                continue;
            const auto& l = *lPtr;
            w.WriteU32(static_cast<uint32_t>(entity));
            w.WriteU32(static_cast<uint32_t>(l.type));
            w.WriteFloat3(l.color.x, l.color.y, l.color.z);
            w.WriteFloat(l.intensity);
            w.WriteFloat(l.range);
            w.WriteFloat(l.spotAngle);
            w.WriteFloat(l.spotInnerAngle);
            w.WriteBool(l.castShadows);
            w.WriteU32(static_cast<uint32_t>(l.shadowMapResolution));
        }
        return count;
    }

    static bool DeserializeLight(SnapshotReader& r, void* reg, uint32_t /*entityCount*/)
    {
        auto& registry = MutableReg(reg);
        uint32_t count = r.ReadU32();
        for (uint32_t i = 0; i < count; ++i)
        {
            auto entity = static_cast<entt::entity>(r.ReadU32());
            LightComponent l;
            l.type = static_cast<LightComponent::Type>(r.ReadU32());
            r.ReadFloat3(l.color.x, l.color.y, l.color.z);
            l.intensity = r.ReadFloat();
            l.range = r.ReadFloat();
            l.spotAngle = r.ReadFloat();
            l.spotInnerAngle = r.ReadFloat();
            l.castShadows = r.ReadBool();
            l.shadowMapResolution = static_cast<int>(r.ReadU32());

            if (!registry.valid(entity))
                entity = registry.create();
            EmplaceOrReplace(registry, entity, l);
        }
        return r.IsValid();
    }

    // --- Camera ---
    static uint32_t SerializeCamera(SnapshotWriter& w, const void* reg)
    {
        auto& registry = MutableReg(reg);
        auto view = registry.view<Camera>();

        // Count first so the record count is written correctly (append-only writer).
        uint32_t count = 0;
        for (auto entity : view)
        {
            if (registry.try_get<Camera>(entity))
                ++count;
        }
        w.WriteU32(count);

        for (auto entity : view)
        {
            const auto* cPtr = registry.try_get<Camera>(entity);
            if (!cPtr)
                continue;
            const auto& c = *cPtr;
            w.WriteU32(static_cast<uint32_t>(entity));
            w.WriteFloat(c.fov);
            w.WriteFloat(c.nearPlane);
            w.WriteFloat(c.farPlane);
            w.WriteBool(c.isMainCamera);
        }
        return count;
    }

    static bool DeserializeCamera(SnapshotReader& r, void* reg, uint32_t /*entityCount*/)
    {
        auto& registry = MutableReg(reg);
        uint32_t count = r.ReadU32();
        for (uint32_t i = 0; i < count; ++i)
        {
            auto entity = static_cast<entt::entity>(r.ReadU32());
            Camera c;
            c.fov = r.ReadFloat();
            c.nearPlane = r.ReadFloat();
            c.farPlane = r.ReadFloat();
            c.isMainCamera = r.ReadBool();

            if (!registry.valid(entity))
                entity = registry.create();
            EmplaceOrReplace(registry, entity, c);
        }
        return r.IsValid();
    }

    // --- MeshRenderer ---
    static uint32_t SerializeMeshRenderer(SnapshotWriter& w, const void* reg)
    {
        auto& registry = MutableReg(reg);
        auto view = registry.view<MeshRenderer>();

        // Count first so the record count is written correctly (append-only writer).
        uint32_t count = 0;
        for (auto entity : view)
        {
            if (registry.try_get<MeshRenderer>(entity))
                ++count;
        }
        w.WriteU32(count);

        for (auto entity : view)
        {
            const auto* mPtr = registry.try_get<MeshRenderer>(entity);
            if (!mPtr)
                continue;
            const auto& m = *mPtr;
            w.WriteU32(static_cast<uint32_t>(entity));
            w.WriteString(m.meshPath);
            w.WriteString(m.materialPath);
            w.WriteBool(m.castShadows);
            w.WriteBool(m.receiveShadows);
            w.WriteBool(m.visible);
        }
        return count;
    }

    static bool DeserializeMeshRenderer(SnapshotReader& r, void* reg, uint32_t /*entityCount*/)
    {
        auto& registry = MutableReg(reg);
        uint32_t count = r.ReadU32();
        for (uint32_t i = 0; i < count; ++i)
        {
            auto entity = static_cast<entt::entity>(r.ReadU32());
            MeshRenderer m;
            m.meshPath = r.ReadString();
            m.materialPath = r.ReadString();
            m.castShadows = r.ReadBool();
            m.receiveShadows = r.ReadBool();
            m.visible = r.ReadBool();

            if (!registry.valid(entity))
                entity = registry.create();
            EmplaceOrReplace(registry, entity, std::move(m));
        }
        return r.IsValid();
    }

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

            if (!registry.valid(entity))
                entity = registry.create();
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

            if (!registry.valid(entity))
                entity = registry.create();
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

            if (!registry.valid(entity))
                entity = registry.create();
            EmplaceOrReplace(registry, entity, col);
        }
        return r.IsValid();
    }

    // ========================================================================
    // Registration
    // ========================================================================

    void RegisterCoreComponentSerializers()
    {
        static std::atomic<bool> s_registered{false};
        if (s_registered.exchange(true))
            return;

        auto& reg = ComponentSerializerRegistry::Instance();

        reg.Register({"NameComponent", TypeId<NameComponent>(), SerializeName, DeserializeName});
        reg.Register({"ActiveComponent", TypeId<ActiveComponent>(), SerializeActive, DeserializeActive});
        reg.Register({"Transform", TypeId<Transform>(), SerializeTransform, DeserializeTransform});
        reg.Register({"LightComponent", TypeId<LightComponent>(), SerializeLight, DeserializeLight});
        reg.Register({"Camera", TypeId<Camera>(), SerializeCamera, DeserializeCamera});
        reg.Register({"MeshRenderer", TypeId<MeshRenderer>(), SerializeMeshRenderer, DeserializeMeshRenderer});
        reg.Register(
            {"AudioSourceComponent", TypeId<AudioSourceComponent>(), SerializeAudioSource, DeserializeAudioSource});
        reg.Register({"RigidBodyComponent", TypeId<RigidBodyComponent>(), SerializeRigidBody, DeserializeRigidBody});
        reg.Register({"ColliderComponent", TypeId<ColliderComponent>(), SerializeCollider, DeserializeCollider});
    }

} // namespace Spark::Editor
