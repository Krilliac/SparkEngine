# EDT-210 Crash Recovery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (- [ ]) syntax for tracking.

**Goal:** Persist a bounded, atomic, user-approved snapshot of an unsaved editor World on the UI thread, then restore or discard it safely after reopening the matching project.

**Architecture:** A new editor-owned recovery store serializes a versioned JSON envelope only after EditorUI has serialized the live World on its owning thread. EditorCrashHandler stops owning recovery JSON and never invokes a World-reading callback from a crash or background context. A small controller holds the persistent Restore, Discard, and retry state that EditorUI renders as a modal after the matching project opens.

**Tech Stack:** C++23, std::filesystem, Spark::Json ParseBounded/StringifyPretty, Spark::SerializeWorld/DeserializeInto, Windows MoveFileExW atomic replacement, Dear ImGui modal UI, Spark CommandHistory.

**Spec:** docs/superpowers/specs/2026-09-08-edt210-asset-recovery-roundtrip-design.md

## Global Constraints

- Only the EditorUI thread reads or serializes the live World; a crash callback and a worker thread never traverse ECS state, allocate a recovery snapshot, or call a UI callback.
- Recovery schema version is exactly 1. The envelope is bounded to 16 MiB, 64 nested JSON levels, 250000 JSON nodes, 50 recent operation descriptions, and a 4096-byte maximum per operation.
- The envelope identifies the canonical active project root and a project-relative scene path. A record for any other root is not offered for restore.
- Writes validate the complete JSON before atomic replacement, preserve a last-known-good .bak record, and never delete a good record after a failed temp write or malformed candidate.
- Restore first deserializes into a fresh World. EditorUI calls SwapWorld only after that succeeds; a failed restore leaves the current World and recovery record untouched.
- Restore is never automatic. The modal provides Restore, Discard, and retry/error feedback. Discard is the only user action that clears an otherwise valid record.
- A successful ordinary SaveCurrentScene clears the matching recovery record after the scene save succeeds. Save failure, project-close failure, and abnormal process termination keep it.
- No legacy handwritten JSON parser remains in EditorCrashHandler. Existing crash dumps and logs remain active.
- Recovery tests use unique temporary directories and never read or clear the user’s editor data directory.

---

## File Structure

- Create: SparkEditor/Source/Core/EditorRecovery.h — schema, bounded store, load result, and modal controller declarations.
- Create: SparkEditor/Source/Core/EditorRecovery.cpp — JSON conversion, strict/bounded validation, backup rotation, atomic replacement, controller state transitions.
- Modify: SparkEditor/Source/Core/EditorCrashHandler.h — remove RecoveryData and the recovery callback/write/read API from crash-handler ownership.
- Modify: SparkEditor/Source/Core/EditorCrashHandler.cpp — remove handwritten recovery persistence, autosave thread, and crash-path SaveRecoveryData call.
- Modify: SparkEditor/Source/Core/EditorUI.h — recovery store/controller members and UI-thread capture/restore methods.
- Modify: SparkEditor/Source/Core/EditorUI.cpp — project-open detection, dirty-sequence capture, modal rendering, explicit restore/discard, normal-save clearing.
- Modify: SparkEditor/Source/UndoRedo/UndoRedoManager.h — expose a read-only monotonic GetEditSequence accessor for recovery scheduling.
- Modify: Tests/CMakeLists.txt — add EditorRecovery.cpp and the recovery tests to SparkTests.
- Create: Tests/TestEditorRecovery.cpp — store, backup, bounded parser, controller, fresh-World restore, and thread-ownership tests.
- Modify: Tests/TestEditorCrashHandlerFilterReal.cpp — keep crash-filter coverage and remove obsolete recovery-json expectations.
- Modify: Tests/TestEditorSubsystems.cpp — remove tests that assert handwritten EditorCrashHandler recovery output and replace them with real store coverage.
- Modify: wiki/gameplay-tools/SparkEditor.md — document explicit recovery behavior after direct evidence exists.

## Interfaces Established by Task 1

