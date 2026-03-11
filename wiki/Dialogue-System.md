# Dialogue System

SparkEngine provides a branching dialogue system for story-driven FPS games. It supports dialogue trees loaded from JSON, player choices with conditions, NPC responses, event triggers, and integration with the localization system.

**Source:** `SparkEngine/Source/Engine/Dialogue/DialogueSystem.h`

## Overview

| Class | Responsibility |
|-------|---------------|
| `DialogueSystem` | Manages trees, active conversations, condition evaluation |
| `DialogueTree` | Graph of `DialogueNode` instances loaded from JSON |
| `DialogueNode` | A single node: text, choice, branch, event, or end |
| `ConversationState` | Runtime state of an active conversation |

## Node Types

```cpp
enum class DialogueNodeType {
    Text,   // NPC speaks text
    Choice, // Player makes a choice
    Branch, // Conditional branch (checks game state)
    Event,  // Fires a game event
    End     // End of conversation
};
```

## Quick Start

```cpp
DialogueSystem dialogue;
dialogue.LoadTree("guard_talk", "Data/Dialogue/guard.json");

dialogue.StartConversation("guard_talk");

// Each frame while conversation is active:
dialogue.Update(deltaTime);
auto* node = dialogue.GetCurrentNode();
// Display node->text and node->speakerName in UI

// When player selects a choice:
dialogue.SelectChoice(0);

// For text-only nodes (no choices):
dialogue.AdvanceNode();
```

## Conditions and Variables

Register condition evaluators so dialogue branches can check game state:

```cpp
dialogue.RegisterCondition("hasItem", [&](const std::string& param) {
    return inventory.HasItem(param);
});

dialogue.RegisterCondition("questComplete", [&](const std::string& param) {
    return questLog.IsComplete(param);
});

// Set conversation-local variables
dialogue.SetVariable("mood", "friendly");
```

## Event Callbacks

Dialogue Event nodes fire gameplay events:

```cpp
dialogue.OnDialogueEvent([](const std::string& eventName, const std::string& data) {
    if (eventName == "give_item") { inventory.AddItem(data); }
});

dialogue.OnConversationEnded([](const std::string& treeId) {
    // Resume gameplay
});
```

## DialogueNode Fields

| Field | Description |
|-------|-------------|
| `speakerName` | Who is speaking |
| `text` | Dialogue text to display |
| `localizationKey` | Localization key for translated text |
| `voiceClip` | Path to voice audio clip |
| `displayDuration` | Auto-advance time (0 = wait for input) |
| `choices` | Player-selectable choices (for Choice nodes) |
| `animation` | NPC animation to play |
| `cameraAngle` | Camera preset name |

## Console Commands

```
dialogue_status     # Show dialogue system status
dialogue_trees      # List loaded dialogue trees
```

---

## See Also

- [Localization](Localization) — Translated dialogue text
- [UI System](UI-System) — Displaying dialogue in game UI
- [AI and Navigation](AI-and-Navigation) — NPC behavior during conversations
- [Audio](Audio) — Voice clip playback
- [Event System](Event-System) — Dialogue-triggered game events
