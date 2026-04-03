# Editor Walkthrough

A practical, hands-on guide to the SparkEditor. This page covers day-to-day workflows rather than exhaustive API details. For the full panel reference, see [SparkEditor](SparkEditor).

---

## Opening the Editor

Press **F1** while the engine is running. The editor is an ImGui overlay that runs on top of the game viewport. Press **F1** again to close it.

The editor requires `ENABLE_EDITOR=ON` at build time (default on Windows).

---

## Default Layout

When you first open the editor, 7 core panels are visible:

| Panel | Position | What it does |
|-------|----------|--------------|
| **Scene View** | Center | 3D viewport with editor gizmos and camera controls |
| **Hierarchy** | Left | Tree view of all entities in the scene |
| **Inspector** | Right | Property editor for the selected entity |
| **Asset Browser** | Bottom | Project file browser with thumbnails |
| **Console** | Bottom | Log output, command input, and filtering |
| **Game View** | Tab (center) | In-game camera preview with HUD |
| **Profiler** | Tab (bottom) | Real-time performance graphs |

All panels can be dragged, docked, resized, or closed. Reopen any panel from the **Window** menu.

---

## Scene View Navigation

The Scene View is your primary 3D viewport.

| Input | Action |
|-------|--------|
| Right-click + drag | Orbit camera |
| Middle-click + drag | Pan camera |
| Scroll wheel | Zoom in/out |
| Click entity | Select it |
| W | Translate gizmo (move) |
| E | Rotate gizmo |
| R | Scale gizmo |

### Render Modes

The Scene View toolbar lets you switch between render modes:

- **Shaded** — Full PBR rendering (default)
- **Wireframe** — Wireframe overlay
- **Unlit** — No lighting, diffuse color only
- **Normals** — Visualize surface normals

### Grid and Snapping

- The grid is visible by default (toggle in Editor settings)
- **Snap to grid** is enabled by default with 1.0 unit spacing
- Change grid size in `settings.ini` under `[Editor]`: `GridSize = 0.5`

---

## Working with Entities

### Creating Entities

- **Hierarchy panel** → Right-click → "Create Entity"
- Or use the **Object Placement** panel (Window → Object Placement) for advanced placement with brush, line, grid, and scatter modes

### Selecting Entities

- Click in the **Scene View** to select
- Or click in the **Hierarchy** panel
- The selected entity highlights in the viewport and the Inspector shows its components

### Editing Components

The **Inspector** panel displays all components attached to the selected entity:

- **Transform** — Position, rotation, scale (always present)
- **MeshRenderer** — 3D mesh and material
- **RigidBody** — Physics body settings
- **Light** — Point, directional, or spot light
- **AudioSource** — Sound emitter
- **Camera** — Camera parameters
- And 70+ more component types

Click **Add Component** at the bottom of the Inspector to attach new components.

### Undo / Redo

All editor operations support undo/redo:

- **Ctrl+Z** — Undo
- **Ctrl+Y** — Redo
- Open the **Undo History** panel to see the full command stack and jump to any point

---

## Content Creation Panels

### Material Editor

Open from **Window → Material Editor**. Edit PBR material properties:

- **Albedo** — Base color and texture
- **Metallic / Roughness** — PBR parameters with texture slots
- **Normal Map** — Surface detail
- **Emission** — Self-illumination
- **AO** — Ambient occlusion

Drag textures from the Asset Browser into texture slots. The live preview sphere updates in real-time.

### Particle Editor

Open from **Window → Particle Editor**. Configure GPU particle emitters:

- **Emission** — Rate, burst count, shape (point, sphere, cone, box, circle)
- **Appearance** — Color gradient over lifetime, size curve
- **Physics** — Gravity, drag, collision
- **Sub-emitters** — Particles that spawn particles
- **Trails** — Ribbon trail rendering

### Sprite & Tilemap Editors

For 2D games:

- **Sprite Editor** — Configure source rects, pivots, pixels-per-unit, sorting layers
- **Sprite Animation Editor** — Frame-based animation with timeline, onion skinning, and sprite sheet auto-slice
- **Tilemap Editor** — Paint tiles with brush, fill, and rectangle tools. Supports collision tiles, layers, and auto-tiling

### Terrain Editor

Open from **Window → Terrain Editor**. Sculpt heightmap terrain:

- Paint height with raise/lower/smooth/flatten brushes
- Paint textures with blending layers
- Integrated with the physics system for collision

---

## Gameplay & Design Panels

### Weapon Editor

Open from **Window → Weapon Editor**. Balance weapons by editing stats:

- Damage, fire rate, magazine size, reload time
- Accuracy, range, recoil pattern
- DPS chart for quick comparison
- Side-by-side weapon comparison table

### AI Editor & Debug

- **AI Editor** — Create behavior tree templates with Selector, Sequence, Action, Condition, Decorator, and Parallel nodes
- **AI Debug** — Live-inspect AI agents during play mode. Shows blackboard variables, BT execution trace, perception ranges, and nav paths

### Dialogue Editor

Open from **Window → Dialogue Editor**. Author branching dialogue trees:

- Create text, choice, branch, event, and end nodes
- Assign speakers, animations, and voice clips
- Set conditions on choices (requires variables set in the Condition Editor)

