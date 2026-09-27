/**
 * @file AngelScriptEngineEvents.cpp
 * @brief Engine-owned delivery of physics/trigger contact events to entity scripts
 *
 * AngelScriptEngine::ConnectEventBus() subscribes the script runtime to the
 * contact events PhysicsSystem (and the TriggerVolumeComponent bridge) publish
 * on the engine EventBus, so an attached script's OnCollision(EntityID),
 * OnTriggerEnter(EntityID) and OnTriggerExit(EntityID) run without any game
 * module forwarding them. Shared by the real and the stub (no AngelScript)
 * builds: the stub's Call* methods are no-ops.
 */

#include "AngelScriptEngine.h"
#include "../Events/EventSystem.h"

void AngelScriptEngine::ConnectEventBus(Spark::EventBus* bus)
{
    // Destroying a handle unsubscribes it.
    m_contactSubscriptions.clear();
    if (!bus)
    {
        return;
    }

    m_contactSubscriptions.reserve(3);
    m_contactSubscriptions.push_back(bus->Subscribe<Spark::CollisionEvent>(
        [this](const Spark::CollisionEvent& e) {
            DispatchContact(ContactCallback::Collision, static_cast<EntityID>(e.entityA),
                            static_cast<EntityID>(e.entityB));
        }));
    m_contactSubscriptions.push_back(bus->Subscribe<Spark::TriggerEnterEvent>(
        [this](const Spark::TriggerEnterEvent& e)
        {
            DispatchContact(ContactCallback::TriggerEnter, static_cast<EntityID>(e.entityId),
                            static_cast<EntityID>(e.triggerId));
        }));
    m_contactSubscriptions.push_back(bus->Subscribe<Spark::TriggerExitEvent>(
        [this](const Spark::TriggerExitEvent& e)
        {
            DispatchContact(ContactCallback::TriggerExit, static_cast<EntityID>(e.entityId),
                            static_cast<EntityID>(e.triggerId));
        }));
}

void AngelScriptEngine::DispatchContact(ContactCallback callback, EntityID first, EntityID second)
{
    // Most contacts involve entities with no script: skip them before any lookup work.
    if (m_entityScripts.empty())
    {
        return;
    }

    // Entity id 0 is the physics "no entity" sentinel (PhysicsBodyDesc::entityId): terrain, props and other bodies
    // created outside the ECS all report 0. It also equals the first entity a World creates, so a 0 in a contact
    // event cannot be trusted to name that entity. It is never a delivery target, and the other participant is
    // told it touched entt::null (what the script entity API returns for "no entity"), not entity 0.
    static constexpr EntityID kNoEntity{0};
    const EntityID firstOther = (second == kNoEntity) ? EntityID{entt::null} : second;
    const EntityID secondOther = (first == kNoEntity) ? EntityID{entt::null} : first;

    const auto deliver = [this, callback](EntityID target, EntityID other)
    {
        if (target == kNoEntity || m_entityScripts.find(target) == m_entityScripts.end())
        {
            return;
        }
        // Destroying an entity does not remove its physics body, so a contact can still name an entity a script
        // already destroyed (possibly earlier in this same step); that entity's script must not run.
        if (World* world = GetBoundWorld(); world && !world->GetRegistry().valid(target))
        {
            return;
        }

        switch (callback)
        {
        case ContactCallback::Collision:
            CallOnCollision(target, other);
            break;
        case ContactCallback::TriggerEnter:
            CallOnTriggerEnter(target, other);
            break;
        case ContactCallback::TriggerExit:
            CallOnTriggerExit(target, other);
            break;
        }
    };

    deliver(first, firstOther);
    // Two bodies of one entity touching is one contact for that entity's script.
    if (second != first)
    {
        deliver(second, secondOther);
    }
}
