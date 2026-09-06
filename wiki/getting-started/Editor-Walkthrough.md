# Editor Walkthrough

A practical, hands-on guide to the SparkEditor. This page covers day-to-day workflows rather than exhaustive API details. For the full panel reference, see [SparkEditor](../gameplay-tools/SparkEditor.md).

> **Release boundary:** SparkEditor is a required Windows authoring product in the
> blocked and uncertified `stable-v1` profile. Panel descriptions are source-level
> guidance; registration, default visibility, and complete undo/gizmo behavior are
> separate gates. Collaboration, visual scripting, plugins, and non-Windows paths
> remain experimental or outside the profile.

---

## Opening the Editor

Build with `ENABLE_EDITOR=ON` and launch the separate `SparkEditor` executable.
The runtime hosts do not currently define an F1 editor-overlay toggle.

---

## Default Layout

The current factory metadata marks 6 core panels visible by default:

| Panel | Position | What it does |
|-------|----------|--------------|
| **Scene View** | Center | 3D viewport with editor gizmos and camera controls |
| **Hierarchy** | Left | Tree view of all entities in the scene |
| **Inspector** | Right | Property editor for the selected entity |
| **Asset Browser** | Bottom | Project file browser with thumbnails |
| **Console** | Bottom | Log output, command input, and filtering |
| **Game View** | Tab (center) | In-game camera preview with HUD |

Registered panels can be dragged, docked, resized, or closed. The **Window** menu can reopen the panels that the current editor build registers; the 65-header inventory is not a registration or release-certification count.

---

## Scene View Navigation

The Scene View is your primary 3D viewport.

| Input | Action |
|-------|--------|
| Right-click + drag | Orbit camera |
| Middle-click + drag | Pan camera |
| Scroll wheel | Zoom in/out |
| Click entity | Select it |
| W | Translate gizmo (implemented path) |
| E | Select rotate mode (current transform application is incomplete; `EDT-210`) |
| R | Select scale mode (current transform application is incomplete; `EDT-210`) |

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
- Or use the **Object Placement** panel (Window → Object Placement): **Quick Place** and **Place Selected** create real, undoable entities from the editor's entity templates. The brush/line/grid/scatter modes, align mode, and snap settings are authored in the panel but not yet read by the viewport

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

Some scene-edit operations participate in the command stack; complete operation coverage remains blocked by `EDT-210`:

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

Open from **Window → Weapon Editor**. It is a balance calculator (DPS chart and comparison table); edits are session-only -- the panel cannot save and no game module reads them:

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

Open **Window → Build & Cook** for the current local build/staging interface. Its output is not a certified release package:

- Select build profile (Debug / Development / Release / Shipping)
- Choose target platform
- Inspect reserved texture/audio/mesh transform controls; those transforms are not implemented and content is copied unchanged
- Run the available local build/staging step with progress monitoring

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

Collaborative editing is an experimental implementation outside `stable-v1`; the following is development guidance rather than a release-support claim:

1. Open **Window → Collaboration**
2. One user clicks **Host** to start a session
3. Others click **Join** and enter the host address
4. Entity locks prevent two users from editing the same object simultaneously
5. The activity log shows who is editing what

See [Collaborative Editing](../subsystems/Collaborative-Editing.md) for details.

---

## Customization

### Themes

The editor uses a customizable theme system. Default themes are applied via `EditorTheme.cpp`. Custom themes can override colors, spacing, and font sizes.

### Panel Visibility

- Registered panels are accessible from the **Window** menu; the repository's
  65 `*Panel.h` classes are a source inventory, not a visibility guarantee
- Drag panels to rearrange the layout
- Layouts persist between sessions

---

## Keyboard Reference

| Shortcut | Action |
|----------|--------|
| F3 | Toggle FPS stats |
| ` (Backtick) | Toggle console |
| W | Translate gizmo |
| E | Select rotate mode (transform application incomplete) |
| R | Select scale mode (transform application incomplete) |
| Ctrl+Z | Undo |
| Ctrl+Y | Redo |
| Ctrl+S | Save scene |
| Delete | Delete selected entity |

---

## See Also

- [SparkEditor](../gameplay-tools/SparkEditor.md) — Full panel reference and architecture details
- [Artist Workflow Guide](Artist-Workflow-Guide.md) — Asset creation workflows
- [Quick-Start Tutorial](Quick-Start-Tutorial.md) — Your first 10 minutes
- [Configuration Reference](../advanced/Configuration-Reference.md) — All settings and console commands