~~~cpp
namespace SparkEditor {
inline constexpr uint32_t kEditorRecoverySchemaVersion = 1;
inline constexpr size_t kEditorRecoveryMaxBytes = 16u * 1024u * 1024u;

struct EditorRecoverySnapshot {
    uint32_t schemaVersion = kEditorRecoverySchemaVersion;
    std::string projectIdentity;
    std::string projectRelativeScene;
    std::string sceneDisplayName;
    std::string serializedWorld;
    std::string layoutIniPath;
    std::vector<std::string> recentOperations;
    uint64_t dirtySequence = 0;
    int64_t capturedUnixMilliseconds = 0;
};

enum class EditorRecoveryLoadState : uint8_t {
    None, Primary, Backup, Invalid
};

struct EditorRecoveryLoadResult {
    EditorRecoveryLoadState state = EditorRecoveryLoadState::None;
    std::optional<EditorRecoverySnapshot> snapshot;
    std::string error;
};

class EditorRecoveryStore {
  public:
    explicit EditorRecoveryStore(std::filesystem::path directory);
    bool Save(const EditorRecoverySnapshot& snapshot, std::string& error);
    EditorRecoveryLoadResult LoadForProject(std::string_view projectIdentity) const;
    bool Clear(std::string& error);
    std::filesystem::path PrimaryPath() const;
    std::filesystem::path BackupPath() const;
};
}
~~~

## Interfaces Established by Task 3

~~~cpp
namespace SparkEditor {
enum class EditorRecoveryDialogState : uint8_t {
    Hidden, Available, RestoreFailed
};

class EditorRecoveryController {
  public:
    void Offer(EditorRecoverySnapshot snapshot);
    void SetRestoreFailure(std::string error);
    void DismissAfterRestore();
    void DismissAfterDiscard();
    [[nodiscard]] EditorRecoveryDialogState State() const;
    [[nodiscard]] const EditorRecoverySnapshot* Snapshot() const;
    [[nodiscard]] std::string_view Error() const;
};
}

// EditorUI owns these calls and invokes them only on its normal UI thread.
void EditorUI::CaptureRecoveryIfDue(bool force);
bool EditorUI::RestorePendingRecovery();
void EditorUI::DiscardPendingRecovery();
void EditorUI::RenderRecoveryModal();
~~~

### Task 1: Versioned Atomic Recovery Store

**Files:**
- Create: SparkEditor/Source/Core/EditorRecovery.h
- Create: SparkEditor/Source/Core/EditorRecovery.cpp
- Create: Tests/TestEditorRecovery.cpp
- Modify: Tests/CMakeLists.txt

**Consumes:** Spark::Json, std::filesystem, and a caller-provided immutable EditorRecoverySnapshot.

**Produces:** Fully validated read/write/backup semantics with no EditorUI, ImGui, World, or crash-handler dependency.

- [ ] **Step 1: Write failing store tests**

~~~cpp
TEST(EditorRecovery_QuotedUnicodeSnapshotRoundTrips)
{
    ScopedRecoveryDirectory scratch;
    SparkEditor::EditorRecoveryStore store(scratch.Path());
    auto snapshot = MakeSnapshot("C:/Games/雪/SparkProject", R"(Scenes/"quoted".scene)",
                                 R"({"name":"雪 \"saved\""})");
    std::string error;
    ASSERT_TRUE(store.Save(snapshot, error));

    const auto loaded = store.LoadForProject(snapshot.projectIdentity);
    ASSERT_EQ(loaded.state, SparkEditor::EditorRecoveryLoadState::Primary);
    ASSERT_TRUE(loaded.snapshot.has_value());
    EXPECT_EQ(loaded.snapshot->serializedWorld, snapshot.serializedWorld);
}

TEST(EditorRecovery_FailedWritePreservesPriorPrimaryAndBackup)
{
    ScopedRecoveryDirectory scratch;
    SparkEditor::EditorRecoveryStore store(scratch.Path());
    std::string error;
    ASSERT_TRUE(store.Save(MakeSnapshot("project-a", "Scenes/a.scene", R"({"a":1})"), error));
    const std::string primaryBefore = ReadAll(store.PrimaryPath());

    auto invalid = MakeSnapshot("project-a", "Scenes/a.scene", "not-json");
    EXPECT_FALSE(store.Save(invalid, error));
    EXPECT_EQ(ReadAll(store.PrimaryPath()), primaryBefore);
    EXPECT_FALSE(ReadAll(store.BackupPath()).empty());
}
~~~

