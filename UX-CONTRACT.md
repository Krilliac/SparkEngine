# UX Contract

This contract records durable decisions for the shared SparkEditor workspace shell.  It covers dockspace startup and reset, named layouts, the menu and toolbar chrome, the status bar, semantic notifications, the Start page, the command palette, and the View > Appearance window.  It does not certify individual authoring panels, runtime game UX, asset import workflows, or unreleased editor commands.

## Product context

- Audience: scene authors and engine developers using a desktop editor.
- Primary jobs: open or continue a project, arrange a working layout, understand whether a command is available, monitor scene context, and act on build/play/editor feedback.
- Target market(s): desktop engine users; no country-specific behavior is introduced by this shell change.
- Active locales: existing English editor copy.  New strings remain ordinary UTF-8 and use existing icon/text conventions.
- Language/content register and native-review policy: concise technical English; localization review is required before adding a new locale, but no locale bundle is changed here.
- Timezone/calendar policy: no date/time control is part of this editor-shell scope.
- Accessibility target: WCAG 2.2 AA principles where applicable to a native Dear ImGui surface; this is not a claim of formal web accessibility conformance.  The concrete commitments are keyboard-operable enabled commands, nonactivatable disabled commands, theme-token contrast including High Contrast, and full-value hover fallback where labels are compressed.

## Business-context sources

| Domain / scope | Authoritative source | Source type | Reviewed date |
|---|---|---|---|
| Workspace visual and interaction rules | `DESIGN.md` | Product design authority | 2026-09-08 |
| Shared editor-shell implementation ownership | `SparkEditor/Source/Core/EditorUI.cpp`, `EditorMenuBar.cpp`, `EditorDockLayout.h`, `EditorNotificationManager.cpp`, `EditorAppearanceWindow.cpp`, `EditorStartPage.cpp`, `EditorLayoutManager.cpp` | Runtime implementation | 2026-10-14 |
| Ember Signal tokens, metrics and widgets | `SparkUI/` (`Spark::Ui` roles, `Px`/`Em` metrics, kit widgets) | Shared design system | 2026-10-14 |
| Docking persistence | `DESIGN.md` and Dear ImGui saved dock-tree behavior | Workspace-state policy | 2026-09-08 |
| Permission model | Not applicable: this scope has no account/role UI | N/A | 2026-09-08 |
| Deletion, retention, billing, and legal copy | Not applicable: no such workflow is changed by this shell slice | N/A | 2026-09-08 |

## Visual contract

- Project `DESIGN.md`: `DESIGN.md` is the maintained design-context source.
- Token ownership model: existing runtime canonical; this contract does not duplicate palette values.
- Runtime design-system/token source: `EditorTheme.cpp` and `EditorThemeData`, published to the shell through the `SparkUI` role table.
- Mapping/export/adapters: shell chrome (toasts, toolbar, status bar, Start page, command palette, Appearance window) reads `Spark::Ui::Col(Role)` and sizes through `Spark::Ui::Px`/`Em`, so palette, appearance, density and UI scale apply without per-surface colors or pixel literals.  `tools/ui/check_ui_literals.py` ratchets raw literals per lane (L2 baseline 0).
- Token drift gate: source compilation plus the existing theme inventory/source tests; visual changes require a live five-theme smoke before binary sign-off.
- Supported themes: three palette families (Ember Studio, Cobalt Forge, Graphite Signal), each in Dark, Light or High Contrast appearance, or Match Windows.  Professional Light and High Contrast load as Ember Studio in the Light and High Contrast appearances.  Compatibility aliases are not additional picker entries.
- Design-context owner/review policy: preserve the `DESIGN.md` ownership map; use shared semantic tokens rather than panel-local colors or new themes.

## Canonical UI Map

| Capability | Canonical owner | Source of truth | Allowed variants | Verification |
|---|---|---|---|---|
| Workspace docking | `EditorUI` + `EditorDockLayout` | Saved ImGui dock tree; `DESIGN.md` | Seed only a new/empty tree; explicit destructive reset | `Gated_EditorDockLayout_PreservesExistingTreeUntilExplicitReset` |
| Appearance window (View > Appearance; Help > Themes... alias) | `EditorAppearanceWindow` + `EditorPreferences` | Persisted preferences file; `EditorThemeData` | Palette family; Dark / Light / High Contrast / Match Windows; density; UI scale Auto or 75-200 % in 12.5 % steps; always-visible keyboard focus; reduce motion; status bar toggle; contrast-audit badge | Existing theme inventory tests; source review; live palette x appearance smoke |
| Named layouts | `EditorLayoutManager` + toolbar Layout dropdown | `<name>.ini` dock tree plus `.ini.meta` | Default, saved layouts, Save Layout As..., Delete... (confirmed), Reset | `EditorLayoutManager_*` SparkTests |
| Start page | `EditorStartPage` | Preferences (`showStartPage`, recent scenes) | Dockable tab in the centre node; first run or while Show at startup is on; never in test mode | Source review; live first-run smoke |
| Command palette | `CommandPalette` | `EditorCommandRegistry` | Scrim-backed overlay; category badge, name, key hint | Source review; live smoke |
| Menu command availability | `EditorMenuBar` | Real command integration state | Enabled implementation or disabled item with hover explanation | Source search; Release editor source compile |
| Toolbar passive hover | `EditorMenuBar` + `EditorThemeData` | `buttonHovered` token | Theme-derived passive hover; semantic active action colors | Release editor source compile; live theme smoke |
| Status bar | `EditorUI` | Viewport side bar reserved before the dockspace | PREVIEW or EDIT state; connection, project, scene chips; tool/object/selection counts; FPS and ms; notification bell; UI scale % | Release editor source compile; live narrow-width smoke |
| Toast | `EditorNotificationManager` | `NotificationSeverity` + `Notify()` | success, info, warning, error; bottom-right wrapping stack; at most 4 visible; 50-entry history | `Gated_EditorNotificationManager_WrapsLongMessagesWithoutOverlappingStack` |

