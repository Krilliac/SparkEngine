# UX Contract

This contract records durable decisions for the shared SparkEditor workspace shell.  It covers dockspace startup and reset, the menu and toolbar chrome, the status bar, semantic notifications, and the shipped theme picker.  It does not certify individual authoring panels, runtime game UX, asset import workflows, or unreleased editor commands.

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
| Shared editor-shell implementation ownership | `SparkEditor/Source/Core/EditorUI.cpp`, `EditorMenuBar.cpp`, `EditorDockLayout.h`, `EditorNotificationManager.cpp` | Runtime implementation | 2026-09-08 |
| Docking persistence | `DESIGN.md` and Dear ImGui saved dock-tree behavior | Workspace-state policy | 2026-09-08 |
| Permission model | Not applicable: this scope has no account/role UI | N/A | 2026-09-08 |
| Deletion, retention, billing, and legal copy | Not applicable: no such workflow is changed by this shell slice | N/A | 2026-09-08 |

## Visual contract

- Project `DESIGN.md`: `DESIGN.md` is the maintained design-context source.
- Token ownership model: existing runtime canonical; this contract does not duplicate palette values.
- Runtime design-system/token source: `EditorTheme.cpp` and `EditorThemeData`.
- Mapping/export/adapters: `EditorTheme::GetCurrentThemeData()` is consumed directly by notifications, toolbar controls, and status rendering.
- Token drift gate: source compilation plus the existing theme inventory/source tests; visual changes require a live five-theme smoke before binary sign-off.
- Supported themes: Ember Studio, Cobalt Forge, Graphite Signal, Professional Light, and High Contrast.  Compatibility aliases are not additional picker entries.
- Design-context owner/review policy: preserve the `DESIGN.md` ownership map; use shared semantic tokens rather than panel-local colors or new themes.

## Canonical UI Map

| Capability | Canonical owner | Source of truth | Allowed variants | Verification |
|---|---|---|---|---|
| Workspace docking | `EditorUI` + `EditorDockLayout` | Saved ImGui dock tree; `DESIGN.md` | Seed only a new/empty tree; explicit destructive reset | `Gated_EditorDockLayout_PreservesExistingTreeUntilExplicitReset` |
| Theme picker | `EditorTheme` + preferences UI | `EditorThemeData`; `DESIGN.md` | Exactly the five shipped themes; compatibility aliases stay hidden | Existing theme inventory tests; source review; live five-theme smoke |
| Menu command availability | `EditorMenuBar` | Real command integration state | Enabled implementation or disabled item with hover explanation | Source search; Release editor source compile |
| Toolbar passive hover | `EditorMenuBar` + `EditorThemeData` | `buttonHovered` token | Theme-derived passive hover; semantic active action colors | Release editor source compile; live theme smoke |
| Status context | `EditorUI` | Measured ImGui content geometry | Full value, icon fallback with hover value, or omitted low-priority context | Release editor source compile; live narrow-width smoke |
| Toast | `EditorNotificationManager` | Shared semantic notification manager | success, info, warning, error; viewport-bounded wrapping stack | `Gated_EditorNotificationManager_WrapsLongMessagesWithoutOverlappingStack` |

No web form, select/listbox, date picker, CRUD, table, browser scrollbar, or native browser dialog capability is introduced by this native editor-shell scope.

## Component behavior

| Component | Default | Hover | Focus | Active | Disabled | Busy | Error |
|---|---|---|---|---|---|---|---|
| Enabled menu command | Semantic theme item | Existing menu hover treatment | Existing keyboard/menu focus behavior | Executes its real command | N/A | Existing command-specific state | Real command reports through its owner |
| Unavailable menu command | Visibly disabled | Descriptive reason via tooltip | Nonactivatable | Never executes | Explanation names the unavailable capability or supported destination | N/A | No false-success toast |
| Toolbar action | Theme `button` token | Theme `buttonHovered` token | Existing ImGui focus behavior | Semantic action color | Existing action-specific policy | Existing play/build owner | Existing action-specific feedback |
| Status context | Full label when it fits | Full value available on fallback icon | Not an action | Current document/connection state | N/A | Live metrics can update | Connection state is semantic success/error color |
| Toast | 56 px minimum, wrapped shared window | N/A | Not interactive in this slice | Semantic accent stripe/icon | N/A | Stack by actual preceding height | Error type uses shared error semantics |

