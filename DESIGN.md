---
version: alpha
name: SparkEditor
description: A viewport-first desktop authoring workspace with compact studio controls.
colors:
  background: "#1a1716"
  surface: "#24201e"
  border: "#3c3733"
  text: "#efeeeb"
  primary: "#f1823a"
  secondary: "#68d3ee"
typography:
  sans:
    fontFamily: "IBM Plex Sans, Roboto, sans-serif"
omitted:
  - section: rounded
    reason: Per-theme geometry remains canonical in EditorThemeData.
  - section: spacing
    reason: Native ImGui metrics and available panel space determine geometry.
  - section: components
    reason: Native component ownership and states are documented below.
---

# SparkEditor Design System

## Overview

Approved direction, 2026-09-07: a dense Unreal/Unity-like editor with the direct
asset-editing utility of SLADE. This is a desktop production tool, not a landing
page. Scene authors need persistent context, legible property controls, and a
large working viewport. Avoid oversized cards, decorative empty bands, and
multiple near-identical presets. No new market or localization scope is implied
by this English-language editor pass.

The signature is the warm Ember workspace with restrained cyan data highlights.
Expression belongs in accents; geometry and operation stay consistent across
themes. The existing C++ runtime owns tokens (not generated CSS):
`EditorTheme.cpp` palette → `EditorThemeData` semantic roles → `ApplyToImGui`
and `GetCurrentThemeData()` → widgets, toolbar, status bar, notifications.
Frontmatter mirrors the accepted Ember palette, not a second runtime source.

## Colors

Expose Ember Studio, Cobalt Forge, Graphite Signal, Professional Light, and High
Contrast. Retired built-in names are compatibility aliases, not extra picker
entries. Preserve explicit custom registrations. Unknown names select Ember.
Success, warning, error, selection, and keyboard focus retain their semantic
roles in every palette; never communicate state through color alone.

## Typography

`EditorFonts::LoadFonts` owns startup font loading: IBM Plex Sans with Roboto
fallback, semibold/bold for emphasis, and the existing utility font for technical
data. Theme switching does not reload the atlas. Keep readable numerical fields
and labeled axis controls; do not shrink text to conceal overflow.

## Layout

The approved target is a compact toolbar above the dockable workspace, hierarchy
left, scene/game viewport center, inspector right, and assets/console below.
Toolbar height should follow its controls, not grow as a percentage of screen
height. Preserve user docking and explicit layout reset behavior.

Shared property controls must account for all gaps and button widths. At narrow
widths, put labels above controls and stack axes when necessary instead of
clipping fields or creating unusably small numeric inputs. Preserve scrolling.
These layout targets require rendered verification; this document is not evidence
that every existing panel already meets them.

## Elevation & Depth

Use tonal surfaces and thin boundaries for hierarchy. Keep the viewport dominant.
Dialogs and notifications may overlay the workspace; startup project selection
and welcome flows must not compete as stacked modals.

## Shapes

Use the active theme's rounding and border values. Avoid panel-local decorative
shapes. Axis reset buttons remain visibly associated with their numeric fields.

## Components

- `EditorTheme` owns palette resolution and native ImGui style mapping.
- `EditorMenuBar` owns toolbar action groups; `EditorUI` owns docking and startup.
- `InspectorPanel::DrawVec3Control` is the shared XYZ field renderer, including
  reflection consumers. Geometry fixes belong here, not in individual components.
- `EditorNotificationManager` owns semantic toasts; dialogs must preserve unsaved
  scene data and existing save/cancel outcomes.
- ImGui supplies keyboard navigation, active/hover/disabled widget states, and
  scrollbars. Keep those behaviors when refining visuals. Existing FontAwesome
  icons and drawn gizmos retain tooltips and recognizable action labels.
- Motion should communicate state, never decorate routine editing. Preserve
  immediate local feedback and avoid moving controls during actions.

## Do's and Don'ts

- Do verify actual rendered bounds at narrow and wide panel sizes.
- Do test theme aliases separately from the visible picker inventory.
- Do preserve scene edits, undo, reset actions, and keyboard input.
- Don't equate a palette test with a polished editor or release readiness.
- Don't broaden the theme list to accommodate one panel's styling needs.
