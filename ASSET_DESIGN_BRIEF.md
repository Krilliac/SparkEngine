# TERRAFRONT — Complete Asset Design Brief
### A full production prompt for a Claude design session
**Hand-off usage:** open a Claude session in `D:\SparkEngine` and say: *"Read ASSET_DESIGN_BRIEF.md and produce the assets it specifies, batch by batch, starting with Batch P0."*

---

## 1. What you are building

You are the art team for **TERRAFRONT**, a PlanetSide-2-spirit combined-arms MMOFPS running on the custom **SparkEngine** (C++23, D3D11, PBR + IBL, TAA, 2048px shadow atlas). Three factions wage persistent territory war over the desert world **Veyra**. The game is mechanically complete (infantry gunplay with first-person viewmodel + procedural animation, hover-physics vehicles, deployables, capture lattice, squads/outfits, continents + sanctuary) but visually built from placeholder primitives and a small CC0 pack. Your job: replace that with an **extremely high-detail, cohesive, futuristic asset suite** that makes the game look shipped.

**You may produce assets two ways, and should mix them:**
1. **Procedural generation** — write Python scripts (numpy + a mesh lib such as `trimesh`, or hand-rolled OBJ writers) that generate the OBJ geometry and PIL/numpy scripts that synthesize the PNG texture sets. This is the preferred path: infinitely revisable, license-clean, and scripts become part of the repo (`Tools/assetgen/`).
2. **Curation** — source CC0-only content (Kenney, OpenGameArt, PolyHaven CC0, ambientCG) and adapt it (retexture to faction palettes, decimate, rename). Every curated file MUST be CC0 and recorded in `Assets/MMOFPS/asset_manifest.json` following its existing entry format (path/source_pack/author/url/license).

**Never** use content under CC-BY, GPL, or unknown license. When in doubt, generate procedurally.

---

## 2. Hard engine contract (violating any of these makes the asset unusable)

| Contract | Value |
|---|---|
| Mesh format | **Wavefront OBJ only.** Static geometry. No rigs, no skinning, no animation data — ALL animation is procedural in-engine (viewmodel sway/recoil, pendulum secondary motion, vehicle hover). Design meshes as articulation-friendly separate OBJs where motion is needed (e.g. turret base + turret head as two files). |
| Mesh must include | Triangulated faces, vertex normals, UVs. One UV set. No negative scaling, no n-gons. |
| Orientation/units | **Match the existing models**: open `Assets/Models/rifle.obj` and `Assets/Models/MMOFPS/vehicles/tank.obj` first, measure their up-axis/forward/scale, and match exactly. Units are meters (a rifle ≈ 1.0 m long, the tank ≈ 6 m). Pivot at object base-center for placeables, at grip for weapons. |
| Textures | **PNG only.** Per material: `*_color.png` (albedo, sRGB), `*_normal.png` (tangent-space, +Y green), `*_roughness.png` (linear gray). Metallic and AO are scalars in the material JSON (0–1) unless you verify the engine's material loader accepts maps for them (check `SparkEngine/Source/Graphics` material code before assuming). |
| Material definition | One JSON per material in `Assets/Materials/MMOFPS/`, matching this exact working example: `{"name":"Terrain_Rock","shader":"PBR","albedo":"Textures/MMOFPS/terrain/rock_color.png","normal":"...","metallic":0.0,"roughness":"Textures/.../rock_roughness.png","ao":1.0,"tiling":[64,64]}` |
| Audio | **WAV only** (16-bit PCM, 44.1 kHz). Loops must be seamless. |
| Scene placement | Scenes are INI-dialect `.scene` files (`Assets/Scenes/MMOFPS/cindral_wastes.scene`) — read it to see how `[Object]` nodes reference models, position, rotation (degrees), scale. New structures must work as scene nodes. |
| Paths | Models → `Assets/Models/MMOFPS/{weapons,vehicles,buildings,props,characters}/`; textures → `Assets/Textures/MMOFPS/<category>/`; audio → `Assets/Audio/MMOFPS/<category>/`; materials → `Assets/Materials/MMOFPS/`. Data table hookups (`weapons.json`, `vehicles.json`, `deployables.json`) reference models by these relative paths — deliver a patch list of the JSON `"model"` fields you change, do not restructure the data files. |
| Collision reality | World collision is auto-built from scene-node AABBs (one box per object). Buildings read as SOLID boxes — design exteriors that read correctly as box colliders; interiors are a future engine feature, so **no enterable interiors yet**. Doorway alcoves ≤ 1 m deep are fine (cosmetic). |
| Budgets | First-person weapons 3k–8k tris, 2048² textures. Vehicles 8k–20k tris, 2048². Buildings 5k–30k tris, 1–2 tiling materials + 1 trim sheet. Props 300–3k tris, 1024². Terrain/structure tiling textures 1024² (they tile 64×). Total new texture footprint ≤ 600 MB. |

