# Gameplay Systems

SparkEngine includes a comprehensive set of gameplay systems built on top of the [Entity Component System](../subsystems/Entity-Component-System.md) architecture. Beyond the FPS-focused player controller and weapon system, the engine provides: [achievement tracking](../subsystems/Achievement-System.md), [ability/condition systems](../subsystems/Entity-Component-System.md), [event response system](../subsystems/Event-System.md) (data-driven When/If/Then rules), [branching dialogue](../subsystems/Dialogue-System.md), [destructible objects](../subsystems/Destruction-System.md), [replay record/playback](../subsystems/Replay-System.md), [inventory and quests](../subsystems/Entity-Component-System.md), [day/night cycle and weather](Day-Night-Cycle-and-Weather.md), [tween/coroutine](../subsystems/Tween-System.md) systems, and [accessibility](../platform/Accessibility.md) (5 colorblind modes, subtitles, reduced motion, one-handed input).

**Source:** `SparkEngine/Source/Game/`, `SparkEngine/Source/Engine/Gameplay/`, `SparkEngine/Source/Engine/ECS/Components/`

## Module Extension vs Fork Policy

For governance on when gameplay behavior must extend engine systems (Quest/Dialogue) versus when module forks are
acceptable (specialized Loot/Skill/Inventory domains), see:

- `docs/architecture/gameplay-extension-policy.md`

## Player Controller

The FPS player controller provides standard first-person movement and interaction:

- **Movement** -- WASD movement with configurable speed
- **Mouse look** -- Pitch/yaw camera control with sensitivity settings
- **Jump** -- Space bar with configurable jump height and gravity
- **Crouch** -- Ctrl with height adjustment
- **Sprint** -- Shift for increased movement speed

## Weapon System

**Source:** `SparkEngine/Source/Engine/Gameplay/WeaponManager.h`

The weapon system is a data-driven, component-based architecture consisting of:
- `WeaponDefinition` -- Shared template data for a weapon type
- `WeaponInstance` -- Per-entity runtime state (ammo, recoil)
- `WeaponInventoryComponent` -- ECS component attached to weapon-carrying entities
- `WeaponRegistry` -- Singleton registry of all weapon definitions
- `WeaponSystem` -- ECS system that processes weapon behavior each frame

### Fire Modes

```cpp
enum class FireMode : uint8_t
{
    SemiAuto,   // One shot per trigger pull
    Burst,      // Fixed burst (e.g., 3-round burst)
    FullAuto    // Continuous fire while trigger held
};
```

### Weapon States

```cpp
enum class WeaponState : uint8_t
{
    Idle,       // Ready to fire
    Firing,     // Currently firing (cooldown active)
    Reloading,  // Magazine swap in progress
    Switching,  // Equip/holster transition
    Empty,      // Magazine empty, needs reload
    Disabled    // Cannot fire (ability disabled, etc.)
};
```

### Weapon Slots

```cpp
enum class WeaponSlot : uint8_t
{
    Primary = 0,     // Assault rifles, SMGs
    Secondary = 1,   // Pistols
    Melee = 2,       // Knives, melee weapons
    Grenade = 3,     // Grenades, throwables
    MaxSlots = 4
};
```

### WeaponDefinition

The `WeaponDefinition` struct is the data template for a weapon type. It is registered once and shared across all instances:

```cpp
struct WeaponDefinition
{
    uint32_t definitionID;            // Unique weapon type ID
    std::string name;                  // Display name ("AK-47")
    WeaponSlot slot;                   // Inventory slot

    // Damage
    float baseDamage = 25.0f;          // Per-projectile damage
    float headshotMultiplier = 2.0f;   // Headshot bonus
    float armorPenetration = 0.0f;     // 0.0 = none, 1.0 = full

    // Fire
    FireMode fireMode = FireMode::FullAuto;
    float fireRate = 600.0f;           // Rounds per minute
    int burstCount = 3;                // Rounds per burst
    float muzzleVelocity = 900.0f;     // m/s (0 = hitscan)
    bool isHitscan = true;             // Raycast vs ballistic

    // Ammo
    int magazineSize = 30;
    int maxReserveAmmo = 120;
    float reloadTime = 2.5f;           // Seconds
    float tacticalReloadTime = 2.0f;   // With round chambered

    // Handling
    float equipTime = 0.5f;            // Draw time
    float holsterTime = 0.3f;          // Put-away time
    float adsTime = 0.2f;              // ADS transition
    float adsFOV = 50.0f;              // FOV when ADS
    float moveSpeedMultiplier = 1.0f;  // Speed penalty

    // Recoil & Spread
    RecoilPattern recoil;
    SpreadConfig spread;

    // Helpers
    float GetShotInterval() const;     // 60 / fireRate
    float GetDPS() const;              // baseDamage * (fireRate / 60)
};
```

