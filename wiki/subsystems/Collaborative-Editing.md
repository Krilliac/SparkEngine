# Collaborative Editing

SparkEngine's collaborative editing system enables multiple editor instances to work on the same scene simultaneously, inspired by HeroEngine's live collaborative editing. The system provides real-time peer presence awareness, node-level locking, edit broadcasting, and optional live push to running game servers.

## Architecture

```
[Editor A] ◄──── TCP ────► [CollaborativeEditSession Host] ◄──── TCP ────► [Editor B]
                               │  Node Locking                                  │
                               │  Edit Broadcasting                              │
                               │  Presence Awareness                             │
                               │                                                 │
                               └──── LiveEditBridge (optional) ──────────────────┘
                                              │
                                      [AreaServer]
                                              │
                                      [Game Clients see changes]
```

### Three Layers

| Layer | System | Purpose |
|-------|--------|---------|
| **Editor Collaboration** | `CollaborativeEditSession` | Peer-to-peer editing between editor instances |
| **Editor ↔ Engine IPC** | `EngineInterface` | Named pipe communication with local engine process |
| **Live Push** | `LiveEditBridge` | Forward edits to a running AreaServer for live game updates |

These are intentionally separate systems. Editor collaboration uses TCP for reliable ordered delivery of edits. Game networking uses UDP for low-latency gameplay. The `LiveEditBridge` connects the two when live editing of a running game world is desired.

## No Game Module Code Required

**All collaborative editing is handled entirely within the editor.** There is no need to write any C++ code in your game module to use collaborative editing. The system is fully integrated into the SparkEditor:

- **Hosting/joining**: Use the **Collaboration panel** (View > Collaboration) to host or join sessions
- **Locking**: The editor automatically acquires/releases locks when you select and edit nodes
- **Edit broadcasting**: Property changes in the Inspector and object operations in the Hierarchy are automatically broadcast to all connected editors
- **Presence**: Other editors' selections and viewport positions are shown automatically in the Scene View

The C++ code examples below are **internal API reference** showing how the editor's subsystems work under the hood. They are provided for engine developers extending the collaborative editing system, not for game developers using it.

## How to Use (No Code Needed)

1. Open the **Collaboration** panel from **View > Collaboration**
2. One editor clicks **Host Session** (sets port and username)
3. Other editors enter the host's IP address and click **Join Session**
4. Edit the scene normally — locking, broadcasting, and presence are automatic
5. For persistent sessions without tying up an editor, run a **headless collab server** (see below)

## Internal API Reference

> The following C++ examples show the editor's internal implementation. You do not need to write this code — it runs automatically when you use the Collaboration panel.

### Hosting a Session

```cpp
SparkEditor::CollaborativeEditSession session;
session.Host(27030, "Alice");  // Opens TCP listener on port 27030
```

### Connecting to a Session

```cpp
session.Connect("192.168.1.100", 27030, "Bob");  // TCP connect with 5s timeout
```

### Node Locking

The editor automatically locks nodes when you select them for editing:

```cpp
if (session.RequestLock("Entity_42"))
{
    // Lock acquired — safe to edit
    SparkEditor::EditMessage msg;
    msg.type = SparkEditor::EditMessageType::NodeModified;
    msg.sourceEditor = session.GetLocalPeerID();
    msg.nodeId = "Entity_42";
    msg.propertyName = "position";
    msg.newValue = "10.0, 5.0, 3.0";
    session.BroadcastEdit(msg);

    session.ReleaseLock("Entity_42");
}
else
{
    auto owner = session.GetLockOwner("Entity_42");
    auto* peer = session.GetPeer(owner);
    // Display: "Locked by Bob"
}
```

### Presence Awareness

The editor broadcasts selection and camera state automatically:

```cpp
// These are called internally by HierarchyPanel and SceneViewPanel
session.SetLocalSelection("Entity_42");
session.SetLocalViewportCamera(cameraPos, cameraDir);

// Called each frame by EditorUI::Update()
session.Update(deltaTime);

// SceneViewPanel renders peer overlays automatically
auto peers = session.GetConnectedPeers();
for (const auto& peer : peers)
{
    // Draw peer's name tag with color and selection info
}
```

### Callbacks

These are wired automatically by `EditorUI::WireCallbacks()`:

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

## Headless Collab Server Mode

The editor can run as a dedicated headless collaboration server without a GUI:

```bash
# Basic usage
SparkEditor --collab-server

# With custom port and name
SparkEditor --collab-server --collab-port 27030 --collab-name "TeamServer"
```

This mode:
- Starts a TCP listener on the specified port
- Accepts editor connections and relays messages between them
- Runs at 10 Hz with periodic status output
- Handles Ctrl+C for graceful shutdown
- Does not create a window or initialize graphics

