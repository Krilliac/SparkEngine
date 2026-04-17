# Event Response System

Data-driven **When / If / Then** rule engine for no-code gameplay logic.

The Event Response System lets designers and modders assemble gameplay rules out of composable trigger → condition → action tuples, defined in JSON and evaluated at runtime against the live [EventBus](Event-System.md). No scripting required.

- **Header:** `SparkEngine/Source/Engine/Gameplay/EventResponseSystem.h`
- **Impl:** `SparkEngine/Source/Engine/Gameplay/EventResponseSystem.cpp` (~870 LOC)
- **Editor panel:** `SparkEditor/Source/Panels/EventResponsePanel.{h,cpp}`
- **Tests:** `Tests/TestEventResponseSystem.cpp` (15 cases)

---

## Rule model

Every rule has the same three pieces:

```
WHEN  [Event fires]    ← EventTriggerType (OnTriggerEnter, OnDamaged, OnTimer, …)
  IF  [Conditions pass]  ← ConditionSystem predicates
THEN  [Actions execute]  ← ActionType list (SpawnEntity, PlaySound, ShowUI, …)
```

`EventResponseRule` is a plain struct — name, trigger, trigger parameter, a list of conditions, and a list of actions — that serializes directly to JSON.

### Triggers (`EventTriggerType`)

| Trigger | Fires on | `triggerParam` |
|---------|----------|----------------|
| `OnTriggerEnter` / `OnTriggerExit` | Entity enters/exits trigger volume | — |
| `OnDamaged` / `OnKilled` | Entity took damage / died | — |
| `OnItemPickup` | Item picked up | — |
| `OnKeyPress` / `OnKeyRelease` | Input action | Action name |
| `OnCollision` | Physics collision | — |
| `OnQuestComplete` | Quest completed | — |
| `OnTimer` | Repeating timer | Interval in seconds |
| `OnStart` | Once at rule load | — |
| `OnWeatherChange` / `OnTimeOfDay` | Weather / time-of-day changed | — |
| `OnEntityCreated` / `OnEntityDestroyed` | Entity lifecycle | — |
| `OnCustom` | `FireCustomEvent(name)` | Event name |

### Actions (`ActionType`)

Grouped by effect:

- **Entity lifecycle** — `SpawnEntity`, `DestroyEntity`, `EnableEntity`, `DisableEntity`
- **Transform** — `SetPosition`, `MoveToward`, `TeleportEntity`
- **Audio / VFX** — `PlaySound`, `PlayAnimation`, `SpawnEffect`
- **Gameplay state** — `SetVariable`, `AddVariable`, `GiveItem`, `TakeItem`, `GiveXP`
- **UI / dialogue** — `ShowUI`, `HideUI`, `ShowDialogue`, `ShowSubtitle`
- **Scripting bridge** — `CallScript`, `FireCustomEvent`
- **Flow control** — `Delay` (defers remaining actions for N seconds)

Each action carries a small `params` array of typed values (string / int / float / bool).

### Conditions

Conditions reuse the [ConditionSystem](../gameplay-tools/Gameplay-Systems.md) — the same predicate engine used by quests, abilities, and dialogue. That gives rules access to entity tags, variable comparisons, inventory checks, and quest-state checks without duplicating logic.

---

## Runtime lifecycle

```cpp
auto& ers = Spark::Gameplay::EventResponseSystem::GetInstance();
ers.Initialize();   // subscribes to all relevant EventBus events

// ...game loop...
ers.Update(dt);     // ticks OnTimer rules and flushes delayed actions

ers.Shutdown();     // unsubscribes
```

- `Initialize()` takes a snapshot subscription to every `EventBus` event the rule engine cares about.
- `Update(dt)` advances timer-based rules and releases any actions queued behind a `Delay`.
- `Shutdown()` releases all `SubscriptionHandle` instances so there are no dangling callbacks.

### Authoring at runtime

```cpp
EventResponseRule rule;
rule.name    = "Door Open";
rule.trigger = EventTriggerType::OnTriggerEnter;
rule.actions.push_back({ActionType::PlayAnimation, {"door_open"}});
ers.AddRule(std::move(rule));
```

`AddRule`, `RemoveRule`, `SetRuleEnabled`, and `ClearRules` mutate the live ruleset; rules are evaluated in insertion order.

### Custom events

`ers.FireCustomEvent("player_reached_checkpoint", playerId)` fires a matched `OnCustom` rule. This is the extension point for gameplay systems that want to participate without adding new `EventTriggerType` values.

---

## Serialization

Rules round-trip to disk as JSON:

```cpp
ers.SaveToJson("Content/Rules/level1.json");
ers.LoadFromJson("Content/Rules/level1.json");
```

The JSON format mirrors the `EventResponseRule` fields one-to-one, which makes it friendly for external tooling and diff review.

---

## Editor integration

`EventResponsePanel` exposes the full rule table in SparkEditor: add / remove rules, edit triggers and actions via dropdowns, enable-disable rules live, and hot-reload the JSON file. See [SparkEditor](../gameplay-tools/SparkEditor.md) for how the panel is registered.

---

## When to use this vs scripting

| Use Event Response System | Use [AngelScript](Scripting-with-AngelScript.md) |
|---------------------------|-------------------------------------------------|
| Designer-authored gameplay rules | Complex algorithms or state machines |
| JSON-authorable, no code review | Logic shared between client/server |
| Hot-editable in the editor | Performance-critical per-frame work |
| Fan-made mods via JSON | Full engine API access |

The two systems compose — an action can call a script, and a script can fire a custom event that triggers further rules.

---

## Related systems

- [Event System](Event-System.md) — underlying EventBus that this subsystem consumes.
- [Gameplay Systems](../gameplay-tools/Gameplay-Systems.md) — ConditionSystem, abilities, and quests all feed into conditions.
- [Visual Scripting](Visual-Scripting.md) — graph-based alternative for richer logic.
- [Dialogue System](Dialogue-System.md) — can emit custom events that rules react to.