### Cinematic Sequencer

Open from **Window → Cinematic Sequencer**. Create cutscenes:

- Add tracks (camera, actor, audio, event)
- Place keyframes on the timeline
- Scrub the timeline for preview
- Export to JSON

### Event Response Panel

Open from **Window → Event Response**. Build "When/If/Then" gameplay rules without code:

- **When** — Choose a trigger event type
- **If** — Add conditions (AND/OR groups)
- **Then** — Define actions to execute

---

## Physics Panels

### Physics 3D

Open from **Window → Physics 3D**. Configure the Jolt Physics world:

- Set gravity, timestep, substep count
- Toggle debug visualization (wireframes, AABBs, contacts, constraints)
- Quick-add primitive shapes (box, sphere, capsule, cylinder)
- Physics material presets
- Raycast testing tool

### Physics 2D

Open from **Window → Physics 2D**. For 2D physics:

- World gravity settings
- Collision layer matrix editor
- Body inspector
- Spatial hash grid visualization

---

## Audio

### Audio Mixer

Open from **Window → Audio Mixer**:

- Master / SFX / Music volume sliders
- Mix bus hierarchy with VU meters
- DSP effects (reverb, EQ, compressor)
- Active sound monitoring
- Sound bank browser
- Reverb zone configuration

---

## Project Management

### Project Settings

Open from **Window → Project Settings**. Tabbed categories for all engine settings:

- Graphics, Rendering, Post-Processing, Audio, Controls
- Physics, AI, Player, Gameplay, Camera
- Network, Scripting, Animation, Editor, Logging

Changes apply at runtime and can be saved to `settings.ini`.

### Build & Cook

Open from **Window → Build & Cook**. Package your game:

- Select build profile (Debug / Development / Release / Shipping)
- Choose target platform
- Configure asset cooking (texture, audio, mesh compression)
- One-click build with progress monitoring

### Game Module Selector

Open from **Window → Game Module Selector**:

- Scan for available game module DLLs
- Toggle modules active/inactive
- Generate `spark.modules.json` manifest

---

## Debugging Panels

### Debug Visualizer

Open from **Window → Debug Visualizer**. Toggle overlays:

- Physics (colliders, AABBs, contacts, raycasts, velocities)
- Navigation (navmesh, paths, agents)
- Audio (listener position, source ranges)
- Camera and light debug
- Performance overlays (FPS, draw calls, triangle count)

### Scene Statistics

Open from **Window → Scene Statistics**:

- Entity counts and component breakdowns
- Rendering statistics (draw calls, triangles, batches)
- Physics body counts
- Memory usage
- Performance graphs with history

### Event Monitor

Open from **Window → Event Monitor**. Watch the EventBus in real-time:

- Filter by event type or category
- Color-coded by category
- Useful for debugging event-driven systems

### Coroutine Debug

Open from **Window → Coroutine Debug**. Monitor active coroutines:

- Status (running, suspended, completed, failed)
- Elapsed time and wait states
- Cancel coroutines individually

---

## Play Mode

The **Play Mode Toolbar** (top of editor) controls simulation:

| Button | Action |
|--------|--------|
| **Play** | Start simulation |
| **Pause** | Freeze simulation |
| **Stop** | Reset to editor state |
| **Step** | Advance one frame |

### Time Scale

The toolbar includes a time-scale slider with presets:

- **0.25x** — Quarter speed (useful for debugging physics)
- **0.5x** — Half speed
- **1x** — Normal
- **2x / 4x** — Fast-forward

### Subsystem Toggles

During play mode, you can selectively enable/disable:

- Physics, AI, Audio, Animation, Scripting, Particles

This is useful for isolating bugs — disable everything except the system you are debugging.

---

## Collaborative Editing

SparkEngine supports multi-user editing sessions:

1. Open **Window → Collaboration**
2. One user clicks **Host** to start a session
3. Others click **Join** and enter the host address
4. Entity locks prevent two users from editing the same object simultaneously
5. The activity log shows who is editing what

See [Collaborative Editing](Collaborative-Editing) for details.

---

## Customization

### Themes

The editor uses a customizable theme system. Default themes are applied via `EditorTheme.cpp`. Custom themes can override colors, spacing, and font sizes.

### Panel Visibility

- All 56 panels are accessible from the **Window** menu
- Drag panels to rearrange the layout
- Layouts persist between sessions

---

## Keyboard Reference

| Shortcut | Action |
|----------|--------|
| F1 | Toggle editor |
| F3 | Toggle FPS stats |
| ` (Backtick) | Toggle console |
| W | Translate gizmo |
| E | Rotate gizmo |
| R | Scale gizmo |
| Ctrl+Z | Undo |
| Ctrl+Y | Redo |
| Ctrl+S | Save scene |
| Delete | Delete selected entity |

---

## See Also

- [SparkEditor](SparkEditor) — Full panel reference and architecture details
- [Artist Workflow Guide](Artist-Workflow-Guide) — Asset creation workflows
- [Quick-Start Tutorial](Quick-Start-Tutorial) — Your first 10 minutes
- [Configuration Reference](Configuration-Reference) — All settings and console commands
