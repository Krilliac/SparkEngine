# Dialogue System

SparkEngine provides a branching dialogue system for story-driven FPS games. It supports dialogue trees loaded from JSON, player choices with conditions, NPC responses, event triggers, and integration with the localization system.

**Source:** `SparkEngine/Source/Engine/Dialogue/DialogueSystem.h`
**Namespace:** `Spark`

## Overview

| Class | Responsibility |
|-------|---------------|
| `DialogueSystem` | Manages trees, active conversations, condition evaluation |
| `DialogueTree` | Graph of `DialogueNode` instances loaded from JSON |
| `DialogueNode` | A single node: text, choice, branch, event, or end |
| `DialogueChoice` | A player-selectable choice with optional condition gating |
| `ConversationState` | Runtime state of an active conversation |

## Architecture

```
+------------------------------------------------------------------+
|                       DialogueSystem                              |
|                                                                   |
|  m_trees : unordered_map<string, unique_ptr<DialogueTree>>       |
|  m_state : ConversationState                                      |
|  m_conditionEvaluators : unordered_map<string, function>         |
|  m_eventCallbacks : vector<function>                              |
|  m_endCallbacks   : vector<function>                              |
|                                                                   |
|  LoadTree(treeId, filePath)                                       |
|  RegisterTree(treeId, tree)                                       |
|  StartConversation(treeId)                                        |
|  EndConversation()                                                |
|  Update(deltaTime) ──────────┐                                    |
|  SelectChoice(index)         │                                    |
|  AdvanceNode()               │                                    |
|                              v                                    |
|                      ProcessNode(node)                            |
|                        ├── Text  → display text, wait/auto-adv   |
|                        ├── Choice → wait for player input         |
|                        ├── Branch → EvaluateCondition() → jump    |
|                        ├── Event  → fire callbacks                |
|                        └── End    → EndConversation()             |
+------------------------------------------------------------------+
```

### Data Flow

```
JSON File ──LoadFromFile()──> DialogueTree ──RegisterTree()──> DialogueSystem
                                                                   │
                                                          StartConversation()
                                                                   │
                                                                   v
                                                          ConversationState
                                                          (treeId, currentNodeId,
                                                           nodeTimer, variables)
                                                                   │
                                                              Update(dt)
                                                                   │
                                                     ┌─────────────┼───────────┐
                                                     v             v           v
                                                 UI Display   Event Fire   Condition
                                                 (text,       (give_item,  Evaluation
                                                  choices)     set_flag)   (hasItem?)
```

## Node Types

```cpp
enum class DialogueNodeType
{
    Text,   // NPC speaks text — displays speakerName + text
    Choice, // Player makes a choice from a list of options
    Branch, // Conditional branch — evaluates a condition, jumps to trueNodeId or falseNodeId
    Event,  // Fires a game event via registered callbacks
    End     // End of conversation — triggers OnConversationEnded callbacks
};
```

| Node Type | Required Fields | Optional Fields |
|-----------|----------------|-----------------|
| `Text` | `id`, `text` | `speakerName`, `localizationKey`, `voiceClip`, `displayDuration`, `nextNodeId`, `animation`, `cameraAngle` |
| `Choice` | `id`, `choices` | `speakerName`, `text` (prompt text above choices) |
| `Branch` | `id`, `condition`, `trueNodeId`, `falseNodeId` | none |
| `Event` | `id`, `eventName` | `eventData`, `nextNodeId` |
| `End` | `id` | `speakerName`, `text` (farewell line) |

## DialogueChoice Struct

```cpp
struct DialogueChoice
{
    std::string text;            // Display text for the choice
    std::string localizationKey; // Localization key (if using L10N)
    std::string nextNodeId;      // Node to jump to if selected
    std::string condition;       // Condition expression (empty = always available)
    bool visited = false;        // Whether this choice was previously selected
};
```

Choices with a non-empty `condition` field are evaluated at display time. If the condition returns `false`, the choice is filtered out of the list returned by `GetAvailableChoices()`. The `visited` flag is set to `true` after the player selects a choice, which can be used by the UI to gray out previously explored options.

