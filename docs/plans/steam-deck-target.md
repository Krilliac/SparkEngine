# Future Target: Steam Deck / SteamOS (DECK-100)

**Created:** 2026-09-25  
**Status:** Proposed. This is future work outside the `stable-v1` profile and outside the readiness work-item ledger (`docs/readiness/work-items/`). It is not scheduled, and nothing here is a support claim.  
**Owner:** Krilliac (unassigned for implementation)

## Why this target

Console-class hardware is otherwise reachable only through NDA-gated platform
programs (PlayStation Partners, ID@Xbox, Nintendo Developer Portal). OD-12 defers
that work, and PLT-250 keeps it gated. Community console SDKs are not an option:
they run only on modified consoles and cannot ship. Steam Deck is the one
console-class device SparkEngine can target in-house. It runs SteamOS (Linux)
on an AMD APU with Vulkan drivers, and it needs no platform NDA or devkit, only
a Steamworks partner account for store publishing.

## Two routes, both evaluated

1. **Proton (Windows build).** Run the `stable-v1` Windows D3D11 package under
   Proton, which translates D3D11 through DXVK. This needs little engine work,
   but correctness depends on DXVK and Proton behaviour that SparkEngine does not
   control.
2. **Native Linux.** Build the Linux package with the Vulkan backend and SDL2
   windowing/input. This builds on existing experimental work, and the Linux
   readiness items such as PLT-210 and RHI-230 must reach windowed release
   quality first.

Pick the route after measuring both on hardware. Keep Proton as the fallback if
native parity lags.

## Scope

- A `steamdeck` build and package configuration: native Linux x64 with Vulkan as
  the primary renderer. The documented Proton launch path is kept as an
  alternative.
- Gamepad-first input. The default controller mapping covers every action, and
  the Steam Input API or SDL game-controller mappings need no keyboard or mouse.
  Text entry goes through the on-screen keyboard.
- 1280×800 (16:10) as the default resolution. The UI and fonts stay legible at
  the Deck's display density, and a 16:10 layout needs no letterboxing.
- Suspend/resume: pause simulation and audio, survive a GPU device loss or
  context reset, and keep networking sessions in a known state.
- Power and thermals: a Deck preset with frame caps (30/40/60) and quality
  scaling, reusing the battery-aware scaling from the mobile layer where it fits.
- Storage: saves and config live under the XDG data/config directories, with
  optional Steam Cloud paths declared, so nothing is written next to the binary.
- No launcher, installer or first-run window step that needs a mouse.

## Acceptance (to be promoted into a readiness work item when scheduled)

1. The SparkGameFPS package builds for the target configuration from a clean
   checkout. It launches from Steam in Gaming Mode and reaches gameplay using
   only the controller.
2. It meets Valve's current Steam Deck compatibility review criteria for input,
   display legibility, seamlessness, and system support, checked on real
   hardware and recorded with the build ID.
3. It holds a stable 30 fps minimum at the Deck preset in the FPS vertical
   slice, with frame-time and power logs captured on hardware.
4. Ten suspend/resume cycles during gameplay and at menus complete with no
   crash, device loss or save corruption.
5. Saves round-trip across a relaunch and a SteamOS reboot, under the XDG
   paths.
6. The chosen route (native or Proton) is justified by measured results, and
   the other route's status is documented.

## Dependencies on current work

- `PLT-210` (Linux windowed/headless runtime without silent NullRHI downgrade).
- `RHI-230` (shipped GLSL→SPIR-V and fail-closed Vulkan validation).
- `SAVE-230` (atomic saves and versioned migration).
- `ASSET-220` (Linux installed package consumer lane).
- The `stable-v1` Windows package, for the Proton route.

## Out of scope

- PlayStation, Xbox and Nintendo consoles (PLT-250, OD-12).
- Store publishing and Steamworks integration beyond what the acceptance items
  need (achievements, overlays and Steam Cloud sync are later work).
- Other handheld PCs. They follow once the Deck preset exists, but are not part
  of this item.

## Risks

- Hardware access: acceptance needs a physical Deck. CI can only cover the
  build, the headless runtime and emulated input.
- DXVK/Proton regressions can break the Proton route without any engine change.
- The native Vulkan backend is experimental today. Parity with the D3D11
  `stable-v1` rendering is the largest cost.