### Recoil Pattern

```cpp
struct RecoilPattern
{
    float verticalPerShot = 0.5f;       // Degrees upward per shot
    float horizontalPerShot = 0.1f;     // Max degrees horizontal
    float recoverySpeed = 5.0f;         // Degrees/sec recovery
    float maxVertical = 10.0f;          // Max accumulated vertical
    float maxHorizontal = 3.0f;         // Max accumulated horizontal
    float firstShotMultiplier = 1.5f;   // First shot bonus
};
```

### Spread Configuration

```cpp
struct SpreadConfig
{
    float baseSpread = 1.0f;            // Standing still (degrees)
    float moveSpread = 3.0f;            // Additional while moving
    float crouchReduction = 0.5f;       // Multiplier when crouching
    float adsReduction = 0.3f;          // Multiplier when ADS
    float sprintPenalty = 2.0f;         // Additional from sprinting
    float maxSpread = 15.0f;            // Absolute maximum
};
```

### Registering Weapons

```cpp
auto& registry = Spark::Gameplay::WeaponRegistry::GetInstance();

// Register default built-in weapons
registry.RegisterDefaults();

// Register a custom weapon
Spark::Gameplay::WeaponDefinition rifle;
rifle.name = "Assault Rifle";
rifle.slot = Spark::Gameplay::WeaponSlot::Primary;
rifle.fireMode = Spark::Gameplay::FireMode::FullAuto;
rifle.fireRate = 700.0f;
rifle.baseDamage = 28.0f;
rifle.magazineSize = 30;
rifle.maxReserveAmmo = 150;
rifle.reloadTime = 2.2f;
rifle.isHitscan = true;
rifle.recoil.verticalPerShot = 0.6f;
rifle.spread.baseSpread = 1.2f;
uint32_t rifleID = registry.RegisterWeapon(rifle);
```

### Equipping Weapons on Entities

```cpp
auto& inv = world.AddComponent<Spark::Gameplay::WeaponInventoryComponent>(player);
auto& primary = inv.weapons[static_cast<size_t>(Spark::Gameplay::WeaponSlot::Primary)];
primary.definitionID = rifleID;
primary.currentAmmo = 30;
primary.reserveAmmo = 120;
primary.state = Spark::Gameplay::WeaponState::Idle;
```

### WeaponFireEvent

When a weapon fires, the `WeaponSystem` emits a `WeaponFireEvent`:

```cpp
struct WeaponFireEvent
{
    uint32_t ownerEntity;    // Entity that fired
    uint32_t weaponDefID;    // Weapon definition ID
    float damage;            // Computed damage
    float spreadAngle;       // Computed spread (degrees)
    bool isHitscan;          // Raycast or ballistic
    float muzzleVelocity;   // Projectile speed
};
```

Subscribe to fire events for projectile spawning, audio, and VFX:

```cpp
weaponSystem.OnFire([&](const Spark::Gameplay::WeaponFireEvent& e) {
    if (e.isHitscan)
        SpawnHitscanTrace(e);
    else
        SpawnProjectile(e);

    audio.PlaySound("gunshot");
    SpawnMuzzleFlash(e.ownerEntity);
});
```

## Projectile System

**Source:** `SparkEngine/Source/Engine/ECS/Components/FPSComponents.h`

The `ProjectileComponent` tracks in-flight projectiles:

### Movement Types

