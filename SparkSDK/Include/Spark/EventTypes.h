/**
 * @file EventTypes.h
 * @brief Public built-in event types and dispatch APIs for game modules
 *
 * Game modules communicate through the EventBus obtained from
 * IEngineContext::GetEventBus(). This header includes the canonical built-in
 * event types and QueuedEventBus from <Spark/EventSystem.h>, together with
 * EventBus and SubscriptionHandle from <Spark/EventBus.h>.
 *
 * Modules can also define their own custom event structs without any
 * base class or registration -- the EventBus uses template type erasure.
 *
 * Usage:
 *   Include <Spark/EventSystem.h> for built-in event types and deferred dispatch.
 *   Include <Spark/EventBus.h> for EventBus and SubscriptionHandle alone.
 *   Keep subscription handles alive, then release them before module unload.
 *   Follow EventBus's DLL lifetime contract for host channel and queue cleanup.
 *
 * Built-in event types (in Spark/EventSystem.h):
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

#include <Spark/EventSystem.h>
