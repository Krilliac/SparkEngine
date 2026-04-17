# Quick-Start Tutorial

A hands-on guide to your first 10 minutes with SparkEngine. By the end, you will have built the engine, launched it, explored the editor, and spawned objects in a scene.

> **Prerequisites:** You have already completed the [Getting Started](Getting-Started.md) guide and have a successful build.

---

## Step 1: Launch the Engine

```bash
# Windows
cd build\bin
SparkEngine.exe

# Linux
cd build/bin
./SparkEngine
```

You should see a window with a 3D viewport and the debug console. The default game module (SparkGameFPS) loads automatically.

**Default controls:**

| Key | Action |
|-----|--------|
| W/A/S/D | Move |
| Mouse | Look around |
| Space | Jump |
| Shift | Sprint |
| Left Click | Fire (captures mouse) |
| Esc | Release mouse |
| ` (Backtick) | Toggle console |
| F1 | Toggle editor |
| F3 | Toggle FPS stats |

---

## Step 2: Open the Console

Press **`** (backtick) to open the debug console. Try these commands:

```
help                    # List all available commands
version                 # Show engine version
render_stats            # Display rendering statistics
physics_metrics         # Show physics performance
```

The console supports tab-completion and command history (up/down arrows).

---

## Step 3: Explore the Editor

Press **F1** to open the editor overlay. You will see these panels:

| Panel | Purpose |
|-------|---------|
| **Scene View** (center) | 3D viewport — click and drag to orbit, scroll to zoom |
| **Hierarchy** (left) | Lists every entity in the scene |
| **Inspector** (right) | Shows components of the selected entity |
| **Asset Browser** (bottom) | Browse project files |
| **Console** (bottom) | Log output and commands |

### Try it:

1. **Select an entity** — Click any item in the Hierarchy panel
2. **Inspect it** — The Inspector shows its Transform, MeshRenderer, and other components
3. **Move it** — In the Scene View, use the gizmo handles (arrows) to drag the entity. Press **W** for translate, **E** for rotate, **R** for scale gizmos
4. **Add a component** — In the Inspector, click "Add Component" and choose a type

---

## Step 4: Spawn Objects via Console

Use the console to spawn entities at specific coordinates:

```
spawn box 0 5 0          # Spawn a physics box 5 units above origin
spawn sphere 2 10 0       # Spawn a physics sphere
spawn box -3 8 0          # Another box
```

Watch the physics objects fall and collide. Toggle physics debug visualization:

```
physics_debug on          # Show collision shapes and contacts
physics_debug off         # Hide debug overlay
```

---

## Step 5: Adjust the World

Try changing the environment:

```
# Time of day
time_set 6               # Sunrise
time_set 12              # Noon
time_set 18              # Sunset
time_set 0               # Midnight
time_speed 10            # Speed up the day/night cycle

# Weather
weather clear
weather rain
weather snow
weather storm

# Physics
physics_gravity 0 -5 0   # Low gravity
physics_gravity 0 -20 0  # Normal gravity (default)
```

---

## Step 6: Change Graphics Settings

Experiment with rendering settings:

```
# Quality presets
render_quality low
render_quality high
render_quality ultra

# Individual toggles
wireframe on              # Wireframe rendering
wireframe off
vsync off                 # Disable VSync
shadows off               # Disable shadows
hdr on                    # Enable HDR
```

Or edit `Resources/Config/settings.ini` for persistent changes. See [Configuration Reference](../advanced/Configuration-Reference.md) for all available settings.

---

## Step 7: Try Game Modes (FPS Module)

If the FPS game module is loaded:

```
# Cheat commands
god                       # Toggle god mode
noclip                    # Toggle noclip (fly through walls)
player_tp 0 50 0          # Teleport to coordinates

# Game modes
gamemode freeplay         # Free exploration
gamemode deathmatch       # Deathmatch mode
gamemode survival         # Wave-based survival

# Inventory
give 1                    # Give item ID 1
inventory                 # Show inventory
```

Start a wave-based survival round:

```
wave_start                # Begin waves
wave_status               # Check current wave
wave_difficulty 2.0       # Increase difficulty
```

---

## Step 8: Look Under the Hood

Check what the engine is doing:

```
engine_subsystems         # List all active subsystems
metrics                   # Comprehensive system metrics
asset_list                # All loaded assets
shader_list               # All loaded shaders
```

Toggle debug visualizations:

```
physics_debug on          # Collision shapes
```

Open the **Profiler** panel in the editor (Window menu) to see frame timings, draw call counts, and system performance.

---

## What's Next?

Now that you have a feel for the engine, explore these guides based on your role:

### Programmers
1. [Creating a Game Module](Creating-a-Game-Module.md) — Build your own game module
2. [Entity Component System](../subsystems/Entity-Component-System.md) — ECS architecture
3. [Scripting with AngelScript](../subsystems/Scripting-with-AngelScript.md) — Hot-reload scripting

### Artists & Designers
1. [Artist Workflow Guide](Artist-Workflow-Guide.md) — Asset creation and import
2. [Editor Walkthrough](Editor-Walkthrough.md) — Complete editor guide
3. [Terrain and Procedural Generation](../gameplay-tools/Terrain-and-Procedural-Generation.md) — Terrain tools

### Gameplay Designers
1. [Making Your First Game](Making-Your-First-Game.md) — Build a complete game
2. [Gameplay Systems](../gameplay-tools/Gameplay-Systems.md) — Weapons, inventory, quests
3. [AI and Navigation](../subsystems/AI-and-Navigation.md) — Behavior trees and pathfinding

### Multiplayer Developers
1. [Multiplayer Quick Start](../subsystems/Multiplayer-Quick-Start.md) — Set up client/server
2. [Networking](../subsystems/Networking.md) — Full networking reference
3. [Dedicated Server](../subsystems/Dedicated-Server.md) — Headless server setup

---

## See Also

- [FAQ](FAQ.md) — Common questions and answers
- [Configuration Reference](../advanced/Configuration-Reference.md) — All settings and commands
- [Editor Walkthrough](Editor-Walkthrough.md) — Practical editor guide
- [Troubleshooting](../advanced/Troubleshooting.md) — Common issues and solutions