## DialogueNode Fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `id` | `std::string` | `""` | Unique node identifier within the tree |
| `type` | `DialogueNodeType` | `Text` | Node behavior type |
| `speakerName` | `std::string` | `""` | Who is speaking (NPC name) |
| `text` | `std::string` | `""` | Dialogue text to display |
| `localizationKey` | `std::string` | `""` | Localization key for translated text |
| `voiceClip` | `std::string` | `""` | Path to voice audio clip |
| `displayDuration` | `float` | `3.0f` | Auto-advance time in seconds (0 = wait for input) |
| `choices` | `vector<DialogueChoice>` | `{}` | Player-selectable choices (for Choice nodes) |
| `condition` | `std::string` | `""` | Condition expression (for Branch nodes) |
| `trueNodeId` | `std::string` | `""` | Node if condition is true (Branch) |
| `falseNodeId` | `std::string` | `""` | Node if condition is false (Branch) |
| `eventName` | `std::string` | `""` | Event to fire (Event nodes) |
| `eventData` | `std::string` | `""` | Data payload for the event |
| `nextNodeId` | `std::string` | `""` | Next node for linear flow |
| `animation` | `std::string` | `""` | NPC animation to play during this node |
| `cameraAngle` | `std::string` | `""` | Camera preset name |

## ConversationState

The runtime state tracking an active conversation:

```cpp
struct ConversationState
{
    std::string treeId;                                     // Active dialogue tree
    std::string currentNodeId;                              // Current node being displayed
    float nodeTimer = 0.0f;                                 // Time spent on current node
    bool waitingForInput = false;                           // Waiting for player choice
    bool isActive = false;                                  // Whether conversation is active
    std::unordered_map<std::string, std::string> variables; // Conversation-local variables
};
```

Variables stored in `ConversationState::variables` are scoped to the active conversation and cleared when the conversation ends. Use `SetVariable()` / `GetVariable()` on `DialogueSystem` to read and write them.

## DialogueTree Class

```cpp
class DialogueTree
{
public:
    void SetId(const std::string& id);
    const std::string& GetId() const;

    void SetStartNodeId(const std::string& nodeId);
    const std::string& GetStartNodeId() const;

    void AddNode(const DialogueNode& node);
    const DialogueNode* GetNode(const std::string& nodeId) const;
    std::vector<std::string> GetNodeIds() const;
    size_t GetNodeCount() const;

    bool LoadFromFile(const std::string& filePath);
};
```

## DialogueSystem API Reference

### Construction and Lifecycle

```cpp
DialogueSystem();
~DialogueSystem() = default;
```

### Tree Management

| Method | Signature | Description |
|--------|-----------|-------------|
| `LoadTree` | `bool LoadTree(const std::string& treeId, const std::string& filePath)` | Load a dialogue tree from a JSON file |
| `RegisterTree` | `void RegisterTree(const std::string& treeId, std::unique_ptr<DialogueTree> tree)` | Register a dialogue tree directly (takes ownership) |

### Conversation Control

| Method | Signature | Description |
|--------|-----------|-------------|
| `StartConversation` | `bool StartConversation(const std::string& treeId)` | Start a conversation; returns false if tree not found |
| `EndConversation` | `void EndConversation()` | End the current conversation and fire end callbacks |
| `Update` | `void Update(float deltaTime)` | Advance timers and auto-advance text nodes |
| `SelectChoice` | `bool SelectChoice(size_t choiceIndex)` | Select a choice (0-based); returns false if invalid |
| `AdvanceNode` | `void AdvanceNode()` | Advance to next node (for text nodes without choices) |

### Query Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `IsConversationActive` | `bool IsConversationActive() const` | Check if a conversation is currently running |
| `GetCurrentNode` | `const DialogueNode* GetCurrentNode() const` | Get the current dialogue node (nullptr if inactive) |
| `GetState` | `const ConversationState& GetState() const` | Get the full conversation state |
| `GetAvailableChoices` | `std::vector<DialogueChoice> GetAvailableChoices() const` | Get choices filtered by conditions |

### Condition System