- [ ] **Step 2: Run the store filter and confirm it fails**

Run:

~~~powershell
$env:SPARK_TEST_NAME='EditorRecovery_'
$env:SPARK_TEST_EXPECT_COUNT='2'
& .\build\windows-release\bin\Release\SparkTests.exe --quiet --empty-is-error
~~~

Expected: compile failure naming EditorRecoveryStore.

- [ ] **Step 3: Implement schema conversion and bounds**

Serialize this exact object shape through Spark::Json::StringifyPretty:

~~~json
{
  "schemaVersion": 1,
  "projectIdentity": "canonical-project-root",
  "projectRelativeScene": "Scenes/Main.scene",
  "sceneDisplayName": "Main",
  "serializedWorld": "{...}",
  "layoutIniPath": "Layouts/current.ini",
  "recentOperations": ["Assign Models/crate.obj"],
  "dirtySequence": 42,
  "capturedUnixMilliseconds": 1760000000000
}
~~~

Require a nonempty projectIdentity, serializedWorld, and sceneDisplayName. Require schemaVersion 1; require projectRelativeScene to be empty or relative with no .. segment; cap every string and collection before constructing the snapshot. Parse both the envelope and serializedWorld with Spark::Json::ParseBounded using maxBytes 16 MiB, maxDepth 64, and maxNodes 250000. A foreign project identity returns None rather than Invalid, so a previous project’s valid data is retained for that project but not shown here.

- [ ] **Step 4: Implement atomic primary and backup writes**

Use a unique sibling temporary filename. Write full bytes, close the stream, reread and byte-compare, ParseBounded validate, then atomically replace. On Windows use MoveFileExW with MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH. Before replacing an existing primary, copy its validated bytes to a temporary backup and atomically replace recovery-v1.json.bak. If backup preparation or primary replacement fails, clean only the temporary file and leave the primary unchanged.

~~~cpp
const auto temp = MakeTemporarySibling(primary);
if (!WriteAndReadBack(temp, document, error) || !ValidateEnvelope(document, error))
    return false;
if (std::filesystem::exists(primary) && !ReplaceBackupFromPrimary(primary, backup, error))
    return false;
return ReplaceFileAtomically(temp, primary, error);
~~~

- [ ] **Step 5: Add corruption and backup fallback tests**

Add tests for truncated primary with valid backup, invalid JSON in both files, oversize primary, deep nesting beyond 64, foreign identity, and Clear removing only the two recovery files. Assert a corrupted primary returns Backup when the backup is valid and never overwrites either file during LoadForProject.

- [ ] **Step 6: Run store verification**

Run:

~~~powershell
$env:SPARK_TEST_NAME='EditorRecovery_'
& .\build\windows-release\bin\Release\SparkTests.exe --quiet --empty-is-error
~~~

Expected: all EditorRecovery_ tests pass with at least one assertion per test.

- [ ] **Step 7: Commit the storage slice**

~~~powershell
git add -- SparkEditor/Source/Core/EditorRecovery.h SparkEditor/Source/Core/EditorRecovery.cpp Tests/TestEditorRecovery.cpp Tests/CMakeLists.txt
git commit -m "feat(editor): persist atomic recovery snapshots"
~~~

### Task 2: Remove Recovery Work from Crash Contexts

**Files:**
- Modify: SparkEditor/Source/Core/EditorCrashHandler.h
- Modify: SparkEditor/Source/Core/EditorCrashHandler.cpp
- Modify: Tests/TestEditorCrashHandlerFilterReal.cpp
- Modify: Tests/TestEditorSubsystems.cpp

**Consumes:** Task 1 persists recovery while the editor is healthy.

**Produces:** A crash handler that still installs/restores filters, records operations, writes crash diagnostics, and no longer owns unsafe recovery JSON.

- [ ] **Step 1: Write regression tests for absence of unsafe recovery dispatch**

