/**
 * @file CoreComponentSerializersInternal.h
 * @brief Shared internal helpers for the core component snapshot serializer translation units
 *
 * Internal-only header included by CoreComponentSerializers.cpp and
 * CoreComponentSerializersPhysicsAudio.cpp. Provides the type-ID hash,
 * registry-pointer casts, and emplace/entity-restore helpers used by every
 * per-component serializer, plus the cross-file registration hook so the
 * physics/audio serializers register in the same order as before the split.
 * Not part of the public API — do not include outside the serializer parts.
 */

#pragma once

#include <entt/entt.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <utility>

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

    static entt::entity EnsureEntity(entt::registry& registry, entt::entity entity)
    {
        if (registry.valid(entity))
            return entity;

        if constexpr (requires(entt::registry& r, entt::entity e) { r.create(e); })
        {
            return registry.create(entity);
        }
        else
        {
            constexpr uint32_t kMaxEntityRestoreGap = 1'000'000;
            const uint32_t target = static_cast<uint32_t>(entity);
            for (uint32_t i = 0; i <= kMaxEntityRestoreGap && !registry.valid(entity); ++i)
            {
                entt::entity created = registry.create();
                if (created == entity || static_cast<uint32_t>(created) >= target)
                    break;
            }
            return registry.valid(entity) ? entity : registry.create();
        }
    }

    /// @brief Register the physics and audio component serializers (RigidBodyComponent,
    /// ColliderComponent, AudioSourceComponent). Called only by
    /// RegisterCoreComponentSerializers(), which owns the once-only guard.
    void RegisterPhysicsAudioComponentSerializers();

} // namespace Spark::Editor
