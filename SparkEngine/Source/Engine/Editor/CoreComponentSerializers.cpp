/**
 * @file CoreComponentSerializers.cpp
 * @brief Snapshot serializers for core scene ECS component types
 *
 * Registers ComponentSerializerEntry callbacks so SceneSnapshotSerializer can
 * round-trip every core component through the binary snapshot format used by
 * PlayModeManager's save/restore. This file covers NameComponent,
 * ActiveComponent, Transform, LightComponent, Camera and MeshRenderer, and owns
 * the public RegisterCoreComponentSerializers() entry point; the physics and
 * audio serializers live in CoreComponentSerializersPhysicsAudio.cpp.
 */

#include "CoreComponentSerializers.h"
#include "CoreComponentSerializersInternal.h"
#include "SceneSnapshotSerializer.h"

#include "../ECS/Components/CoreComponents.h"
#include "../ECS/Components/GameplayComponents.h"
#include "../ECS/Components/LightComponents.h"

#include <entt/entt.hpp>

#include <atomic>
#include <string>

namespace Spark::Editor
{

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
            entity = EnsureEntity(registry, entity);
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
            entity = EnsureEntity(registry, entity);
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

            entity = EnsureEntity(registry, entity);
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

            entity = EnsureEntity(registry, entity);
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

            entity = EnsureEntity(registry, entity);
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

            entity = EnsureEntity(registry, entity);
            EmplaceOrReplace(registry, entity, std::move(m));
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

        // Physics and audio serializers live in CoreComponentSerializersPhysicsAudio.cpp;
        // register them here so the overall registration order matches the pre-split file.
        RegisterPhysicsAudioComponentSerializers();
    }

} // namespace Spark::Editor