Use this for persistent team collaboration sessions where you don't want one editor to be the host.

## Live Push to Running Games

The `LiveEditBridge` enables HeroEngine-style live editing where changes made by editors appear in the running game world in real-time. This is also handled within the editor — no game module code is needed. The editor's Collaboration panel will include a "Connect to AreaServer" option when an AreaServer is running.

### Architecture

```
Editor → CollaborativeEditSession → LiveEditBridge → AreaServer → Game Clients
```

### Internal API (called automatically by EditorUI)

```cpp
// Created and managed by EditorUI — not user code
SparkEditor::LiveEditBridge bridge;
bridge.Connect("192.168.1.200", 27031, "EditorAlice");  // AreaServer inter-server port

// Edits are forwarded automatically when HierarchyPanel operations occur
bridge.PushEdit(editMessage);

// Called each frame by EditorUI::Update()
bridge.Update();
```

### Custom Message Types

The bridge uses `UserDefined` message types (starting at 1000) to avoid collision with game messages:

| Type | Value | Description |
|------|-------|-------------|
| `SceneEdit` | 1000 | A scene edit (node/component change) |
| `LockNotify` | 1001 | Lock state change notification |
| `EditorJoin` | 1002 | Editor connecting as privileged client |
| `EditorLeave` | 1003 | Editor disconnecting |

## Editor UI — Collaboration Panel

The **Collaboration** panel (View → Collaboration) provides:
- **Connection controls**: Host or join a session with username and port
- **Peer list**: Shows all connected editors with their colors and current selections
- **Lock list**: Active locks with owner names, durations, and release buttons
- **Edit log**: Recent edit activity across all peers
- **Session stats**: Peer count, lock count, edit counts, session duration

## Viewport Peer Visualization

When a collaborative session is active, the Scene View panel shows:
- **Name tags**: Each remote peer's username displayed in their assigned color
- **Selection info**: What node each peer is currently editing
- Tags appear in the top-right corner of the viewport

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
- When an editor disconnects, all their locks are released automatically
- Lock ownership is tracked with editor name for UI display
- Double-locking the same node by the same peer is idempotent (succeeds)

## Wire Protocol

Messages are sent as length-prefixed TCP frames:

```
[4 bytes: message length N] [N bytes: serialized InternalMessage]
```

The serialization uses big-endian integers and length-prefixed strings. Maximum message size is 16 MB.

## Thread Safety

- `CollaborativeEditSession` uses one network thread for socket I/O plus per-client handler threads on the host
- Message queues (`m_incomingMessages`, `m_outgoingMessages`) are mutex-protected
- `m_connected` and `m_shuttingDown` use `std::atomic<bool>` with appropriate memory ordering
- Peer map and lock map have separate mutexes (`m_peerMutex`, `m_lockMutex`)
- The main thread drains queues and fires callbacks — callbacks always run on the main thread

## Session Statistics

```cpp
auto stats = session.GetStats();
// stats.peerCount, stats.activeLocks, stats.editsBroadcast, stats.editsReceived, stats.sessionDuration
```

## Design Decisions

- **Pessimistic locking** over CRDTs/OT for simplicity and predictability in small teams (2-10 editors)
- **Node-level granularity** (not property-level) reduces lock contention while keeping the protocol simple
- **Auto-expiry** prevents forgotten locks from blocking other editors
- **TCP** for editor collaboration (reliable, ordered) vs **UDP** for game networking (low-latency)
- **Separate networking stacks** — editor collab and game networking serve fundamentally different needs
- **Peer-hosted by default** — no infrastructure required; dedicated server mode available for larger teams

## Source Files

| File | Description |
|------|-------------|
| `SparkEditor/Source/Communication/CollaborativeEditSession.h` | Session class, types, wire protocol |
| `SparkEditor/Source/Communication/CollaborativeEditSession.cpp` | Session implementation with TCP networking |
| `SparkEditor/Source/Communication/LiveEditBridge.h` | Live push bridge to AreaServer |
| `SparkEditor/Source/Communication/LiveEditBridge.cpp` | Bridge implementation |
| `SparkEditor/Source/Panels/CollaborationPanel.h` | Collaboration UI panel |
| `SparkEditor/Source/Panels/CollaborationPanel.cpp` | Panel implementation |
| `SparkEditor/Source/main.cpp` | `--collab-server` CLI mode |
| `Tests/TestCollaborativeEditing.cpp` | Unit tests |

## Related Pages

- [SparkEditor](../gameplay-tools/SparkEditor.md) — Editor overview
- [Networking](Networking.md) — Base networking system (game networking)