| Method | Signature | Description |
|--------|-----------|-------------|
| `RegisterCondition` | `void RegisterCondition(const std::string& name, std::function<bool(const std::string&)> evaluator)` | Register a named condition evaluator |
| `SetVariable` | `void SetVariable(const std::string& name, const std::string& value)` | Set a conversation-local variable |
| `GetVariable` | `std::string GetVariable(const std::string& name) const` | Get a variable value (empty if unset) |

### Callbacks

| Method | Signature | Description |
|--------|-----------|-------------|
| `OnDialogueEvent` | `void OnDialogueEvent(std::function<void(const std::string&, const std::string&)> callback)` | Register event callback (eventName, eventData) |
| `OnConversationEnded` | `void OnConversationEnded(std::function<void(const std::string&)> callback)` | Register end callback (treeId) |

### Console Integration

| Method | Signature | Description |
|--------|-----------|-------------|
| `Console_GetStatus` | `std::string Console_GetStatus() const` | Get dialogue system status for console display |
| `Console_ListTrees` | `std::string Console_ListTrees() const` | List all loaded dialogue trees |

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

dialogue.RegisterCondition("hasGold", [&](const std::string& param) {
    int required = std::stoi(param);
    return player.GetGold() >= required;
});

dialogue.RegisterCondition("factionRep", [&](const std::string& param) {
    // param format: "factionName:minRep"
    auto sep = param.find(':');
    auto faction = param.substr(0, sep);
    int minRep = std::stoi(param.substr(sep + 1));
    return factionSystem.GetReputation(faction) >= minRep;
});

// Set conversation-local variables
dialogue.SetVariable("mood", "friendly");
dialogue.SetVariable("visitCount", "3");
```

### Condition Expression Format

Conditions are formatted as `conditionName:parameter`. The system splits on the first colon, looks up the condition evaluator by name, and passes the parameter string to it.

| Example Condition | Evaluator | Parameter |
|-------------------|-----------|-----------|
| `hasItem:captain_letter` | `hasItem` | `captain_letter` |
| `questComplete:main_quest_01` | `questComplete` | `main_quest_01` |
| `hasGold:100` | `hasGold` | `100` |
| `factionRep:guard:50` | `factionRep` | `guard:50` |

## Event Callbacks

Dialogue Event nodes fire gameplay events through registered callbacks:

```cpp
dialogue.OnDialogueEvent([&](const std::string& eventName, const std::string& data) {
    if (eventName == "give_item")
    {
        inventory.AddItem(data);
    }
    else if (eventName == "set_quest")
    {
        questLog.StartQuest(data);
    }
    else if (eventName == "play_animation")
    {
        animationSystem.Play(data);
    }
    else if (eventName == "change_faction_rep")
    {
        // data format: "factionName:amount"
        auto sep = data.find(':');
        factionSystem.ModifyReputation(data.substr(0, sep), std::stoi(data.substr(sep + 1)));
    }
});

dialogue.OnConversationEnded([&](const std::string& treeId) {
    // Resume gameplay, unlock player movement
    playerController.SetMovementEnabled(true);
    inputSystem.SetMouseLookEnabled(true);
});
```

## Building a Dialogue Tree in Code

```cpp
// Create a dialogue tree programmatically (instead of loading from JSON)
auto tree = std::make_unique<DialogueTree>();
tree->SetId("shopkeeper");
tree->SetStartNodeId("greeting");

// Greeting node
DialogueNode greeting;
greeting.id          = "greeting";
greeting.type        = DialogueNodeType::Text;
greeting.speakerName = "Shopkeeper";
greeting.text        = "Welcome, traveler! What can I do for you?";
greeting.nextNodeId  = "main_choice";
tree->AddNode(greeting);

// Choice node
DialogueNode mainChoice;
mainChoice.id   = "main_choice";
mainChoice.type = DialogueNodeType::Choice;
mainChoice.choices = {
    {"I'd like to buy supplies.",   "", "buy_response",   "", false},
    {"Any news from the front?",    "", "news_response",  "", false},
    {"Goodbye.",                    "", "farewell",       "", false}
};
tree->AddNode(mainChoice);