~~~cpp
TEST(EditorCrashHandler_RecoveryPersistenceIsNotOwnedByCrashHandler)
{
    const std::string source = ReadSourceFile("SparkEditor/Source/Core/EditorCrashHandler.cpp");
    EXPECT_FALSE(source.contains("SaveRecoveryData("));
    EXPECT_FALSE(source.contains("m_recoveryCallback"));
    EXPECT_FALSE(source.contains("AutoSaveRecoveryThread"));
}

TEST(EditorCrashHandler_RecordOperationStillAcceptsBoundedHistory)
{
    auto& handler = SparkEditor::EditorCrashHandler::GetInstance();
    ScopedCrashDirectory scratch;
    ASSERT_TRUE(handler.Initialize(scratch.Path().string()));
    for (int index = 0; index != 75; ++index)
        handler.RecordOperation("op-" + std::to_string(index));
    EXPECT_TRUE(handler.GenerateCrashReport({}).contains("op-74"));
    handler.Shutdown();
}
~~~

- [ ] **Step 2: Run the focused crash-handler tests and confirm the first one fails**

Run:

~~~powershell
$env:SPARK_TEST_NAME='EditorCrashHandler_'
& .\build\windows-release\bin\Release\SparkTests.exe --quiet --empty-is-error
~~~

Expected: the source-contract test fails while legacy recovery methods exist.

- [ ] **Step 3: Remove the handwritten recovery API**

Delete RecoveryData, RecoveryCallback, SetRecoveryCallback, SaveRecoveryData, LoadRecoveryData, HasRecoveryData, ClearRecoveryData, SetAutoSaveRecovery, AutoSaveRecoveryThread, autosave fields, and every related include from EditorCrashHandler. Remove the SaveRecoveryData call from HandleCrashInternal. Keep CrashInfo recent operation reporting and the filter/dump/log code untouched.

The deletion is intentional: the crash handler no longer needs a recovery callback because EditorRecoveryStore has already persisted the last UI-thread snapshot. Do not replace it with a lambda, mutex, background worker, or a serialized World pointer.

- [ ] **Step 4: Rewrite obsolete unit coverage**

Replace CrashHandler_SaveAndHasRecoveryData and CrashHandler_ClearRecoveryData with direct EditorRecoveryStore tests from Task 1. Keep real filter installation, empty-directory rejection, directory creation, shutdown restoration, and RecordOperation tests.

- [ ] **Step 5: Run crash handler and recovery filters**

Run:

~~~powershell
$env:SPARK_TEST_NAME='EditorCrashHandler_'
$env:SPARK_TEST_EXPECT_COUNT='2'
& .\build\windows-release\bin\Release\SparkTests.exe --quiet --empty-is-error

$env:SPARK_TEST_NAME='EditorRecovery_'
$env:SPARK_TEST_EXPECT_COUNT='6'
& .\build\windows-release\bin\Release\SparkTests.exe --quiet --empty-is-error
~~~

Expected: 0 failed. Existing crash filter tests keep their prior platform guards.

- [ ] **Step 6: Commit the safety slice**

~~~powershell
git add -- SparkEditor/Source/Core/EditorCrashHandler.h SparkEditor/Source/Core/EditorCrashHandler.cpp Tests/TestEditorCrashHandlerFilterReal.cpp Tests/TestEditorSubsystems.cpp
git commit -m "fix(editor): keep recovery out of crash callbacks"
~~~

### Task 3: UI-Thread Capture Scheduling and Project Matching

**Files:**
- Modify: SparkEditor/Source/UndoRedo/UndoRedoManager.h
- Modify: SparkEditor/Source/Core/EditorUI.h
- Modify: SparkEditor/Source/Core/EditorUI.cpp
- Modify: Tests/TestEditorRecovery.cpp

**Consumes:** Task 1 store and Task 2 safe crash handler.

**Produces:** Fresh immutable recovery snapshots for all command-history mutations and matching-project startup detection.

- [ ] **Step 1: Write failing UI-thread capture tests**

