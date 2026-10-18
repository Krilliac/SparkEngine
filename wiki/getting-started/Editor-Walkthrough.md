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

### The Start Page

The first time the editor opens, a **Start** tab sits in the centre of the
workspace. From it you can **Open Scene...**, start a **New Scene**, **Open
Project Folder**, **Reset workspace layout**, reopen one of your recent scenes,
or read the keyboard-shortcut sheet. Clear **Show at startup** to stop it
appearing; **Help > Welcome...** (or **View > Start Page**) brings it back, and
Esc closes it.

### The Status Bar and Notifications

The bar along the bottom of the window shows the editor mode (EDIT, or PREVIEW
with the whole bar in the ember accent while a play preview or launched game
runs), the engine connection, the project and the scene (marked `*` with
unsaved changes), selection counts, FPS and frame time, a notification bell and
the UI scale. Messages from the editor appear as toasts in the bottom-right
corner: Info and Success disappear after 4 seconds, Warning after 8, and Error
stays until you close it. Point at a toast to pause it, and click the bell to see
the last 50 messages.

---

## Default Layout

The core panels visible by default are:

| Panel | Position | What it does |
|-------|----------|--------------|
| **Scene View** | Center | 3D viewport with editor gizmos and camera controls |
| **Hierarchy** | Left | Tree view of all entities in the scene |
| **Inspector** | Right | Property editor for the selected entity |
| **Asset Browser** | Bottom | Project file browser with thumbnails |
| **Console** | Bottom | Log output, command input, and filtering |
| **Game View** | Tab (center) | In-game camera preview with HUD |

Registered panels can be dragged, docked, resized, or closed. The **Window** menu can reopen the panels that the current editor build registers; the 64-header inventory is not a registration or release-certification count.

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
- And many more component types

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
- **AI Debug** — Inspect available agent diagnostics and panel views. State preview does not run AI or produce live game-agent data; use a separately launched game for gameplay diagnosis.

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

## State Preview and Launch Game

The editor's **State Preview Toolbar** controls a local preview state machine.
It does not tick game physics, AI, scripts, audio, or a game module. The main
toolbar button and F5/F6 open **Play Control**; select a module there and use
**Launch Game** to start gameplay in a separate SparkEngine process. Verify
that process independently.

| Button | Action |
|--------|--------|
| **Start** | Enter state preview; no gameplay runs |
| **Pause** | Pause the preview state counter |
| **Stop** | Exit state preview and restore the editor snapshot |
| **Step** | Advance one preview state frame, not a game frame |

### Time Scale

The toolbar includes a time-scale slider with presets:

- **0.25x** — Quarter preview-counter speed
- **0.5x** — Half speed
- **1x** — Normal
- **2x / 4x** — Fast-forward

These settings scale preview state time, not gameplay simulation speed.

### Subsystem Toggles

The preview exposes flags named for these subsystems:

- Physics, AI, Audio, Animation, Scripting, Particles

These flags affect preview counters only. They do not enable, disable, or
exercise the actual subsystems, so they cannot isolate gameplay bugs.

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

### Appearance and Themes

Open **View > Appearance...** (Help > Themes... opens the same window). There you
choose:

- the **palette**: Ember Studio (default), Cobalt Forge or Graphite Signal, or a
  custom theme you imported;
- the **appearance**: Dark, Light, High Contrast, or Match Windows;
- the **density**: Standard, or Compact for smaller controls (text never shrinks);
- the **UI scale**: Auto (your monitor's display scale) or a fixed 75-200 %;
- **Always show keyboard focus**, **Reduce motion**, **Show status bar** and
  **Show Start page at startup**.

Your choices apply immediately and are remembered the next time you start the
editor. To try a palette for one session only, start the editor with
`--theme "Cobalt Forge"`.

### UI Scale

| Shortcut | Action |
|----------|--------|
| Ctrl+= (or keypad +) | Make the UI larger by 12.5 % |
| Ctrl+- (or keypad -) | Make the UI smaller by 12.5 % |
| Ctrl+0 | Back to Auto (the display scale) |

The same commands are in **View > UI Scale**, and clicking the percentage on the
status bar resets it. On Windows the editor follows each monitor's scale when you
drag it between monitors.

### Panel Visibility and Layouts

- Registered panels are accessible from the **Window** menu; the repository's
  64 `*Panel.h` classes are a source inventory, not a visibility guarantee
- Drag panels to rearrange the layout
- Layouts persist between sessions
- Save, load, delete and reset named layouts from the **Window** menu or from
  the layout menu at the right end of the toolbar; **Save Layout As...** asks for
  a name, and deleting a layout asks for confirmation

Layouts saved by an earlier editor open with the bottom panels about 26 px
shorter the first time, because the status bar no longer covers the dock area.
Save the layout again to keep the new size.

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
| Ctrl+P | Command palette |
| Ctrl+F | Search panel |
| Ctrl+= / Ctrl+- | UI scale up / down |
| Ctrl+0 | UI scale back to Auto |
| Esc | Close the Start page |
| Delete | Delete selected entity |

---

## See Also

- [SparkEditor](../gameplay-tools/SparkEditor.md) — Full panel reference and architecture details
- [Artist Workflow Guide](Artist-Workflow-Guide.md) — Asset creation workflows
- [Quick-Start Tutorial](Quick-Start-Tutorial.md) — Your first 10 minutes
- [Configuration Reference](../advanced/Configuration-Reference.md) — All settings and console commands
