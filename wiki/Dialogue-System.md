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

## Building a Dialogue Tree in Code

```cpp
// Create a dialogue tree programmatically (instead of loading from JSON)
DialogueTree tree;
tree.SetId("shopkeeper");
tree.SetStartNodeId("greeting");

// Greeting node
DialogueNode greeting;
greeting.id          = "greeting";
greeting.type        = DialogueNodeType::Text;
greeting.speakerName = "Shopkeeper";
greeting.text        = "Welcome, traveler! What can I do for you?";
greeting.nextNodeId  = "main_choice";
tree.AddNode(greeting);

// Choice node
DialogueNode mainChoice;
mainChoice.id   = "main_choice";
mainChoice.type = DialogueNodeType::Choice;
mainChoice.choices = {
    {"I'd like to buy supplies.",   "", "buy_response",   "", false},
    {"Any news from the front?",    "", "news_response",  "", false},
    {"Goodbye.",                    "", "farewell",       "", false}
};
tree.AddNode(mainChoice);

// Conditional branch — checks if player has enough gold
DialogueNode buyResponse;
buyResponse.id          = "buy_response";
buyResponse.type        = DialogueNodeType::Branch;
buyResponse.condition   = "hasGold:100";
buyResponse.trueNodeId  = "can_buy";
buyResponse.falseNodeId = "no_gold";
tree.AddNode(buyResponse);

// Event node — gives an item
DialogueNode canBuy;
canBuy.id        = "can_buy";
canBuy.type      = DialogueNodeType::Event;
canBuy.eventName = "give_item";
canBuy.eventData = "health_potion";
canBuy.nextNodeId = "buy_thanks";
tree.AddNode(canBuy);

// End node
DialogueNode farewell;
farewell.id          = "farewell";
farewell.type        = DialogueNodeType::End;
farewell.speakerName = "Shopkeeper";
farewell.text        = "Safe travels!";
tree.AddNode(farewell);

// Register and start
dialogue.RegisterTree("shopkeeper", tree);
dialogue.StartConversation("shopkeeper");
```

## Dialogue JSON File Format

```json
{
    "id": "guard_talk",
    "startNode": "greeting",
    "nodes": [
        {
            "id": "greeting",
            "type": "text",
            "speaker": "Guard",
            "text": "Halt! State your business.",
            "voiceClip": "Audio/Dialogue/guard_greeting.wav",
            "nextNode": "player_choice"
        },
        {
            "id": "player_choice",
            "type": "choice",
            "choices": [
                { "text": "I'm here to see the captain.", "nextNode": "captain_check" },
                { "text": "Just passing through.", "nextNode": "pass_through" }
            ]
        },
        {
            "id": "captain_check",
            "type": "branch",
            "condition": "hasItem:captain_letter",
            "trueNode": "allowed",
            "falseNode": "denied"
        }
    ]
}
```

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