| Type | Description |
|------|-------------|
| `Hitscan` | Instant ray-cast; resolves on the frame it's fired |
| `Ballistic` | Physics-based arc with gravity |

### Impact Behaviors

| Behavior | Description |
|----------|-------------|
| `Destroy` | Destroy on first impact |
| `Bounce` | Ricochet off surfaces |
| `Pierce` | Pass through targets |
| `Stick` | Attach to the hit surface/entity |

### ProjectileComponent Fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `direction` | `XMFLOAT3` | `{0,0,1}` | Travel direction (normalized) |
| `speed` | `float` | `100.0` | Units per second |
| `gravityScale` | `float` | `1.0` | Gravity multiplier |
| `damage` | `float` | `25.0` | Base impact damage |
| `explosionRadius` | `float` | `0.0` | Splash damage radius |
| `maxRange` | `float` | `500.0` | Auto-destroy distance |
| `maxLifetime` | `float` | `10.0` | Auto-destroy time |
| `bouncesRemaining` | `int` | `0` | Ricochet count |
| `piercesRemaining` | `int` | `0` | Penetration count |
| `ownerEntityId` | `uint32_t` | `0` | Owner for friendly fire |
| `teamId` | `int` | `-1` | Team for damage rules |

## Interaction System

**Source:** `SparkEngine/Source/Engine/ECS/Components/FPSComponents.h`

The `InteractionComponent` marks entities as player-interactable:

### Interaction Types

| Type | Description |
|------|-------------|
| `Use` | Press 'E' to activate (doors, switches) |
| `Pickup` | Auto-collect or press to pick up (ammo, health) |
| `Hold` | Hold to interact (defuse, hack, revive) |
| `Toggle` | Toggle on/off (lights, generators) |

### Interaction States

| State | Description |
|-------|-------------|
| `Idle` | Ready for interaction |
| `Active` | Currently being held (Hold type) |
| `Cooldown` | Temporarily unavailable |
| `Disabled` | Cannot be interacted with |
| `Destroyed` | Permanently unusable |

### InteractionComponent Fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `displayName` | `string` | `""` | Name shown in prompt |
| `actionVerb` | `string` | `"Use"` | Verb in prompt ("Open", "Pick up") |
| `interactionRadius` | `float` | `2.5` | Max interaction distance |
| `holdDuration` | `float` | `0.0` | Required hold time |
| `cooldownDuration` | `float` | `0.0` | Post-use cooldown |
| `usesRemaining` | `int` | `-1` | Uses left (-1 = unlimited) |
| `showHighlight` | `bool` | `true` | Show outline when in range |
| `requiredItemId` | `string` | `""` | Required key/item |
| `onInteractEvent` | `string` | `""` | Script event name |

## Decal System

The `DecalComponent` renders projected textures for bullet holes, blood, and scorch marks:

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `texturePath` | `string` | `""` | Decal texture path |
| `category` | `string` | `"generic"` | Grouping category |
| `size` | `XMFLOAT3` | `{0.1, 0.1, 0.05}` | Projection half-extents |
| `color` | `XMFLOAT4` | `{1,1,1,1}` | Tint color (RGBA) |
| `lifetime` | `float` | `30.0` | Seconds before removal (0 = permanent) |
| `fadeOutDuration` | `float` | `2.0` | Fade-out time at end of life |
| `receiveLighting` | `bool` | `true` | Receive scene lighting |
| `sortOrder` | `int` | `0` | Draw order for overlapping decals |

## Class System

A player class system for multiplayer game modes with configurable:
- Starting weapons and equipment
- Health and armor values
- Movement speed modifiers
- Special abilities

## Vehicle System

[Physics](../subsystems/Physics.md)-driven vehicle mechanics:
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

The inventory system is compiled as part of the engine; there is no separate CMake toggle.

Item management system:
- Inventory slots with weight/capacity limits
- Item stacking
- Pickup and drop mechanics
- Equipment slots