~~~cpp
TEST(EditorRecovery_CaptureUsesWorldOwnedByCallingThread)
{
    World world;
    const EntityID entity = MakeNamedEntity(world, "Recovery Entity");
    const auto snapshot = CaptureRecoverySnapshotForTest(world, "project-a", "Scenes/Main.scene", 7);
    EXPECT_TRUE(snapshot.serializedWorld.contains("Recovery Entity"));
    EXPECT_EQ(snapshot.dirtySequence, uint64_t(7));
}

TEST(EditorRecovery_ProjectMismatchIsNotOffered)
{
    ScopedRecoveryDirectory scratch;
    SparkEditor::EditorRecoveryStore store(scratch.Path());
    std::string error;
    ASSERT_TRUE(store.Save(MakeSnapshot("project-a", "Scenes/Main.scene", R"({"a":1})"), error));
    EXPECT_EQ(store.LoadForProject("project-b").state, SparkEditor::EditorRecoveryLoadState::None);
}
~~~

- [ ] **Step 2: Add a read-only dirty sequence accessor**

Add this one-line API to UndoRedoManager:

~~~cpp
[[nodiscard]] uint64_t GetEditSequence() const { return m_editSequence; }
~~~

Do not reset the monotonic value in Clear. Recovery compares the value with EditorUI’s last successfully persisted sequence and uses HasUnsavedChanges plus m_sceneModified to decide whether capture is needed.

- [ ] **Step 3: Add EditorUI recovery state**

Add an EditorRecoveryStore rooted at ProjectManager::GetEditorDataDirectory()/Crashes, an EditorRecoveryController, a bounded deque of recent command descriptions, lastCapturedSequence, lastCaptureTime, and a recovery capture interval of 30 seconds. Add one private helper that canonicalizes ProjectManager::GetActiveProjectPath into the projectIdentity string. The identity must not fall back to the current working directory.

In Update, after shortcut and command processing:

~~~cpp
void EditorUI::CaptureRecoveryIfDue(bool force)
{
    if (!m_world || !m_projectManager || !m_projectManager->HasOpenProject())
        return;
    auto& history = Spark::Editor::CommandHistory::GetInstance();
    if (!IsSceneModified())
        return;
    if (!force && history.GetEditSequence() == m_lastCapturedSequence &&
        Clock::now() - m_lastRecoveryCapture < m_recoveryInterval)
        return;
    PersistRecoverySnapshotOnUiThread(history.GetEditSequence());
}
~~~

PersistRecoverySnapshotOnUiThread calls Spark::SerializeWorld while EditorUI owns m_world, constructs a value-only EditorRecoverySnapshot, calls store.Save, and advances m_lastCapturedSequence only on success. On failure it shows one error notification and leaves the prior record intact.

- [ ] **Step 4: Capture operation descriptions**

After every successful EditorUI::RecordAppliedDocumentMutation, CreateDocumentEntity, DeleteSelectedDocumentEntity, NewSceneNow mutation, and typed asset assignment, append the command description to the bounded deque and call EditorCrashHandler::RecordOperation with the same description. In Update, also detect CommandHistory::GetEditSequence changes made by SceneView, Hierarchy, or keyboard undo/redo; append GetUndoDescription or GetRedoDescription when nonempty and force the next UI-thread capture. This ensures editor paths that already use direct CommandHistory calls enter recovery without creating a second command stack.

- [ ] **Step 5: Offer recovery only after matching project opens**

At the end of the ProjectManager opened callback, after OpenScene or NewSceneNow established a safe base World, call store.LoadForProject(projectIdentity). For Primary or Backup, call m_recoveryController.Offer(snapshot) and never mutate m_world there. For Invalid, show a warning that contains the parse error and retain both recovery files. For project close, clear only in-memory controller state; do not delete the on-disk record.

- [ ] **Step 6: Run recovery capture verification**

Run:

~~~powershell
$env:SPARK_TEST_NAME='EditorRecovery_'
& .\build\windows-release\bin\Release\SparkTests.exe --quiet --empty-is-error
~~~

Expected: all storage and capture tests pass; no test requires a background thread or user editor-data path.

- [ ] **Step 7: Commit the capture slice**

