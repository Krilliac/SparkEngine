/**
 * @file AngelScriptTransformRef.cpp
 * @brief The script `Transform@` handle: an entity id re-resolved on every access
 *
 * getTransform() used to hand scripts a raw, uncounted pointer into the EnTT
 * Transform pool. A script that kept it past destroyEntity() then read and
 * wrote whichever entity EnTT swapped into the slot, or a dead slot, and after
 * World teardown freed memory. ScriptTransformRef stores only the EntityID; see
 * AngelScriptEngine.h for its contract. Shared by the real and the stub (no
 * AngelScript) builds; only the script-exception report needs the SDK.
 */

#include "AngelScriptEngine.h"
#include "../../Utils/LogMacros.h"

namespace
{
    /// Raise @p message as a script exception on the executing context. A native
    /// caller outside script execution has no context and gets the safe default.
    void RaiseScriptException([[maybe_unused]] const char* message) noexcept
    {
#ifdef SPARK_ANGELSCRIPT_SUPPORT
        if (asIScriptContext* context = asGetActiveContext())
        {
            context->SetException(message);
        }
#endif
    }

    /// The entity's Transform in the bound World, or nullptr when the World is
    /// unbound, the entity is not alive in it or has no Transform.
    Transform* FindLiveTransform(EntityID entity) noexcept
    {
        World* world = AngelScriptEngine::GetBoundWorld();
        if (!world || entity == entt::null || !world->GetRegistry().valid(entity))
        {
            return nullptr;
        }
        return world->GetRegistry().try_get<Transform>(entity);
    }
} // namespace

void ScriptTransformRef::AddRef() noexcept
{
    ++m_refCount;
}

void ScriptTransformRef::Release() noexcept
{
    if (--m_refCount == 0)
    {
        delete this; // AngelScript reference counting owns the object (created in ASGetTransform)
    }
}

bool ScriptTransformRef::IsValid() const noexcept
{
    return FindLiveTransform(m_entity) != nullptr;
}

Transform* ScriptTransformRef::Resolve() const noexcept
{
    Transform* transform = FindLiveTransform(m_entity);
    if (!transform)
    {
        RaiseScriptException("Transform handle used after its entity was destroyed or lost its Transform");
    }
    return transform;
}

DirectX::XMFLOAT3 ScriptTransformRef::GetPosition() const noexcept
{
    const Transform* transform = Resolve();
    return transform ? transform->position : DirectX::XMFLOAT3{0.0f, 0.0f, 0.0f};
}

void ScriptTransformRef::SetPosition(const DirectX::XMFLOAT3& value) noexcept
{
    if (Transform* transform = Resolve())
    {
        transform->position = value;
    }
}

DirectX::XMFLOAT3 ScriptTransformRef::GetRotation() const noexcept
{
    const Transform* transform = Resolve();
    return transform ? transform->rotation : DirectX::XMFLOAT3{0.0f, 0.0f, 0.0f};
}

void ScriptTransformRef::SetRotation(const DirectX::XMFLOAT3& value) noexcept
{
    if (Transform* transform = Resolve())
    {
        transform->rotation = value;
    }
}

DirectX::XMFLOAT3 ScriptTransformRef::GetScale() const noexcept
{
    const Transform* transform = Resolve();
    return transform ? transform->scale : DirectX::XMFLOAT3{0.0f, 0.0f, 0.0f};
}

void ScriptTransformRef::SetScale(const DirectX::XMFLOAT3& value) noexcept
{
    if (Transform* transform = Resolve())
    {
        transform->scale = value;
    }
}

ScriptTransformRef* ASGetTransform(EntityID entity)
{
    if (!FindLiveTransform(entity))
    {
        SPARK_LOG_WARN(Spark::LogCategory::Scripting,
                       "getTransform: entity %u is not alive in the bound World or has no Transform",
                       static_cast<uint32_t>(entity));
        return nullptr;
    }
    // Ownership passes to AngelScript with the one reference the constructor holds.
    return new ScriptTransformRef(entity);
}