// Conditional branch -- checks if player has enough gold
DialogueNode buyResponse;
buyResponse.id          = "buy_response";
buyResponse.type        = DialogueNodeType::Branch;
buyResponse.condition   = "hasGold:100";
buyResponse.trueNodeId  = "can_buy";
buyResponse.falseNodeId = "no_gold";
tree->AddNode(buyResponse);

// Event node -- gives an item
DialogueNode canBuy;
canBuy.id        = "can_buy";
canBuy.type      = DialogueNodeType::Event;
canBuy.eventName = "give_item";
canBuy.eventData = "health_potion";
canBuy.nextNodeId = "buy_thanks";
tree->AddNode(canBuy);

// End node
DialogueNode farewell;
farewell.id          = "farewell";
farewell.type        = DialogueNodeType::End;
farewell.speakerName = "Shopkeeper";
farewell.text        = "Safe travels!";
tree->AddNode(farewell);

// Register and start (note: RegisterTree takes unique_ptr)
dialogue.RegisterTree("shopkeeper", std::move(tree));
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
            "animation": "guard_alert",
            "cameraAngle": "closeup",
            "displayDuration": 0,
            "nextNode": "player_choice"
        },
        {
            "id": "player_choice",
            "type": "choice",
            "choices": [
                {
                    "text": "I'm here to see the captain.",
                    "nextNode": "captain_check"
                },
                {
                    "text": "Just passing through.",
                    "nextNode": "pass_through"
                },
                {
                    "text": "[Bribe] Here's 50 gold for your trouble.",
                    "condition": "hasGold:50",
                    "nextNode": "bribe_success"
                }
            ]
        },
        {
            "id": "captain_check",
            "type": "branch",
            "condition": "hasItem:captain_letter",
            "trueNode": "allowed",
            "falseNode": "denied"
        },
        {
            "id": "allowed",
            "type": "event",
            "eventName": "set_quest",
            "eventData": "meet_captain",
            "nextNode": "allowed_text"
        },
        {
            "id": "allowed_text",
            "type": "text",
            "speaker": "Guard",
            "text": "Ah, you have the captain's letter. Go on through.",
            "nextNode": "end_allowed"
        },
        {
            "id": "end_allowed",
            "type": "end"
        },
        {
            "id": "denied",
            "type": "text",
            "speaker": "Guard",
            "text": "No letter, no entry. Move along.",
            "nextNode": "end_denied"
        },
        {
            "id": "end_denied",
            "type": "end"
        }
    ]
}
```

### JSON Field Mapping

| JSON Field | Struct Field | Notes |
|------------|-------------|-------|
| `id` | `DialogueNode::id` | Required for all nodes |
| `type` | `DialogueNode::type` | `"text"`, `"choice"`, `"branch"`, `"event"`, `"end"` |
| `speaker` | `DialogueNode::speakerName` | Optional |
| `text` | `DialogueNode::text` | Required for text/end nodes |
| `voiceClip` | `DialogueNode::voiceClip` | Optional audio path |
| `displayDuration` | `DialogueNode::displayDuration` | `0` = wait for input |
| `animation` | `DialogueNode::animation` | Optional NPC animation |
| `cameraAngle` | `DialogueNode::cameraAngle` | Optional camera preset |
| `nextNode` | `DialogueNode::nextNodeId` | Next node for linear flow |
| `condition` | `DialogueNode::condition` | For branch and choice conditions |
| `trueNode` | `DialogueNode::trueNodeId` | Branch: condition true |
| `falseNode` | `DialogueNode::falseNodeId` | Branch: condition false |
| `eventName` | `DialogueNode::eventName` | Event node: event to fire |
| `eventData` | `DialogueNode::eventData` | Event node: data payload |
| `choices` | `DialogueNode::choices` | Array of choice objects |
| `localizationKey` | `DialogueNode::localizationKey` | For localized text |

## Internal Implementation

### Node Processing Flow

When `Update(deltaTime)` is called, the system:

1. Checks if a conversation is active (`m_state.isActive`).
2. Increments `m_state.nodeTimer` by `deltaTime`.
3. Calls `ProcessNode()` on the current node, which performs type-specific logic:
   - **Text**: If `displayDuration > 0` and timer exceeds it, auto-advances to `nextNodeId`. If `displayDuration == 0`, sets `waitingForInput = true`.
   - **Choice**: Sets `waitingForInput = true` and waits for `SelectChoice()`.
   - **Branch**: Calls `EvaluateCondition()` and immediately jumps to `trueNodeId` or `falseNodeId`.
   - **Event**: Fires all registered event callbacks with `(eventName, eventData)` and immediately advances to `nextNodeId`.
   - **End**: Calls `EndConversation()`, firing all end callbacks.

### Condition Evaluation

`EvaluateCondition(condition)` splits the condition string on the first `:` separator to extract a condition name and parameter. It looks up the name in `m_conditionEvaluators` and calls the registered function with the parameter. Returns `false` if no evaluator is registered for the given name.

### Memory Management

- `DialogueTree` instances are stored as `std::unique_ptr` in the system's `m_trees` map.
- `DialogueNode` instances are stored by value inside each `DialogueTree`'s internal `unordered_map`.
- `ConversationState` is a plain struct stored by value; it is reset when a new conversation starts.

## Error Handling

| Scenario | Behavior |
|----------|----------|
| `LoadTree` with invalid file path | Returns `false`, logs error |
| `StartConversation` with unknown tree ID | Returns `false`, no state change |
| `SelectChoice` with out-of-range index | Returns `false`, no state change |
| `AdvanceNode` on a Choice node | No effect (must use `SelectChoice`) |
| `GetCurrentNode` when no conversation active | Returns `nullptr` |
| Branch node references unknown node ID | Conversation ends, logs error |
| Condition evaluator not registered | `EvaluateCondition` returns `false` |
| Multiple simultaneous conversations | Not supported; starting a new one ends the current |

## Performance

- Dialogue trees are loaded once and cached by ID. Repeated `LoadTree` calls with the same ID overwrite the previous tree.
- `ProcessNode` for Branch and Event nodes executes immediately (zero-frame), so chains of Branch/Event nodes resolve in a single `Update()` call.
- Condition evaluation is synchronous. Avoid expensive operations in condition evaluators (e.g., full inventory searches). Cache results if necessary.
- `GetAvailableChoices()` evaluates conditions for each choice every time it is called. Call it once per frame and cache the result.

## Thread Safety

The `DialogueSystem` is **not thread-safe**. All method calls must occur on the main thread. This includes:
- Loading and registering trees
- Starting, updating, and ending conversations
- Registering conditions and callbacks
- Setting and getting variables

If dialogue data needs to be accessed from a background thread (e.g., for preloading voice clips), copy the data out of the system first and operate on the copy.

## Console Commands

```
dialogue_status     # Show dialogue system status (active tree, current node, variables)
dialogue_trees      # List all loaded dialogue trees with node counts
```

## Troubleshooting

| Problem | Cause | Solution |
|---------|-------|----------|
| Conversation does not start | Tree ID not loaded | Verify `LoadTree` returned `true`; check file path |
| Choices not appearing | All choices have failing conditions | Check condition evaluators; use `GetAvailableChoices()` |
| Node stuck, not advancing | `displayDuration` is 0 and no input handler | Call `AdvanceNode()` or `SelectChoice()` |
| Event not firing | No event callback registered | Register with `OnDialogueEvent()` before starting |
| Branch always goes to false | Condition evaluator not registered | Register condition before starting conversation |
| Voice clip not playing | Audio system not integrated | Connect `voiceClip` field to your audio playback |
| Localized text not showing | Localization system not connected | Use `localizationKey` with the Localization system |

---

## See Also

- [Localization](Localization.md) -- Translated dialogue text
- [UI System](UI-System.md) -- Displaying dialogue in game UI
- [AI and Navigation](AI-and-Navigation.md) -- NPC behavior during conversations
- [Audio](Audio.md) -- Voice clip playback
- [Event System](Event-System.md) -- Dialogue-triggered game events