No web form, select/listbox, date picker, CRUD, table, browser scrollbar, or native browser dialog capability is introduced by this native editor-shell scope.

## Component behavior

| Component | Default | Hover | Focus | Active | Disabled | Busy | Error |
|---|---|---|---|---|---|---|---|
| Enabled menu command | Semantic theme item | Existing menu hover treatment | Existing keyboard/menu focus behavior | Executes its real command | N/A | Existing command-specific state | Real command reports through its owner |
| Unavailable menu command | Visibly disabled | Descriptive reason via tooltip | Nonactivatable | Never executes | Explanation names the unavailable capability or supported destination | N/A | No false-success toast |
| Toolbar action | Segmented controls for tool and space; accent-filled Play Control button; Snap icon button with a Mono step field | Shared hover role | ImGui keyboard focus; always visible when the preference is on | Selected segment uses the accent role | Snap step field disabled while snapping is off | Preview chip while the play session runs | Existing action-specific feedback |
| Status bar | Chips and Mono metrics on the side bar | Tooltips carry frame number, asset count and full chip values | Bell and scale % are buttons | Bell opens notification history; scale % resets to Auto | N/A | Live metrics update every frame | FPS below 30 uses warning, below 15 uses error |
| Toast | Severity glyph and word, clock, wrapped title and body | Hover pauses the countdown | Focus pauses the countdown; Dismiss button | Dismiss removes the toast | N/A | Info/Success 4 s, Warning 8 s; fade only when Reduce motion is off | Error persists until dismissed |
| Appearance window | Current preferences selected | Swatches and audit failures in tooltips | Keyboard-operable radios, combo, checkboxes | Each change applies and saves immediately | N/A | N/A | Audit badge reports failing token pairs |

## Dataset navigation

Not applicable to this shell change.  Asset Browser and other data-heavy panels retain their own established owners and are not redefined here.

## Flow ledger

| Operation | Trigger | Pending | Success destination | Success feedback | Failure recovery | Focus outcome | Source ref |
|---|---|---|---|---|---|---|---|
| Seed first workspace | New or empty dock tree | None | Default dock layout | Usable initial panel arrangement | Preserve saved tree instead when one exists | Existing dockspace focus | `EditorDockLayout.h` |
| Reset workspace layout | Explicit Reset to Default Layout | Next frame rebuild request | Default dock layout | Layout changes only after explicit request | Retry explicit reset; no startup reset side effect | Existing dockspace focus | `EditorUI.cpp` |
| Request unavailable menu command | Hover the disabled command | None | No state transition | Tooltip explains availability | Use the named supported destination when one exists | Menu remains open under normal ImGui behavior | `EditorMenuBar.cpp` |
| Publish editor feedback | Shared owner calls `Notify()` or `ShowNotification()` | Severity-default lifetime | Bottom-right viewport stack | Semantic wrapped toast, also kept in history | Older feedback stays readable in the status bar history | No focus steal | `EditorNotificationManager.cpp` |
| Change appearance | View > Appearance, Help > Themes..., or palette commands | None | Current workspace | Shared token update and saved preferences | Reset to defaults (keeps recent scenes and last layout) | Appearance window keeps focus; Esc closes | `EditorAppearanceWindow.cpp` |
| Change UI scale | Ctrl+= / Ctrl+- / Ctrl+0, View > UI Scale, or status bar scale % | None | Current workspace | Fonts and metrics rebuild at the new scale | Ctrl+0 returns to Auto (DPI only) | Unchanged | `EditorUI.cpp` |
| Save layout as | Toolbar Layout > Save Layout As... or Window menu | Modal with name validation | Named layout listed in the dropdown | Success toast | Invalid names are explained inline; existing names require Replace | Modal returns focus to the workspace | `EditorLayoutManager.cpp` |
| Delete layout | Toolbar Layout > Delete... | Destructive confirmation (Cancel default) | Layout and its companions removed | Success toast | Failure toast names the reason | Workspace | `EditorLayoutManager.cpp` |
| Open from Start page | Start page action or recent scene | Existing unsaved-changes transition | Scene, new scene, project folder, or reset layout | Existing scene-load feedback | Missing recent scene reports through the scene loader | Start page closes on Esc | `EditorStartPage.cpp` |