~~~powershell
git add -- SparkEditor/Source/UndoRedo/UndoRedoManager.h SparkEditor/Source/Core/EditorUI.h SparkEditor/Source/Core/EditorUI.cpp Tests/TestEditorRecovery.cpp
git commit -m "feat(editor): capture recovery snapshots on UI thread"
~~~

### Task 4: Explicit Restore, Discard, and Clean-Save Semantics

**Files:**
- Modify: SparkEditor/Source/Core/EditorRecovery.h
- Modify: SparkEditor/Source/Core/EditorRecovery.cpp
- Modify: SparkEditor/Source/Core/EditorUI.h
- Modify: SparkEditor/Source/Core/EditorUI.cpp
- Modify: Tests/TestEditorRecovery.cpp

**Consumes:** A valid pending snapshot offered by Task 3.

**Produces:** A persistent modal state machine that cannot corrupt the current World or silently erase a recovery record.

- [ ] **Step 1: Write failing restore/controller tests**

~~~cpp
TEST(EditorRecovery_FailedRestoreLeavesCurrentWorldAndRecord)
{
    ScopedRecoveryDirectory scratch;
    SparkEditor::EditorRecoveryStore store(scratch.Path());
    std::string error;
    auto snapshot = MakeSnapshot("project-a", "Scenes/Main.scene", "malformed-world");
    ASSERT_FALSE(store.Save(snapshot, error));

    World current;
    const EntityID keep = MakeNamedEntity(current, "Keep Me");
    EXPECT_FALSE(RestoreWorldFromRecoveryForTest(current, snapshot, error));
    EXPECT_TRUE(current.GetRegistry().valid(keep));
}

TEST(EditorRecovery_ExplicitDiscardClearsOnlyAfterUserAction)
{
    SparkEditor::EditorRecoveryController controller;
    controller.Offer(MakeSnapshot("project-a", "Scenes/Main.scene", R"({"a":1})"));
    EXPECT_EQ(controller.State(), SparkEditor::EditorRecoveryDialogState::Available);
    controller.DismissAfterDiscard();
    EXPECT_EQ(controller.State(), SparkEditor::EditorRecoveryDialogState::Hidden);
}
~~~

- [ ] **Step 2: Implement controller transitions**

EditorRecoveryController owns the optional snapshot and an error string. Offer always enters Available. SetRestoreFailure enters RestoreFailed while retaining the same snapshot. DismissAfterRestore and DismissAfterDiscard clear the snapshot and enter Hidden. The controller has no filesystem or World pointer.

- [ ] **Step 3: Implement RestorePendingRecovery atomically**

~~~cpp
bool EditorUI::RestorePendingRecovery()
{
    const auto* snapshot = m_recoveryController.Snapshot();
    if (!snapshot || !m_world)
        return false;
    auto restored = std::make_unique<::World>();
    if (!Spark::DeserializeInto(*restored, snapshot->serializedWorld)) {
        m_recoveryController.SetRestoreFailure("Recovery document could not be deserialized");
        return false;
    }
    SwapWorld(std::move(restored));
    m_currentScenePath = snapshot->projectRelativeScene;
    m_currentSceneName = snapshot->sceneDisplayName;
    m_sceneModified = true;
    m_recoveryController.DismissAfterRestore();
    std::string error;
    if (!m_recoveryStore->Clear(error))
        ShowNotification("Recovered document restored; recovery cleanup failed: " + error, "warning");
    return true;
}
~~~

Before SwapWorld, verify the controller snapshot identity matches the active canonical project identity. Clear only after successful SwapWorld; a clear failure is nonfatal and leaves an already-restored document dirty.

- [ ] **Step 4: Render a persistent modal**

Call RenderRecoveryModal from EditorUI::Render after normal panels and before the project browser overlay can take focus. Open the ImGui popup once per controller state transition, not every frame. The modal displays the scene display name, capture timestamp, whether the backup was used, and the last operation description. Restore calls RestorePendingRecovery. Discard calls store.Clear then DismissAfterDiscard only if Clear succeeds. Restore failure displays its error and retains Restore and Discard buttons.

- [ ] **Step 5: Clear after successful ordinary save only**

