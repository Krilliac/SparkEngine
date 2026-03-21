/**
 * @file EventTypes.h
 * @brief Event system usage guide and forward declarations for game modules
 *
 * Game modules communicate through the EventBus obtained from
 * IEngineContext::GetEventBus(). The engine defines built-in event
 * types in Engine/Events/EventSystem.h. Modules that want to subscribe
 * to engine events should include that header directly.
 *
 * Modules can also define their own custom event structs without any
 * base class or registration -- the EventBus uses template type erasure.
 *
 * Usage:
 *   Include "Engine/Events/EventSystem.h" for built-in event types.
 *   Include "Utils/EventBus.h" for EventBus and SubscriptionHandle.
 *
 * Built-in event types (in Engine/Events/EventSystem.h):
 *   Gameplay: EntityDamagedEvent, EntityKilledEvent, ItemPickedUpEvent
 *   Collision: CollisionEvent, TriggerEnterEvent, TriggerExitEvent
 *   Entity: EntityCreatedEvent, EntityDestroyedEvent, EntityDeathEvent
 *   Scene: SceneLoadedEvent, SceneUnloadedEvent
 *   Engine: EngineStartEvent, EngineShutdownEvent, FrameBeginEvent
 *   Input: InputActionEvent, MouseMoveEvent
 *   Graphics: WindowResizeEvent, QualityChangedEvent
 *   Audio: SoundPlayedEvent
 */

#pragma once