---

## 3. World & faction art direction

### The world: Veyra
A resource-strip-mined desert world. **Cindral Wastes** (the live continent, 13 regions): ochre/rust hardpan, glassy wind-polished mesas, salt flats, heat-haze horizon. Palette: desaturated warm neutrals (sand #C2A878, rust #8A5A3C, basalt #4A4440, glass-teal accents in the flats) so **faction colors carry all the saturation**. Structures on Veyra are colonial-industrial: prefab alloy, cable runs, holo-signage, wind-scoured wear on every windward face.

**Sanctuary Haven** (new starting zone): a pristine orbital-elevator plaza — clean white/gray composite, floating holo-UI, warm welcoming light. Deliberate contrast: the war below is dusty; home is clean.

### Faction visual languages (silhouette-first — a player must ID faction at 100 m from shape alone)

| | **Meridian Accord (MRA)** | **Aurum Combine (AUC)** | **Helix Covenant (HLX)** |
|---|---|---|---|
| Identity | Authoritarian colonial remnant. Order at any cost. | Corporate-secessionist mercenaries. Freedom, invoiced monthly. | Transhumanist tech-cult. The flesh remembers; the helix decides. |
| Primary color | Signal red `RGB(199,31,38)` | Cobalt blue `RGB(41,97,217)` | Royal violet `RGB(140,51,204)` |
| Secondary | Gunmetal `RGB(89,92,97)` | Brushed gold `RGB(222,184,51)` | Bio-teal `RGB(51,191,179)` |
| Shape grammar | Heavy rectilinear slabs, 45° chamfers, exposed rivets/bolts, stenciled numerals, slab armor over everything | Sleek aerodynamic panels, thin gold trim lines, chamfered edges, glass-cockpit surfaces, corporate decals | Segmented curves, helical/vertebral repetition, asymmetry, grown-not-built seams, subsurface glow veins |
| Materials | Matte painted steel, worn edges to bare metal, roughness 0.6–0.8 | Satin composite + polished gold accents, roughness 0.3–0.5 | Iridescent ceramic-chitin, emissive teal veins, roughness 0.2–0.9 variance |
| Emissive accent | Thin red strip lights, hard rectangles | Blue-white running lights along trim | Pulsing teal organic veins |
| Weapon read | Boxy receivers, angular muzzle brakes, iron-sight silhouettes | Streamlined bullpups, gold inlay, holo optics | Coiled/ribbed bodies, no visible magazine (bio-feed), glowing coolant lines |

Every faction asset gets a small **faction crest decal** (design 3 crests: MRA eagle-chevron, AUC hex-coin, HLX double-helix eye) delivered as 512² PNG decal sheets with alpha.

---

## 4. THE COMPLETE ASSET MANIFEST

Work in priority batches. Finish and QA a batch before starting the next. Every mesh ships with its texture set + material JSON + (for data-table items) the JSON patch line.

### Batch P0 — Weapons (highest visual payoff: always on screen in first person)
23 weapons exist in `Assets/MMOFPS/Data/weapons.json` under these keys — model each as first-person-quality OBJ (the same mesh is used in-world; keep silhouette strong):
- **MRA:** `mra_rifle`, `mra_carbine`, `mra_lmg`, `mra_sniper`, `mra_pistol`
- **AUC:** `auc_rifle`, `auc_carbine`, `auc_lmg`, `auc_sniper`, `auc_pistol`
- **HLX:** `hlx_rifle`, `hlx_carbine`, `hlx_lmg`, `hlx_sniper`, `hlx_pistol`
- **Common pool:** `np_shotgun`, `np_launcher` (AV rocket), `np_knife`, `tool_repair` (fabricator gun), `tool_med` (med applicator)
- **Mounted:** `col_cannon` (Colossus arm cannon), `veh_ravager_90` (tank 90 mm), `veh_aegis_pdw` (APC point-defense turret — deliver as base+head OBJ pair)

Per weapon also deliver: muzzle-flash sprite sheet (additive PNG, 4 frames), and a small dangling **charm mount point** documented in a README (the engine hangs procedural pendulum charms — mark the lug position in a comment).
Weapon audio per faction family: fire (3 round-robin variants), reload, empty-click, distant-fire tail — WAV, layered synth + noise design is fine, aim for punchy PS2-like weight. (~70 WAV files.)

### Batch P1 — Vehicles + the Colossus
From `vehicles.json` (keep the `"model"` paths or patch them):
- **Drifter** — fast quad. Exposed frame, faction-tint panels. (`vehicles/quad.obj`)
- **Aegis** — 8-seat armored APC + deployable mobile spawn: design a deployed state read (fold-out spawn pylons as separate OBJ shown when deployed). (`vehicles/apc.obj` + `apc_deployed_pylons.obj`)
- **Ravager** — light hover tank; hull + turret as separate OBJs. (`vehicles/tank.obj`)
- **Vulture** — VTOL gunship (currently disabled in data; build it anyway). (`vehicles/vtol.obj`)
- **Colossus** — the 6th infantry class is a heavy exosuit/mech: build a 2.8 m armored frame (single static OBJ; engine animates procedurally), plus its `col_cannon`.
All vehicles: 3 faction texture variants (same mesh, faction palette swap), engine loop WAV + explosion WAV upgrades, hover-dust decal sprite.

### Batch P2 — Structures for the 13 Cindral Wastes regions
Region list (names imply their kit): Skyanchor Aurelia / Cobalt Reach / Violet Gate (the 3 faction **warpgates** — one big shield-dome gate structure each, faction-styled), Dustline Relay, Kiln Overlook, Breaker Quarry, Sunder Pass, Glasswind Flats, Cinder Redoubt, Fluxwell Prime, Mirage Depot, Saltcrown Ridge, Hollowmark Station.
Build a **modular colonial-industrial kit** (neutral, wind-worn, faction-agnostic — ownership is shown by holo-flags):
- Capture-point tower (the flag structure: 12 m pylon + rotating holo-banner ring; banner texture per faction + neutral)
- Prefab barracks block, storage silo cluster, comms relay dish, refinery stack (for Kiln/Fluxwell), quarry excavator gantry, depot warehouse, rail/pipeline segments, watchtower, blast-wall segments + gate, landing pad, holo-sign set
- 12–15 modules total, each 1–3 materials from a shared **structure trim sheet** + 2 tiling alloy textures
- **Sanctuary kit** (5 pieces): elevator plaza core, terminal kiosk (the continent-select terminal — make it iconic, 2.2 m holo-arch), spawn pad, rail balustrade, ambient holo-tree
- **Skybox**: 4096×2048 equirect (or 6×1024 cube faces — check `Textures/MMOFPS/sky/` for the format the engine loads) — Veyra day: high thin cirrus, twin moons, dust-haze horizon; plus a sanctuary variant (orbital view, planet below)

### Batch P3 — Deployables, props, characters, terrain, UI
- **Deployables** (`deployables.json`): `FabTurret`, `FabAmmoPack`, `MedBeacon`, `ResupplyStation`, `AVTurret` (base+head), `ShieldWall` (frame OBJ + additive shield-plane texture) — faction-tinted emissives.
- **Props** (~20): supply crates (3 sizes), barrels, cable spools, antenna masts, rock set (5 wind-carved variants), dead colonial vehicle wreck, scrap piles, holo-map table, ammo printer.
- **Characters**: 6 class bodies × 3 factions is 18 — but the engine renders players from primitives today. Deliver **6 class-silhouette static OBJs** (Ghost light-recon, Striker assault, Medtech, Fabricator, Bulwark heavy, Colossus already in P1), 5–8k tris, A-pose, one 2048² texture with 3 faction palette variants. These replace capsules until skeletal animation lands — silhouette + faction read is what matters.
- **Terrain texture upgrade**: re-generate the 4 terrain sets (sand, rock, gravel, grass→cracked hardpan) at 1024² with matching normal/roughness, seamless at 64× tiling, plus 2 new sets (salt flat, glassed dune) and 512² detail-overlay noise.
- **UI/HUD icon set** (PNG with alpha, 128² and 64²): 23 weapon icons (side profile silhouettes), 5 vehicle icons, 6 class icons, 6 deployable icons, 13 region-type map icons, faction crests (3), directive/medal set (12), map lattice-link arrows, capture-point states (owned/contested/capturing), minimap player/squad/vehicle pips, hit markers, damage-direction chevron, reload/interact prompts. Consistent 2 px stroke, white-on-transparent (engine tints).
- **Ambient audio**: Cindral wind bed (3 loops), distant-battle bed, sanctuary interior hum, capture-point alarm, terminal UI bleeps, footsteps (sand/metal × 4 variants), hover-vehicle dust wash.

---

## 5. Working method (follow exactly)

1. **Recon first**: read `Assets/Models/rifle.obj` + `tank.obj` (orientation/scale), one materials JSON, `cindral_wastes.scene`, and `asset_manifest.json` before generating anything.
2. **Scripts live in `Tools/assetgen/`**, one per asset family (`gen_weapons_mra.py`…), stdlib+numpy+PIL(+trimesh if available), deterministic (seeded), re-runnable. Each script prints the files it wrote.
3. **Per-batch QA gate** before moving on: (a) OBJ loads in-engine — place it in a test scene node and boot `SparkEngine.exe -game SparkGameMMOFPS.dll` windowed, screenshot it; (b) tri count within budget (print from script); (c) textures power-of-two, normal map +Y; (d) manifest + ATTRIBUTION updated for any curated file; (e) data-table `"model"` patches listed in the batch report.
4. **Faction consistency check** at the end of every batch: render/screenshot the batch's assets side by side; verify the 100 m silhouette rule and palette compliance against §3's hex values.
5. **Deliver per batch**: files in place + a `BATCH_REPORT.md` (what shipped, tri/texture stats, JSON patches to apply, known compromises).
6. Commit per batch on the current branch with targeted `git add` paths (never `-A`); message prefix `assets(P<n>):`.

## 6. Taste calibration (what "extremely nice" means here)
- PS2's readability with modern PBR: strong value structure first, material richness second, decoration third.
- Wear tells stories: windward sandblasting, hand-worn grips, heat discoloration at muzzles/exhausts — but keep grunge ≤ 30% of any surface; these are maintained military machines, not derelicts.
- Emissives are the faction jewelry — use them precisely (edges, seams, optics), never as floodlights.
- Repetition with variation: every module/kit piece should tile or cluster without obvious mirroring artifacts.
- When a choice trades silhouette clarity for detail, silhouette wins. Always.