At the end of SaveCurrentScene, after Spark::SaveWorld succeeds and MarkSaved completes, clear a pending/matching record. Do not clear during OpenScene, NewScene, ResetWorldAfterProjectClose, a failed save, or EditorUI::Shutdown. On shutdown, force one final UI-thread CaptureRecoveryIfDue(true) only when IsSceneModified and a project is open; then release the World normally.

- [ ] **Step 6: Run recovery tests and a bounded editor smoke**

Run:

~~~powershell
$env:SPARK_TEST_NAME='EditorRecovery_'
& .\build\windows-release\bin\Release\SparkTests.exe --quiet --empty-is-error
& .\build\windows-release\bin\Release\SparkEditor.exe --test-mode --test-frames 3
~~~

Expected: recovery filter passes; the editor exits after its frame limit without an unhandled recovery exception.

- [ ] **Step 7: Commit the restore UX slice**

~~~powershell
git add -- SparkEditor/Source/Core/EditorRecovery.h SparkEditor/Source/Core/EditorRecovery.cpp SparkEditor/Source/Core/EditorUI.h SparkEditor/Source/Core/EditorUI.cpp Tests/TestEditorRecovery.cpp
git commit -m "feat(editor): restore unsaved worlds explicitly"
~~~

### Task 5: Documentation and Release Gate Evidence

**Files:**
- Modify: wiki/gameplay-tools/SparkEditor.md
- Modify: docs/readiness/work-items/20-platform-runtime-editor.json
- Modify: .github/workflows/build.yml

**Consumes:** Passing Task 1 through Task 4 tests and a clean editor startup smoke.

**Produces:** Accurate recovery documentation and exact-SHA CI evidence without prematurely closing EDT-210.

- [ ] **Step 1: Write the docs from confirmed behavior**

Document recovery location class, matching-project rule, UI-thread serialization, Restore/Discard behavior, backup fallback, and the fact that normal save clears only after persistence succeeds. Do not state that a crash handler serializes a World or that recovery happens automatically.

- [ ] **Step 2: Add labelled integration coverage**

Register the recovery integration test with the editor-integration CTest label and update the matching workflow command:

~~~bash
ctest --test-dir build/windows-shipping -C MinSizeRel -L editor-integration --output-on-failure --no-tests=error
~~~

Archive the exact test output with github.sha in the artifact name. A missing label fails the job through --no-tests=error.

- [ ] **Step 3: Reconcile readiness data**

Keep EDT-210 open until the separate asset/package plan has exact-SHA package proof. Add direct recovery evidence references to the work item only after local tests and CI complete. Do not change any P1/P0 status based on planned behavior.

- [ ] **Step 4: Run release-facing verification**

Run:

~~~powershell
$env:SPARK_TEST_NAME='EditorRecovery_'
& .\build\windows-shipping\bin\MinSizeRel\SparkTests.exe --quiet --empty-is-error

$env:SPARK_TEST_NAME='EditorCrashHandler_'
& .\build\windows-shipping\bin\MinSizeRel\SparkTests.exe --quiet --empty-is-error
ctest --test-dir build/windows-shipping -C MinSizeRel -L editor-integration --output-on-failure --no-tests=error
git diff --check
~~~

Expected: zero test failures, a nonempty labelled CTest run, and no whitespace errors.

- [ ] **Step 5: Commit the evidence slice**

~~~powershell
git add -- wiki/gameplay-tools/SparkEditor.md docs/readiness/work-items/20-platform-runtime-editor.json .github/workflows/build.yml
git commit -m "docs(editor): document recovery evidence"
~~~

## Final Verification

- [ ] Run the EditorRecovery_ and EditorCrashHandler_ selectors separately with SPARK_TEST_NAME and --empty-is-error (the runner accepts one name filter at a time).
- [ ] Run the full SparkTests suite in the affected Windows Release configuration.
- [ ] Run the shipping editor-integration CTest label with --no-tests=error.
- [ ] Inspect git diff --check and a targeted diff before each commit.
- [ ] Require exact-SHA successful CI evidence before recording recovery as complete in the readiness ledger.
