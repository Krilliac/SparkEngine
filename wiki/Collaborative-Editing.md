# Collaborative Editing

SparkEngine's collaborative editing system enables multiple editor instances to work on the same scene simultaneously, inspired by HeroEngine's live collaborative editing. The system provides peer presence awareness, node-level locking, and edit broadcasting.

## Overview

```
[Editor A] ◄──────► [CollaborativeEditSession] ◄──────► [Editor B]
   Host                 Node Locking                      Client
                        Edit Broadcasting
                        Presence Awareness
```

The `CollaborativeEditSession` class manages:
- **Peer presence** — See other editors' selections and viewport cameras
- **Node locking** — Prevent conflicting edits via pessimistic locking
- **Edit broadcasting** — Real-time edit visibility across all editors
- **Conflict resolution** — Lock-based conflict prevention with auto-expiry

## Usage

### Hosting a Session

```cpp
SparkEditor::CollaborativeEditSession session;

// Host a session (one editor acts as host)
session.Host(27030, "Alice");

// Or connect to an existing session
session.Connect("192.168.1.100", 27030, "Bob");
```

### Node Locking

Before editing a scene node, request a lock:

```cpp
if (session.RequestLock("Entity_42"))
{
    // Lock acquired — safe to edit
    // Make edits...

    // Broadcast the edit to other editors
    SparkEditor::EditMessage msg;
    msg.type = SparkEditor::EditMessageType::NodeModified;
    msg.nodeId = "Entity_42";
    msg.propertyName = "position";
    msg.newValue = "10.0, 5.0, 3.0";
    session.BroadcastEdit(msg);

    // Release the lock when done
    session.ReleaseLock("Entity_42");
}
else
{
    // Locked by another editor — show who holds the lock
    auto owner = session.GetLockOwner("Entity_42");
    auto* peer = session.GetPeer(owner);
    // Display: "Locked by Bob"
}
```

### Presence Awareness

```cpp
// Update local state for other editors to see
session.SetLocalSelection("Entity_42");
session.SetLocalViewportCamera(cameraPos, cameraDir);

// In the editor loop:
session.Update(deltaTime);

// Get connected peers and render their selections
auto peers = session.GetConnectedPeers();
for (const auto& peer : peers)
{
    // Draw peer's selection highlight in viewport
    // Show peer's camera frustum
    // Display peer's name tag
}
```

### Callbacks

```cpp
session.SetPeerConnectedCallback([](const SparkEditor::EditorPeer& peer) {
    LOG("Editor '%s' joined", peer.userName.c_str());
});

session.SetEditReceivedCallback([](const SparkEditor::EditMessage& msg) {
    // Apply the edit locally
});

session.SetLockChangedCallback([](const std::string& nodeId, SparkEditor::PeerID owner) {
    // Update lock indicators in the UI
});
```

## Edit Message Types

| Type | Description |
|------|-------------|
| `NodeAdded` | A new node was added to the scene |
| `NodeRemoved` | A node was removed from the scene |
| `NodeModified` | A node's properties were changed |
| `NodeMoved` | A node's transform was changed |
| `NodeRenamed` | A node was renamed |
| `ComponentAdded` | A component was added to an entity |
| `ComponentRemoved` | A component was removed from an entity |
| `ComponentModified` | A component's properties were changed |

## Lock Behavior

- Locks are **pessimistic** — only one editor can lock a node at a time
- Locks auto-expire after **5 minutes** (configurable via `NodeLock::maxDurationSeconds`)
- When an editor disconnects, all their locks are released
- Lock ownership is tracked with editor name for UI display

## Session Statistics

```cpp
auto stats = session.GetStats();
// stats.peerCount, stats.activeLocks, stats.editsBroadcast, stats.editsReceived
```

## Design Decisions

- **Pessimistic locking** was chosen over CRDTs/OT for simplicity and predictability in small teams (2-10 editors)
- **Node-level granularity** (not property-level) reduces lock contention while keeping the protocol simple
- **Auto-expiry** prevents forgotten locks from blocking other editors

## Source Files

| File | Description |
|------|-------------|
| `SparkEditor/Source/Communication/CollaborativeEditSession.h` | Session class and types |
| `SparkEditor/Source/Communication/CollaborativeEditSession.cpp` | Session implementation |

## Related Pages

- [SparkEditor](SparkEditor) — Editor overview
- [Networking](Networking) — Base networking system
