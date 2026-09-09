# EDT-210: Asset Authoring, Recovery, and Package Round-Trip Design

## Status and scope

This is the architecture record for the editor-owned portion of release-blocking work item `EDT-210`.  It implements the stable-v1 authoring path without claiming that panel count or a cosmetic thumbnail closes the gate.  The user has standing authorization to continue the release-readiness work; this design does not authorize a push, tag, release publication, or any account-owner action.

`EDT-210` remains open until its package-side dependency `ASSET-220` supplies a clean author-to-installed-runtime scenario.  This design therefore includes the integration contract and tests needed to close both sides rather than silently treating the editor slice as the whole work item.

## Current facts

- `TextureProcessor`, `MeshProcessor`, and `AudioProcessor` currently return `false` from `GenerateThumbnail`.
- `AssetBrowserPanel` renders generic file icons and emits no ImGui asset drag payload.
- World-backed inspector edits are command-backed through `EditorUI::RecordAppliedDocumentMutation`, but no typed asset consumer exists.
- `EditorCrashHandler` persists a hand-written JSON-like `RecoveryData` format, but no `RecoveryCallback` is registered, no editor mutations call `RecordOperation`, and `ShowRecoveryDialog()` is never rendered.
- `EditorCrashHandler`'s background autosave thread must not serialize the live `World`; it would race editor-thread mutation.  Recovery persistence must be captured on the editor thread.
- `EditorUI` already uses `Spark::SerializeWorld`, `Spark::DeserializeInto`, and `SwapWorld` for lossless document handoff.  Recovery must reuse this path rather than invent a second scene serializer.

## Goals

1. Give every editor asset flow one safe identity: a validated project-relative UTF-8 reference plus an explicit type.
2. Show actual supported asset content in the Asset Browser, not only a file-extension icon.
3. Accept an asset only at compatible editor targets and commit the resulting World mutation through the existing undo/redo boundary.
4. Persist a versioned, atomic, restorable unsaved-scene snapshot without reading the live World from a worker or crash handler.
5. Prove a clean author -> save -> cook/package -> installed-runtime path using the same reference form emitted by the editor.

## Non-goals

- Rebuilding every specialized editor panel.
- A cloud asset service, external catalog, or generated placeholder content.
- Support for arbitrary proprietary media codecs.  Unsupported source formats must be visibly rejected or use an explicit fallback state; they must not be presented as rendered previews.
- Silent recovery, automatic overwriting of a saved scene, or a second independent document model.

## Considered approaches

### A. Canonical asset reference + live preview + cached recovery snapshot (recommended)

Use one typed, project-relative asset reference at the browser boundary; render supported content through the editor's existing graphics/audio paths; apply drops through the document command boundary; capture an immutable recovery envelope on the UI thread and atomically persist it.  This is the smallest approach that satisfies the ledger's end-to-end and failure-recovery requirements.

### B. Extend the current string fields and generic icon grid

Add an untyped path payload and teach the existing recovery dialog to load the current layout.  This is cheaper, but it leaves invalid target assignment, generic thumbnails, unsafe/partial recovery data, and the author-to-package identity mismatch unresolved.  Reject.

### C. Offline Blender-generated thumbnails and a separate recovery format

Render every mesh externally and persist a parallel scene representation.  It expands tools, assets, and release dependencies without making runtime asset resolution more trustworthy.  Reject for stable-v1.  Blender remains an optional content-authoring tool, not the editor's runtime preview authority.

## Architecture

### 1. Canonical asset identity and drag payload

Add a small editor-owned asset-reference module, for example `AssetPipeline/EditorAssetReference.{h,cpp}`:

```cpp
enum class EditorAssetKind : uint8_t { Texture, Mesh, Material, Audio, Unknown };

struct EditorAssetReference {
    EditorAssetKind kind;
    std::string projectRelativePath; // UTF-8, normalized below the active Assets root
};
```

The module owns:

- extension-to-kind classification for the stable-v1 supported formats;
- canonicalization against the active project Assets directory;
- rejection of absolute paths, empty paths, embedded NULs, `..` traversal, and paths outside that root;
- conversion to the runtime's project-relative forward-slash representation; and
- versioned binary encode/decode helpers for ImGui payloads.

The payload type is `SPARK_EDITOR_ASSET_REF`.  It contains a fixed header (`version`, `kind`, `pathByteCount`) followed by the UTF-8 path bytes.  The source passes only the encoded byte buffer to `SetDragDropPayload`; it never exposes an owning `std::string`, raw filesystem pointer, or host-absolute path.  Decode rejects malformed sizes, unsupported versions, invalid UTF-8, or paths that no longer satisfy the project-root rule before any target reads it.

`AssetBrowserPanel` owns source selection and payload publication.  A target owns its mutation.  The browser must not know the selected entity, alter a `World`, or write an arbitrary filesystem path.

### 2. Preview service

Add an editor-owned preview cache bound to the non-owning `GraphicsEngine` that `EditorUI::SetGraphicsDevice` already provides to `AssetBrowserPanel`.

- Textures: resolve the canonical reference through the live asset resolver and display the resulting D3D shader-resource view.
- Meshes: render a bounded off-screen turntable/card using the same supported mesh/material path as Scene View, then display its render target as the grid thumbnail.
- Audio: draw a waveform for supported decoded audio (initial stable-v1 fixture uses WAV); unsupported codecs render a clearly labelled unavailable-preview state rather than a generic success-looking thumbnail.
- Materials: preview their resolved albedo/definition through the existing basic-material resolution path; an unresolved material has a visible error state.

The cache is keyed by canonical reference plus source modification time/size.  It has explicit invalidation after import, project switch, graphics-device reset, and panel shutdown, and releases D3D resources before the device owner is torn down.  It never writes generated thumbnails into a project as a side effect of browsing.

### 3. Compatible, undo-safe drop targets

Asset drops are explicit contracts, not a global "drop any file anywhere" behavior:

| Asset kind | Compatible stable-v1 target | Mutation |
| --- | --- | --- |
| Mesh | Selected World's `MeshRenderer.meshPath`; Scene View surface to create a new mesh entity | Assign/create through `RecordAppliedDocumentMutation` |
| Material | Selected World's `MeshRenderer.materialPath` | Assign through `RecordAppliedDocumentMutation` |
| Texture | Reflected texture-path fields such as `SpriteRenderer`, `DecalComponent`, and material texture slots | Assign through `RecordAppliedDocumentMutation` |
| Audio | A supported `AudioSourceComponent` reference field after resolving its runtime sound identity | Assign through `RecordAppliedDocumentMutation` |

Every target decodes and validates the reference before opening a command.  A type mismatch, missing selected entity, unresolved file, or failed command emits a clear error notification and leaves the World and history unchanged.  A successful application creates exactly one undoable command, records the operation description for recovery diagnostics, marks the document dirty, and survives save/reopen.

The legacy `SceneFile` inspector remains isolated: it may expose its own compatible fields, but it cannot mutate the live World behind the command boundary.  The release test path targets the live World-backed inspector and Scene View.

### 4. Durable recovery

Replace the handwritten `RecoveryData` persistence with an editor recovery envelope serialized through `Spark::Json` and written atomically in the per-user editor-data directory:

```text
schemaVersion
projectIdentity / project-relative scene path
current scene display name
serialized World document (Spark::SerializeWorld output)
saved ImGui/window-layout state reference
recent command descriptions
captured timestamp and dirty sequence
```

Capture rules:

1. The UI thread takes a snapshot only when the document is dirty and the configured interval or mutation sequence requires it.
2. Serialization uses `Spark::SerializeWorld` while the UI thread owns the World, producing an immutable payload.
3. The store writes a temporary sibling, validates bounded JSON, then atomically replaces the prior envelope while retaining a last-known-good backup.
4. The crash handler's callback returns only the most recent immutable envelope; it never walks the live World from its autosave/crash context.
5. Clean shutdown clears recovery only after normal save/close succeeds.  Save failure, abnormal shutdown, or a malformed candidate never destroys the last known good recovery record.

At startup, `EditorUI` checks the store after project/layout services exist and before normal authoring begins.  `Render()` owns a persistent modal state machine: **Restore**, **Discard**, and error/retry.  Restore validates the envelope and project boundary, deserializes into a fresh `World`, then invokes `SwapWorld` only after the entire document is valid.  It clears recovery data only after a successful swap.  Discard clears the record explicitly.  Failure keeps the current document and the recovery record intact, with an error message and retry/discard choices.  There is no silent restore.

### 5. Author-to-installed-runtime contract

The editor emits only the relative references accepted by the runtime asset resolver.  The integration fixture must:

1. create/open a project containing a supported mesh, material/texture, and audio fixture;
2. use the typed asset path to assign them to a live World;
3. save and reopen the scene, verifying reference and component identity;
4. cook/package through the `ASSET-220` canonical path;
5. launch the installed/headless runtime and verify the packaged scene resolves the same assets without reaching the authoring tree.

The final test must run in `editor-integration` and `editor-package-roundtrip` CI jobs on the exact release SHA.  A host-only editor test is evidence for the editor slice, not proof that `EDT-210` is closed.

## Failure and security rules

- All user-controlled paths are canonicalized below the active project assets root before preview, drop, save, or recovery use.
- Recovery parsing has bounded size/depth and rejects a foreign project identity or malformed `SerializeWorld` payload.
- An incomplete temp file, malformed primary record, or write failure preserves the previous good recovery snapshot.
- Preview failures do not block browsing or mutate project files.
- Drop failure is atomic: no partial component update, command entry, or recovery clear.
- Device loss/project close invalidates previews and payload targets before resources or project roots are released.

## Test plan and evidence

### Unit and ImGui integration tests

- `EditorAssetReference_*`: classification, canonicalization, payload round-trip, malformed payloads, traversal rejection, UTF-8 and length limits.
- `EditorAssetDrag_*`: source payload publication; compatible and incompatible target behavior; one command per successful drop; undo/redo restores both prior and new references.
- `EditorPreview_*`: supported texture/mesh/audio/material result or explicit unavailable/error state; project/device invalidation.
- `EditorRecovery_*`: quoted/Unicode state, dirty sequence capture, atomic primary/backup behavior, corrupted record recovery, failed restore preserving current World, explicit discard, clean-shutdown clearing.
- `EditorDocument_*`: every new drop path calls `RecordAppliedDocumentMutation` and produces no direct mutation outside the command stack.

### End-to-end tests

- `EditorCookPackage_*`: author project -> save -> reopen -> cook/package -> installed/headless runtime asset resolution.
- `ctest --test-dir build/windows-shipping -L editor-integration --output-on-failure --no-tests=error` runs the declared gate.
- Exact-SHA CI evidence is required from both `editor-integration` and `editor-package-roundtrip` before the ledger status changes.

## Delivery order

1. Add asset-reference/payload validation tests, then implementation.
2. Add preview cache and real supported previews, then Asset Browser source publication.
3. Add compatible live-World targets and command/undo coverage.
4. Add recovery envelope/state machine with atomic-store and restore tests.
5. Integrate the fixture into the canonical cook/package/install path and exact-SHA CI.
6. Update `wiki/gameplay-tools/SparkEditor.md`, `Asset-Pipeline.md`, `Game-Packaging.md`, and readiness data only when each stated behavior has direct evidence.

## Rollback

Each delivery slice is independently revertible.  The recovery store accepts only its declared schema version; an unknown/corrupt new record is ignored without changing existing projects.  No scene, asset, or package format is rewritten in place by browsing or preview generation.