```cpp
auto& inv = world.AddComponent<InventoryComponent>(player);
inv.maxSlots  = 20;
inv.maxWeight = 50.0f;

// Add items
inv.AddItem(ItemDef{ "healthpack", "Health Pack", 1.0f, true, 5 });
inv.AddItem(ItemDef{ "ammo_rifle", "Rifle Ammo", 0.1f, true, 60 });

// Check inventory
if (inv.HasItem("healthpack")) {
    inv.RemoveItem("healthpack", 1);
    player.health += 50.0f;
}

// Equipment slots
inv.Equip("primary_weapon", rifleItemId);
inv.Equip("armor", vestItemId);
```

## Quest System

The quest system is compiled as part of the engine; there is no separate CMake toggle.

Objective tracking:
- Quest definitions with stages
- Objective tracking and completion
- Quest log and notifications
- Integration with `QuestCompletedEvent`

```cpp
QuestDefinition quest;
quest.id   = 1;
quest.name = "Clear the Building";
quest.stages = {
    {"Find the entrance",   ObjectiveType::GoToLocation, {10.0f, 0.0f, 20.0f}},
    {"Eliminate all enemies", ObjectiveType::KillCount, 5},
    {"Collect the intel",    ObjectiveType::CollectItem, "intel_docs"},
    {"Return to base",      ObjectiveType::GoToLocation, {0.0f, 0.0f, 0.0f}}
};
questSystem.RegisterQuest(quest);
questSystem.StartQuest(1);

// Update progress from event callbacks
bus.Subscribe<EntityKilledEvent>([&](const EntityKilledEvent& e) {
    questSystem.UpdateKillCount(e.killerId);
});

if (questSystem.IsQuestComplete(1)) {
    bus.Publish(QuestCompletedEvent{ playerId, 1, "Clear the Building" });
}
```

## Game Mode Management

Configurable game modes for multiplayer:
- Deathmatch
- Team Deathmatch
- Capture the Flag
- Domination
- Search and Destroy
- Free for All
- Custom modes

See [Dedicated Server](../subsystems/Dedicated-Server.md) for `GameModeType` enum details.

## HealthComponent

Core gameplay component for damageable entities:

```cpp
auto& hp = world.AddComponent<HealthComponent>(entity);
hp.maxHealth      = 100.0f;
hp.health         = 100.0f;
hp.isDead         = false;
hp.isInvulnerable = false;
```

Damage events are published through the [event bus](../subsystems/Event-System.md):

```cpp
bus.Publish(EntityDamagedEvent{ entityId, damage, "Weapon" });
```

## Gravity System

Configurable gravity for physics-driven gameplay:
- Per-scene gravity settings (via `SceneMetadata`, see [Scene Management](../subsystems/Scene-Management.md))
- Gravity zones for localized effects

## Interactive Objects

Objects that respond to player interaction:
- Doors (open/close)
- Buttons and switches
- Pickup items

```cpp
auto& interact = world.AddComponent<InteractionComponent>(doorEntity);
interact.type = InteractionComponent::InteractionType::Use;
interact.displayName = "Door";
interact.actionVerb = "Open";
interact.interactionRadius = 2.5f;
interact.onInteractEvent = "OpenDoor";
```

---

## See Also

- [Entity Component System](../subsystems/Entity-Component-System.md) -- Gameplay components
- [Physics](../subsystems/Physics.md) -- Physics-driven gameplay
- [Event System](../subsystems/Event-System.md) -- Gameplay events
- [AI and Navigation](../subsystems/AI-and-Navigation.md) -- NPC behavior
- [Networking](../subsystems/Networking.md) -- Multiplayer game modes
- [Dedicated Server](../subsystems/Dedicated-Server.md) -- Server game modes
- [Scene Management](../subsystems/Scene-Management.md) -- Scene gravity and level loading
- [Scripting with AngelScript](../subsystems/Scripting-with-AngelScript.md) -- Scripting gameplay logic
- [Input System](../subsystems/Input-System.md) -- Player input handling
- [Audio](../subsystems/Audio.md) -- Gameplay sound effects
- [Rendering and Graphics](../subsystems/Rendering-and-Graphics.md) -- HUD and visual effects
- [Animation](../subsystems/Animation.md) -- Player and weapon animations
- [Terrain and Procedural Generation](Terrain-and-Procedural-Generation.md) -- Level environments