## Navigation and responsive behavior

- Route document title policy: not applicable; SparkEditor is a native desktop application.
- Route error / 403 page behavior: not applicable; this shell change has no web routing or account roles.
- Breadcrumb/tab/route-state policy: docked panel state is preserved by the existing ImGui layout state.
- Sidebar/drawer/bottom-sheet transformation: not applicable; panels remain dockable desktop windows.
- Responsive table strategy: not applicable in this shell scope.
- Truncation/full-value access: the status bar is a two-column table, so metrics keep their column; chip values that do not fit remain available in tooltips.
- UI scale: every shell measurement goes through `Spark::Ui::Px`/`Em`; the default dock layout is built with `Spark::Ui::Scale()`.
- Focus restoration and sticky-obstruction policy: notifications and the Start page do not steal focus from an active edit; the command palette and layout dialogs are the only focus-taking overlays and close on Esc.

## Overlays and feedback

- Dialog primitive: existing app-owned Dear ImGui dialogs remain the owner; no native browser dialog is introduced.
- Destructive confirmation levels: deleting a named layout uses `Spark::Ui::ConfirmDestructive` (Cancel has default focus, Esc cancels).
- Toast placement/duration/deduplication: toasts stack upward from the bottom-right work area, at most four visible.  Info and Success last 4 s, Warning 8 s, Error persists; hover or focus pauses the countdown; fading is skipped when Reduce motion is on.  The last 50 entries stay in the status bar history.
- Alert/banner scope and persistence: unchanged.
- Tooltip delay/dismissal: existing ImGui behavior; unavailable commands and compressed status icons use descriptive tooltips.
- Unsaved-changes behavior: unchanged; reset-layout changes only dock geometry and remains explicit.
- Layer/z-index contract: existing ImGui modal/dialog layering remains above normal dockspace; notification windows stack within their shared viewport region.

## Async and resilience

- Mutation default, idempotency, auto-save, offline behavior, retries, conflict handling, session expiry, and long-running job progress are not changed by this native shell pass.
- Notification resilience commitment: a long message must not overlap a subsequent notification; the focused ImGui test exercises this geometry.
- The unresolved asset-recovery and runtime workflow gates remain release-ledger work, not claims made by this contract.

## Validation

No form validation surface is modified.  Browser `noValidate`, first-invalid focus, and browser-specific validation recovery do not apply to this native ImGui shell scope.

## Permission and clipboard

- Permission UI strategy: no account/role permission model is changed.
- Clipboard copy policy: clipboard actions without a real editor implementation are disabled rather than pretending to copy.
- Disabled-state explanation: use the shared unavailable-menu presentation and a concise hover reason.

## Migration status

- Migration ledger location: the release readiness ledger remains authoritative for broader editor gaps.
- Canonical primitives and owners: the Canonical UI Map above.
- Current risk-prioritized slice: Ember Signal shell (Appearance window, Start page replacing the welcome modal, side-bar status bar, toasts with history, named layouts, scale shortcuts) on top of saved docking preservation.
- Rollout/rollback: these changes are source-local and reversible by a normal commit revert; no persistent data migration is added.

## Verification

- Required static commands: `git diff --check`; the focused SparkTests below; Release editor source compile; frontend contract audit in strict mode.
- Browser/device/locale matrix: not applicable to this native editor shell.  A desktop live smoke covers each palette in each appearance, UI scale 75 % and 200 %, disabled-command hover help, long stacked toasts, saved and named layouts, the Start page, and narrow status rendering.  `tools/test-editor-live.py --ui-scale` keeps its click targets aligned at non-default scales.
- Accessibility checks: compile-time/token review plus live keyboard/High Contrast smoke before binary sign-off.
- Canonical sibling flow used for comparison: existing `DESIGN.md` ownership and shared `EditorThemeData`/`EditorNotificationManager` behavior.
- Component-state coverage: `Gated_EditorDockLayout_ToolbarUsesContentHeightAndKeepsPanelsSeparate`; `Gated_EditorDockLayout_PreservesExistingTreeUntilExplicitReset`; `Gated_EditorNotificationManager_WrapsLongMessagesWithoutOverlappingStack`; `EditorLayoutManager_SaveAsCreatesNamedLayoutAndListsIt`; `EditorLayoutManager_SaveAsRejectsInvalidNamesWithReason`; `EditorLayoutManager_DeleteRemovesDockTreeCompanions`; `EditorLayoutManager_DeleteCurrentLayoutResetsCurrentName`.
- Current source evidence (2026-10-14, Ember Signal wave 3): clang-format clean, literal ratchet at 0 for the shell lane, play-truth and world-mutation source pins pass.  No build or test run was made for this revision.
- Remaining binary evidence: the SparkEditor and SparkTests builds, the focused tests above, and the live smoke are still required; source-level verification does not substitute for them.