## Dataset navigation

Not applicable to this shell change.  Asset Browser and other data-heavy panels retain their own established owners and are not redefined here.

## Flow ledger

| Operation | Trigger | Pending | Success destination | Success feedback | Failure recovery | Focus outcome | Source ref |
|---|---|---|---|---|---|---|---|
| Seed first workspace | New or empty dock tree | None | Default dock layout | Usable initial panel arrangement | Preserve saved tree instead when one exists | Existing dockspace focus | `EditorDockLayout.h` |
| Reset workspace layout | Explicit Reset to Default Layout | Next frame rebuild request | Default dock layout | Layout changes only after explicit request | Retry explicit reset; no startup reset side effect | Existing dockspace focus | `EditorUI.cpp` |
| Request unavailable menu command | Hover the disabled command | None | No state transition | Tooltip explains availability | Use the named supported destination when one exists | Menu remains open under normal ImGui behavior | `EditorMenuBar.cpp` |
| Publish editor feedback | Shared owner calls notification manager | Existing lifetime handling | Top-right viewport stack | Semantic wrapped toast | Later feedback remains visible below actual toast height | No focus steal | `EditorNotificationManager.cpp` |
| Change theme | Existing theme picker action | Existing theme application | Current workspace | Shared token update | Re-select any shipped theme | Existing editor focus | `EditorTheme.cpp` |

## Navigation and responsive behavior

- Route document title policy: not applicable; SparkEditor is a native desktop application.
- Route error / 403 page behavior: not applicable; this shell change has no web routing or account roles.
- Breadcrumb/tab/route-state policy: docked panel state is preserved by the existing ImGui layout state.
- Sidebar/drawer/bottom-sheet transformation: not applicable; panels remain dockable desktop windows.
- Responsive table strategy: not applicable in this shell scope.
- Truncation/full-value access: status text is measured first.  Long project or scene labels fall back to an icon with a hover tooltip; lower-priority tool/selection text is omitted before metrics are allowed to overlap.
- Focus restoration and sticky-obstruction policy: the shell change adds no focus-stealing overlay; notifications do not steal focus.

## Overlays and feedback

- Dialog primitive: existing app-owned Dear ImGui dialogs remain the owner; no native browser dialog is introduced.
- Destructive confirmation levels: unchanged and outside this shell slice.
- Toast placement/duration/deduplication: the shared manager owns semantic toasts in the main viewport's top-right work area.  This change establishes viewport-bounded width, wrapped height, and actual-height stacking; it does not change the manager's existing lifetime or deduplication policy.
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
- Current risk-prioritized slice: shared shell feedback, saved docking preservation, and theme-semantic toolbar behavior.
- Rollout/rollback: these changes are source-local and reversible by a normal commit revert; no persistent data migration is added.

## Verification

- Required static commands: `git diff --check`; the focused SparkTests below; Release editor source compile; frontend contract audit in strict mode.
- Browser/device/locale matrix: not applicable to this native editor shell.  A desktop live smoke covers all five themes, disabled-command hover help, long stacked toasts, saved docking, and narrow status rendering.
- Accessibility checks: compile-time/token review plus live keyboard/High Contrast smoke before binary sign-off.
- Canonical sibling flow used for comparison: existing `DESIGN.md` ownership and shared `EditorThemeData`/`EditorNotificationManager` behavior.
- Component-state coverage: `Gated_EditorDockLayout_ToolbarUsesContentHeightAndKeepsPanelsSeparate`; `Gated_EditorDockLayout_PreservesExistingTreeUntilExplicitReset`; `Gated_EditorNotificationManager_WrapsLongMessagesWithoutOverlappingStack`.
- Current source evidence: the dock tests passed (2 tests, 24 assertions); the notification test passed (1 test, 4 assertions); `SparkEngine.vcxproj /t:ClCompile /p:Configuration=Release /m:2` completed successfully after this change.
- Remaining binary evidence: SparkEditor is already open, so its executable was intentionally not relinked.  Close and rebuild it before the live smoke; source-level verification does not substitute for that binary-level check.
