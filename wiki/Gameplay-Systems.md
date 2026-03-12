# Gameplay Systems

SparkEngine includes a comprehensive set of FPS gameplay systems built on top of the [Entity Component System](Entity-Component-System) architecture.

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

```cpp
// Set up a weapon component on the player
auto& weapon = world.AddComponent<WeaponComponent>(player);
weapon.type         = ProjectileType::Bullet;
weapon.fireRate     = 10.0f;   // rounds per second
weapon.damage       = 25.0f;
weapon.spread       = 0.02f;   // radians
weapon.recoilForce  = 1.5f;
weapon.maxAmmo      = 30;
weapon.currentAmmo  = 30;
weapon.reserveAmmo  = 120;
weapon.reloadTime   = 2.0f;    // seconds

// Fire the weapon (typically in response to input)
if (input.IsMouseButtonDown(0) && weapon.CanFire()) {
    weapon.Fire(cameraPosition, cameraForward);
    bus.Publish(SoundPlayedEvent{ "gunshot", cameraPosition });
}

// Reload
if (input.WasKeyPressed('R') && weapon.currentAmmo < weapon.maxAmmo) {
    weapon.StartReload();
}
```

## Class System

A player class system for multiplayer game modes with configurable:
- Starting weapons and equipment
- Health and armor values
- Movement speed modifiers
- Special abilities

## Vehicle System

[Physics](Physics)-driven vehicle mechanics:
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

```cpp
// Add inventory to a player entity
auto& inv = world.AddComponent<InventoryComponent>(player);
inv.maxSlots  = 20;
inv.maxWeight = 50.0f;

// Add items
inv.AddItem(ItemDef{ "healthpack", "Health Pack", 1.0f, true, 5 });  // stackable, max 5
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

`ENABLE_QUEST_SYSTEM=ON`

Objective tracking:
- Quest definitions with stages
- Objective tracking and completion
- Quest log and notifications
- Integration with `QuestCompletedEvent`

```cpp
// Define a quest with stages
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

// Start the quest
questSystem.StartQuest(1);

// Update progress (from event callbacks)
bus.Subscribe<EntityKilledEvent>([&](const EntityKilledEvent& e) {
    questSystem.UpdateKillCount(e.killerId);
});

// Check completion
if (questSystem.IsQuestComplete(1)) {
    bus.Publish(QuestCompletedEvent{ playerId, 1, "Clear the Building" });
}
```

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

Damage events are published through the [event bus](Event-System):

```cpp
bus.Publish(EntityDamagedEvent{ entityId, damage, "Weapon" });
```

## Gravity System

Configurable gravity for physics-driven gameplay:
- Per-scene gravity settings (via `SceneMetadata`, see [Scene Management](Scene-Management))
- Gravity zones for localized effects

## Interactive Objects

Objects that respond to player interaction:
- Doors (open/close)
- Buttons and switches
- Pickup items

```cpp
// Set up an interactive door
auto& interactable = world.AddComponent<InteractableComponent>(doorEntity);
interactable.interactPrompt = "Press E to open";
interactable.interactRange  = 2.5f;

interactable.onInteract = [&](EntityID player) {
    auto& door = world.GetComponent<DoorComponent>(doorEntity);
    door.isOpen = !door.isOpen;
    audio.PlaySound(door.isOpen ? "door_open" : "door_close");
};

// Check for interaction each frame
if (input.WasKeyPressed('E')) {
    for (auto [entity, interact, tf] :
         world.GetEntitiesWith<InteractableComponent, Transform>().each())
    {
        float dist = Distance(playerPos, tf.position);
        if (dist <= interact.interactRange) {
            interact.onInteract(playerId);
            break;
        }
    }
}
```

---

## See Also

- [Entity Component System](Entity-Component-System) — Gameplay components
- [Physics](Physics) — Physics-driven gameplay
- [Event System](Event-System) — Gameplay events
- [AI and Navigation](AI-and-Navigation) — NPC behavior
- [Networking](Networking) — Multiplayer game modes
- [Scene Management](Scene-Management) — Scene gravity and level loading
- [Scripting with AngelScript](Scripting-with-AngelScript) — Scripting gameplay logic
- [Input System](Input-System) — Player input handling
- [Audio](Audio) — Gameplay sound effects
- [Rendering and Graphics](Rendering-and-Graphics) — HUD and visual effects
- [Animation](Animation) — Player and weapon animations
- [Terrain and Procedural Generation](Terrain-and-Procedural-Generation) — Level environments
