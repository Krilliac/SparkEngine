# Gameplay Systems

SparkEngine includes a comprehensive set of FPS gameplay systems built on top of the [[Entity Component System]] architecture.

**Source:** `SparkEngine/Source/Game/`

## Player Controller

The FPS player controller provides standard first-person movement and interaction:

- **Movement** — WASD movement with configurable speed
- **Mouse look** — Pitch/yaw camera control with sensitivity settings
- **Jump** — Space bar with configurable jump height and gravity
- **Crouch** — Ctrl with height adjustment
- **Sprint** — Shift for increased movement speed

## Weapon System

Three projectile types are supported:

| Type | Description |
|------|-------------|
| **Bullet** | Hitscan or fast projectile with raycast hit detection |
| **Rocket** | Physics-driven projectile with area-of-effect damage |
| **Grenade** | Arc-based projectile with timed detonation |

Features:
- Fire rate control
- Recoil and spread patterns
- Reload mechanics
- Ammunition tracking
- Muzzle flash and impact effects

## Class System

A player class system for multiplayer game modes with configurable:
- Starting weapons and equipment
- Health and armor values
- Movement speed modifiers
- Special abilities

## Vehicle System

[[Physics]]-driven vehicle mechanics:
- Enter/exit vehicles
- Steering and acceleration
- Suspension and physics simulation

## HUD System

Built-in HUD elements for FPS gameplay:

| Element | Description |
|---------|-------------|
| Crosshair | Configurable weapon crosshair |
| Health bar | Player health display |
| Kill feed | Recent kill notifications |
| Minimap | Top-down area overview |
| Compass | Directional compass |
| Ammo counter | Current weapon ammunition |

## Inventory System

`ENABLE_INVENTORY=ON`

Item management system:
- Inventory slots with weight/capacity limits
- Item stacking
- Pickup and drop mechanics
- Equipment slots

## Quest System

`ENABLE_QUEST_SYSTEM=ON`

Objective tracking:
- Quest definitions with stages
- Objective tracking and completion
- Quest log and notifications
- Integration with `QuestCompletedEvent`

## Game Mode Management

Configurable game modes for multiplayer:
- Deathmatch
- Team Deathmatch
- Objective-based modes
- Score tracking and round management

## HealthComponent

Core gameplay component for damageable entities:

```cpp
auto& hp = world.AddComponent<HealthComponent>(entity);
hp.maxHealth     = 100.0f;
hp.health        = 100.0f;
hp.isDead        = false;
hp.isInvulnerable = false;
```

Damage events are published through the [[Event System|event bus]]:

```cpp
bus.Publish(EntityDamagedEvent{ entityId, damage, "Weapon" });
```

## Gravity System

Configurable gravity for physics-driven gameplay:
- Per-scene gravity settings (via `SceneMetadata`, see [[Scene Management]])
- Gravity zones for localized effects

## Interactive Objects

Objects that respond to player interaction:
- Doors (open/close)
- Buttons and switches
- Pickup items

---

## See Also

- [[Entity Component System]] — Gameplay components
- [[Physics]] — Physics-driven gameplay
- [[Event System]] — Gameplay events
- [[AI and Navigation]] — NPC behavior
- [[Networking]] — Multiplayer game modes
- [[Scene Management]] — Scene gravity and level loading
- [[Scripting with AngelScript]] — Scripting gameplay logic
- [[Input System]] — Player input handling
- [[Audio]] — Gameplay sound effects
- [[Rendering and Graphics]] — HUD and visual effects
- [[Animation]] — Player and weapon animations
- [[Terrain and Procedural Generation]] — Level environments
