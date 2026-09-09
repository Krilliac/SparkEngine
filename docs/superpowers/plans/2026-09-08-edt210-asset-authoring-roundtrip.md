# EDT-210 Asset Authoring and Package Round-Trip Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (- [ ]) syntax for tracking.

**Goal:** Let authors browse supported project assets, preview them honestly, assign compatible references through undo-safe editor actions, and prove the saved references survive the installed package path.

**Architecture:** The editor owns a compact typed project-relative asset-reference boundary. Asset Browser publishes only a validated binary payload; Inspector and Scene View decode it and delegate every mutation to EditorUI, which captures a document snapshot and records one CommandHistory entry. Preview rendering reuses GraphicsEngine texture/material loading and the existing WorldBasic renderer, never a Blender-generated or disk-written invented thumbnail.

**Tech Stack:** C++23, std::filesystem, Dear ImGui drag/drop, Direct3D 11 on Windows, Spark World/ComponentReflection/CommandHistory, SparkCooker and the installed-package CTest path.

**Spec:** docs/superpowers/specs/2026-09-08-edt210-asset-recovery-roundtrip-design.md

## Global Constraints

- Canonical references use UTF-8 forward-slash paths relative to the active project Assets directory; absolute paths, embedded NULs, traversal, and a path outside Assets fail before preview or mutation.
- Payload type is SPARK_EDITOR_ASSET_REF with a versioned fixed header and copied byte buffer; no host path pointer or raw std::string object crosses ImGui drag/drop.
- Mesh, material, and texture assignments mutate only through EditorUI::RecordAppliedDocumentMutation and create exactly one CommandHistory entry.
- Audio remains visibly unavailable for assignment until ASSET-220 supplies a packaged sound-name registry; no file path is written to AudioSourceComponent::soundName.
- A missing file, type mismatch, missing selected entity, device loss, or malformed payload leaves the World, undo stack, and project files unchanged.
- Browser previews are read-only and use no generated image file, Blender process, or authoring-tree write.
- All new tests run from a temporary project root and leave no user project, editor data, or installed package modified.
- The final gate is evidence from exact-SHA editor-integration and editor-package-roundtrip CI jobs; a host-only SparkTests pass is supporting evidence only.

---

## File Structure

- Create: SparkEditor/Source/AssetPipeline/EditorAssetReference.h — canonical asset type, project-root validation, payload encoder/decoder contract.
- Create: SparkEditor/Source/AssetPipeline/EditorAssetReference.cpp — filesystem containment, UTF-8 validation, byte-level payload implementation.
- Create: SparkEditor/Source/AssetPipeline/EditorAssetAssignment.h — pure World mutation contract for one compatible asset target.
- Create: SparkEditor/Source/AssetPipeline/EditorAssetAssignment.cpp — component-specific assignment with no command-history ownership.
- Create: SparkEditor/Source/AssetPipeline/EditorAssetPreviewCache.h — cache state and Windows preview-resource ownership.
- Create: SparkEditor/Source/AssetPipeline/EditorAssetPreviewCache.cpp — texture/material SRV lookup, mesh-card render target, WAV waveform data, invalidation.
- Modify: SparkEditor/Source/Panels/AssetBrowserPanel.h — active asset root, preview cache, and drag source helpers.
- Modify: SparkEditor/Source/Panels/AssetBrowserPanel.cpp — real preview rendering and typed source payload publication.
- Modify: SparkEditor/Source/Panels/InspectorPanel.cpp — explicit compatible drop targets beside World-backed component fields.
- Modify: SparkEditor/Source/Panels/SceneViewPanel.h — EditorUI asset-drop sink.
- Modify: SparkEditor/Source/Panels/SceneViewPanel.cpp — viewport mesh-drop target.
- Modify: SparkEditor/Source/Core/EditorUI.h — public typed assignment/create entrypoints.
- Modify: SparkEditor/Source/Core/EditorUI.cpp — snapshot, assignment, notification, dirty-state, and project/device invalidation wiring.
- Modify: SparkEditor/CMakeLists.txt — compile the new editor source files when source enumeration is explicit.
- Modify: Tests/CMakeLists.txt — link pure asset modules in SparkTests and enable ImGui/D3D preview tests only under their existing feature guards.
- Create: Tests/TestEditorAssetReference.cpp — path and payload contract tests.
- Create: Tests/TestEditorAssetAssignment.cpp — component compatibility and undo-boundary-facing mutation tests.
- Create: Tests/TestEditorAssetPreview.cpp — cache invalidation, honest unavailable state, and Windows WARP rendering tests.
- Create: Tests/TestEditorCookPackageRoundTrip.cpp — author/save/reopen/cook/package/installed-runtime fixture.
- Modify: .github/workflows/build.yml — named editor-integration and editor-package-roundtrip exact-SHA checks with retained reports.
- Modify: wiki/gameplay-tools/SparkEditor.md, wiki/gameplay-tools/Asset-Pipeline.md, wiki/gameplay-tools/Game-Packaging.md — exact supported-format, recovery, and package behavior only after tests pass.

## Interfaces Established by Task 1

~~~cpp
namespace SparkEditor {
inline constexpr std::string_view kEditorAssetDragPayload = "SPARK_EDITOR_ASSET_REF";
inline constexpr uint16_t kEditorAssetPayloadVersion = 1;
inline constexpr size_t kEditorAssetReferenceMaxBytes = 4096;

enum class EditorAssetKind : uint8_t { Texture, Mesh, Material, Audio, Unknown };

struct EditorAssetReference {
    EditorAssetKind kind = EditorAssetKind::Unknown;
    std::string projectRelativePath;
};

[[nodiscard]] std::optional<EditorAssetReference>
CreateEditorAssetReference(const std::filesystem::path& assetRoot,
                           const std::filesystem::path& candidate,
                           std::string& error);
[[nodiscard]] bool EncodeEditorAssetReference(const EditorAssetReference& reference,
                                              std::vector<std::byte>& payload,
                                              std::string& error);
[[nodiscard]] bool DecodeEditorAssetReference(std::span<const std::byte> payload,
                                              const std::filesystem::path& assetRoot,
                                              EditorAssetReference& reference,
                                              std::string& error);
}
~~~

## Interfaces Established by Task 3

~~~cpp
namespace SparkEditor {
enum class EditorAssetAssignmentTarget : uint8_t {
    MeshRendererMesh,
    MeshRendererMaterial,
    SpriteTexture,
    DecalTexture,
    NineSliceTexture,
    BillboardTexture,
    TrailMaterial
};

[[nodiscard]] bool ApplyEditorAssetAssignment(
    ::World& world, ::EntityID entity, EditorAssetAssignmentTarget target,
    const EditorAssetReference& reference, std::string& error);
}

// EditorUI owns the snapshot and CommandHistory boundary.
bool EditorUI::ApplyAssetReferenceToSelection(
    EditorAssetAssignmentTarget target, const EditorAssetReference& reference);
bool EditorUI::CreateDocumentMeshFromAsset(const EditorAssetReference& reference);
~~~

### Task 1: Canonical Asset Reference and Payload

**Files:**
- Create: SparkEditor/Source/AssetPipeline/EditorAssetReference.h
- Create: SparkEditor/Source/AssetPipeline/EditorAssetReference.cpp
- Create: Tests/TestEditorAssetReference.cpp
- Modify: Tests/CMakeLists.txt
- Modify: SparkEditor/CMakeLists.txt

**Consumes:** Active Assets root from AssetBrowserPanel::SetProjectPath.

**Produces:** CreateEditorAssetReference, EncodeEditorAssetReference, DecodeEditorAssetReference, EditorAssetKind, and kEditorAssetDragPayload for every later task.

- [ ] **Step 1: Write the failing reference tests**

~~~cpp
TEST(EditorAssetReference_CanonicalizesAFileBelowAssets)
{
    ScopedAssetProject project;
    const auto file = project.Write("Assets/Textures/UI/checker.png", "png");
    std::string error;
    const auto ref = SparkEditor::CreateEditorAssetReference(project.Assets(), file, error);
    ASSERT_TRUE(ref.has_value());
    EXPECT_EQ(ref->kind, SparkEditor::EditorAssetKind::Texture);
    EXPECT_EQ(ref->projectRelativePath, std::string("Textures/UI/checker.png"));
}

TEST(EditorAssetReference_RejectsTraversalAndMalformedPayload)
{
    ScopedAssetProject project;
    std::string error;
    EXPECT_FALSE(SparkEditor::CreateEditorAssetReference(
        project.Assets(), project.Root() / "outside.obj", error).has_value());

    std::vector<std::byte> bad = {std::byte{1}, std::byte{0}};
    SparkEditor::EditorAssetReference decoded;
    EXPECT_FALSE(SparkEditor::DecodeEditorAssetReference(bad, project.Assets(), decoded, error));
}
~~~

- [ ] **Step 2: Run the filtered tests and confirm they fail because the module is absent**

Run:

~~~powershell
$env:SPARK_TEST_NAME='EditorAssetReference_'
$env:SPARK_TEST_EXPECT_COUNT='2'
& .\build\windows-release\bin\Release\SparkTests.exe --quiet --empty-is-error
~~~

Expected: compile or link failure naming EditorAssetReference, then zero green EditorAssetReference tests.

- [ ] **Step 3: Implement the validation and binary codec**

Implement the functions declared in the interface block with these exact checks:

~~~cpp
const auto root = std::filesystem::weakly_canonical(assetRoot, ec);
const auto file = std::filesystem::weakly_canonical(candidate, ec);
const auto relative = std::filesystem::relative(file, root, ec).lexically_normal();
if (ec || relative.empty() || relative.is_absolute() || *relative.begin() == "..")
    return std::nullopt;
if (!std::filesystem::is_regular_file(file, ec) || ec)
    return std::nullopt;
~~~

Classify only .png, .jpg, .jpeg, .tga, .bmp, .dds as Texture; .obj, .fbx, .gltf, .glb as Mesh; .mat, .material, .json as Material; .wav, .ogg, .mp3 as Audio. The payload header is little-endian version:uint16_t, kind:uint8_t, reserved:uint8_t, pathByteCount:uint32_t. Reject a byte count of zero, a count above kEditorAssetReferenceMaxBytes, trailing bytes, an unsupported kind/version, invalid UTF-8, a NUL byte, or a decoded path that no longer resolves below assetRoot.

- [ ] **Step 4: Add the new sources to editor and test targets**

Add EditorAssetReference.cpp to the explicit SparkEditor target source list when present. Add EditorAssetReference.cpp and TestEditorAssetReference.cpp to SparkTests outside the ImGui-only target_sources block so validation runs on Linux and Windows.

- [ ] **Step 5: Run focused reference verification**

Run:

~~~powershell
$env:SPARK_TEST_NAME='EditorAssetReference_'
$env:SPARK_TEST_EXPECT_COUNT='2'
& .\build\windows-release\bin\Release\SparkTests.exe --quiet --empty-is-error
~~~

Expected: 2 passed, 0 failed; the test output contains no host-absolute path in encoded payload assertions.

- [ ] **Step 6: Commit the self-contained slice**

~~~powershell
git add -- SparkEditor/Source/AssetPipeline/EditorAssetReference.h SparkEditor/Source/AssetPipeline/EditorAssetReference.cpp Tests/TestEditorAssetReference.cpp Tests/CMakeLists.txt SparkEditor/CMakeLists.txt
git commit -m "feat(editor): validate typed asset references"
~~~

### Task 2: Honest Preview Cache and Asset-Browser Source

**Files:**
- Create: SparkEditor/Source/AssetPipeline/EditorAssetPreviewCache.h
- Create: SparkEditor/Source/AssetPipeline/EditorAssetPreviewCache.cpp
- Modify: SparkEditor/Source/Panels/AssetBrowserPanel.h
- Modify: SparkEditor/Source/Panels/AssetBrowserPanel.cpp
- Create: Tests/TestEditorAssetPreview.cpp
- Modify: Tests/CMakeLists.txt

**Consumes:** EditorAssetReference and the non-owning GraphicsEngine already set by EditorUI::SetGraphicsDevice.

**Produces:** Cache-backed preview state, native texture preview resource on Windows, explicit unavailable/error presentation, and a typed drag source.

- [ ] **Step 1: Write preview-state and invalidation tests**

~~~cpp
TEST(EditorPreview_UnsupportedAudioIsLabelledUnavailable)
{
    SparkEditor::EditorAssetPreviewCache cache;
    const auto preview = cache.QueryCpuPreview(
        {SparkEditor::EditorAssetKind::Audio, "Audio/ambience.mp3"});
    EXPECT_EQ(preview.state, SparkEditor::EditorAssetPreviewState::Unavailable);
    EXPECT_STR_CONTAINS(preview.message, "sound-name registry");
}

TEST(EditorPreview_ProjectResetDropsCachedEntries)
{
    SparkEditor::EditorAssetPreviewCache cache;
    cache.NotePreviewForTest({SparkEditor::EditorAssetKind::Texture, "Textures/a.png"});
    cache.ClearProject();
    EXPECT_EQ(cache.EntryCountForTest(), size_t(0));
}
~~~

- [ ] **Step 2: Run the focused preview tests and confirm they fail**

Run:

~~~powershell
$env:SPARK_TEST_NAME='EditorPreview_'
$env:SPARK_TEST_EXPECT_COUNT='2'
& .\build\windows-release\bin\Release\SparkTests.exe --quiet --empty-is-error
~~~

Expected: compile failure until EditorAssetPreviewCache is linked.

- [ ] **Step 3: Implement preview cache ownership and real supported previews**

Expose the following data boundary:

~~~cpp
enum class EditorAssetPreviewState : uint8_t { Ready, Unavailable, Failed };
struct EditorAssetPreview {
    EditorAssetPreviewState state = EditorAssetPreviewState::Unavailable;
    std::string message;
#ifdef _WIN32
    ID3D11ShaderResourceView* srv = nullptr; // borrowed from cache while entry is alive
#endif
    std::vector<float> waveform;
};
~~~

For texture references, resolve the confined native path and call GraphicsEngine::GetOrLoadTextureSRV. A null return becomes Failed with the failed asset name, not Ready with a file icon. For material references, call GraphicsEngine::GetOrLoadBasicMaterial(projectRelativePath, projectRootUtf8) and expose its albedo srv only when it exists. For WAV, parse RIFF/WAVE headers and bounded PCM samples into a normalized waveform; for MP3 and OGG return Unavailable with the registry message. For mesh references, create a 128x128 D3D11 render target, a depth target, and a tiny local World containing one MeshRenderer whose meshPath is the canonical reference; render it with Spark::RenderWorldBasic and retain the target SRV only after stats.drawn is nonzero. Restore the previous D3D output-merger targets, viewport, shader-resource bindings, and raster/depth/blend state before returning to ImGui.

Key entries include reference kind, path, file size, and last-write time. Invalidate an entry after AssetBrowserPanel::ImportAsset succeeds; ClearProject, SetGraphics(nullptr), Shutdown, and device recreation release every COM resource before its owner goes away.

- [ ] **Step 4: Render previews and publish drag bytes in AssetBrowserPanel**

At each asset grid cell:

~~~cpp
std::string error;
const auto reference = CreateEditorAssetReference(assetsRoot, assetPath, error);
const auto preview = reference ? m_previewCache.Query(*reference) : EditorAssetPreview{};
RenderAssetPreviewOrStatus(preview, fileIcon, iconColor, size);
if (reference && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
    std::vector<std::byte> payload;
    if (EncodeEditorAssetReference(*reference, payload, error))
        ImGui::SetDragDropPayload(kEditorAssetDragPayload, payload.data(), payload.size());
    ImGui::TextUnformatted(filename.c_str());
    ImGui::EndDragDropSource();
}
~~~

Render generic file glyphs only for unknown non-assignable files. Render a labelled unavailable or failed surface for a known supported asset whose preview cannot be produced. The source path remains private to AssetBrowserPanel.

- [ ] **Step 5: Add a Windows WARP preview test**

Extend the repository’s existing WARP D3D11 fixture to create a 2x2 PNG and a simple OBJ in ScopedAssetProject. Assert texture preview returns Ready with a non-null srv, mesh preview returns Ready with a non-null srv and one drawn render candidate, and a corrupted image returns Failed. Guard only the D3D device-creation assertion with the existing Windows feature macro; never pass it by skipping after an attempted preview.

- [ ] **Step 6: Run preview verification**

Run:

~~~powershell
$env:SPARK_TEST_NAME='EditorPreview_'
& .\build\windows-release\bin\Release\SparkTests.exe --quiet --empty-is-error
~~~

Expected: every EditorPreview test passes; missing graphics support reports an explicit skipped build feature rather than a false ready preview.

- [ ] **Step 7: Commit the preview/source slice**

~~~powershell
git add -- SparkEditor/Source/AssetPipeline/EditorAssetPreviewCache.h SparkEditor/Source/AssetPipeline/EditorAssetPreviewCache.cpp SparkEditor/Source/Panels/AssetBrowserPanel.h SparkEditor/Source/Panels/AssetBrowserPanel.cpp Tests/TestEditorAssetPreview.cpp Tests/CMakeLists.txt SparkEditor/CMakeLists.txt
git commit -m "feat(editor): add real asset browser previews"
~~~

### Task 3: Compatible World Drop Targets and Undo Boundaries

**Files:**
- Create: SparkEditor/Source/AssetPipeline/EditorAssetAssignment.h
- Create: SparkEditor/Source/AssetPipeline/EditorAssetAssignment.cpp
- Modify: SparkEditor/Source/Core/EditorUI.h
- Modify: SparkEditor/Source/Core/EditorUI.cpp
- Modify: SparkEditor/Source/Panels/InspectorPanel.cpp
- Modify: SparkEditor/Source/Panels/SceneViewPanel.h
- Modify: SparkEditor/Source/Panels/SceneViewPanel.cpp
- Create: Tests/TestEditorAssetAssignment.cpp
- Modify: Tests/CMakeLists.txt

**Consumes:** The Task 1 decoded reference and the Task 2 payload source.

**Produces:** Compatible target mutation, one undo entry per accepted drop, and no direct mutation from AssetBrowserPanel.

- [ ] **Step 1: Write assignment tests before implementation**

~~~cpp
TEST(EditorAssetDrag_MeshAssignmentRoundTripsThroughUndo)
{
    World world;
    const EntityID entity = MakeMeshEntity(world, "Models/old.obj");
    const auto before = Spark::SerializeWorld(world);
    std::string error;
    ASSERT_TRUE(SparkEditor::ApplyEditorAssetAssignment(
        world, entity, SparkEditor::EditorAssetAssignmentTarget::MeshRendererMesh,
        {SparkEditor::EditorAssetKind::Mesh, "Models/new.obj"}, error));
    EXPECT_EQ(world.GetComponent<MeshRenderer>(entity)->meshPath, std::string("Models/new.obj"));
    EXPECT_NE(Spark::SerializeWorld(world), before);
}

TEST(EditorAssetDrag_TextureCannotOverwriteMeshPath)
{
    World world;
    const EntityID entity = MakeMeshEntity(world, "Models/old.obj");
    std::string error;
    EXPECT_FALSE(SparkEditor::ApplyEditorAssetAssignment(
        world, entity, SparkEditor::EditorAssetAssignmentTarget::MeshRendererMesh,
        {SparkEditor::EditorAssetKind::Texture, "Textures/grid.png"}, error));
    EXPECT_EQ(world.GetComponent<MeshRenderer>(entity)->meshPath, std::string("Models/old.obj"));
}
~~~

- [ ] **Step 2: Run the focused assignment tests and confirm they fail**

Run:

~~~powershell
$env:SPARK_TEST_NAME='EditorAssetDrag_'
$env:SPARK_TEST_EXPECT_COUNT='2'
& .\build\windows-release\bin\Release\SparkTests.exe --quiet --empty-is-error
~~~

Expected: compile failure naming EditorAssetAssignment.

- [ ] **Step 3: Implement pure compatible assignment**

Apply only these valid pairs:

| Target | Expected kind | Field |
| --- | --- | --- |
| MeshRendererMesh | Mesh | MeshRenderer::meshPath |
| MeshRendererMaterial | Material | MeshRenderer::materialPath |
| SpriteTexture | Texture | SpriteRenderer::texturePath |
| DecalTexture | Texture | DecalComponent::texturePath |
| NineSliceTexture | Texture | NineSliceSprite::texturePath |
| BillboardTexture | Texture | BillboardComponent::texturePath |
| TrailMaterial | Material | TrailRendererComponent::materialPath |

The helper validates registry validity and component presence, compares the old and new values, changes one field only after every check succeeds, and returns false with a human-readable error for every rejected case. It owns no notification, snapshot, dirty flag, or CommandHistory call.

- [ ] **Step 4: Put every accepted drop through EditorUI**

Implement the exact EditorUI entrypoint:

~~~cpp
bool EditorUI::ApplyAssetReferenceToSelection(
    EditorAssetAssignmentTarget target, const EditorAssetReference& reference)
{
    if (!m_world || m_selectedEntity == entt::null)
        return false;
    const std::string before = CaptureDocumentSnapshot();
    std::string error;
    if (!ApplyEditorAssetAssignment(*m_world, m_selectedEntity, target, reference, error)) {
        ShowNotification(error, "error");
        return false;
    }
    if (!RecordAppliedDocumentMutation(before, "Assign " + reference.projectRelativePath))
        return false;
    m_sceneModified = true;
    return true;
}
~~~

After a successful command, record the same bounded description with EditorCrashHandler::RecordOperation. Reject before taking a command snapshot if the payload fails DecodeEditorAssetReference against the active Assets root.

- [ ] **Step 5: Add explicit ImGui targets**

In InspectorPanel::RenderWorldBackedInspector, render one labelled drop target directly below each matching component header. Decode only SPARK_EDITOR_ASSET_REF, call EditorUI::ApplyAssetReferenceToSelection with the matching enum, and show the returned error as an editor notification. Do not make a generic string widget accept all payloads.

In SceneViewPanel, add a mesh-only drop target over the active viewport. Wire a non-owning EditorUI pointer with SetAssetDropSink. Implement EditorUI::CreateDocumentMeshFromAsset to create Transform and MeshRenderer components, set meshPath, select the new entity, and record exactly one command using the existing CreateDocumentEntity snapshot pattern.

- [ ] **Step 6: Add command-history integration assertions**

Drive EditorUI::ApplyAssetReferenceToSelection in a production-source test with a real World. Assert one undo count after a valid assignment; Undo restores the prior value; Redo restores the new value; malformed payload and incompatible target leave undo count unchanged. Add the matching scene-view mesh-create test, including undo and redo of the entity itself.

- [ ] **Step 7: Run focused assignment verification**

Run:

~~~powershell
$env:SPARK_TEST_NAME='EditorAssetDrag_'
& .\build\windows-release\bin\Release\SparkTests.exe --quiet --empty-is-error
~~~

Expected: 0 failed; each accepted assignment has a one-command undo/redo assertion.

- [ ] **Step 8: Commit the assignment slice**

~~~powershell
git add -- SparkEditor/Source/AssetPipeline/EditorAssetAssignment.h SparkEditor/Source/AssetPipeline/EditorAssetAssignment.cpp SparkEditor/Source/Core/EditorUI.h SparkEditor/Source/Core/EditorUI.cpp SparkEditor/Source/Panels/InspectorPanel.cpp SparkEditor/Source/Panels/SceneViewPanel.h SparkEditor/Source/Panels/SceneViewPanel.cpp Tests/TestEditorAssetAssignment.cpp Tests/CMakeLists.txt SparkEditor/CMakeLists.txt
git commit -m "feat(editor): make asset drops undo-safe"
~~~

### Task 4: Saved Scene and Installed Package Round Trip

**Files:**
- Create: Tests/TestEditorCookPackageRoundTrip.cpp
- Modify: Tests/CMakeLists.txt
- Modify: .github/workflows/build.yml
- Modify: wiki/gameplay-tools/SparkEditor.md
- Modify: wiki/gameplay-tools/Asset-Pipeline.md
- Modify: wiki/gameplay-tools/Game-Packaging.md

**Consumes:** Task 3 assigned project-relative references and the ASSET-220 canonical cook/package entrypoint.

**Produces:** A non-interactive project fixture that validates saved content and installed runtime asset resolution under a foreign working directory.

- [ ] **Step 1: Write the end-to-end fixture test**

~~~cpp
TEST(EditorCookPackage_AuthorSaveReopenInstalledRuntime)
{
    ScopedPackagedProject project;
    project.WriteObj("Assets/Models/crate.obj");
    project.WritePng("Assets/Textures/grid.png");
    project.WriteBasicMaterial("Assets/Materials/crate.json", "Textures/grid.png");

    World authored;
    const EntityID entity = MakeMeshEntity(authored, "");
    AssignFixtureReferences(authored, entity,
        "Models/crate.obj", "Materials/crate.json");
    ASSERT_TRUE(Spark::SaveWorld(authored, project.ScenePath().string()));

    World reopened;
    ASSERT_TRUE(Spark::LoadWorld(reopened, project.ScenePath().string()));
    EXPECT_EQ(reopened.GetComponent<MeshRenderer>(entity)->meshPath, std::string("Models/crate.obj"));

    ASSERT_TRUE(project.CookAndPackage());
    ASSERT_TRUE(project.RunInstalledRuntimeFromForeignWorkingDirectory());
    EXPECT_FALSE(project.InstalledRuntimeTouchedAuthoringTree());
}
~~~

- [ ] **Step 2: Run it and confirm the absent fixture route fails**

Run:

~~~powershell
$env:SPARK_TEST_NAME='EditorCookPackage_'
$env:SPARK_TEST_EXPECT_COUNT='1'
& .\build\windows-shipping\bin\MinSizeRel\SparkTests.exe --quiet --empty-is-error
~~~

Expected: a focused failure until the fixture invokes the production cook/package path.

- [ ] **Step 3: Implement the fixture through production components**

Use ProjectManager materialization plus Spark::SaveWorld/Spark::LoadWorld to author the project. Invoke the canonical ASSET-220 cooker/package API or process entrypoint; do not copy an Assets folder in test-only code. Launch the installed runtime with its working directory deliberately outside the package and capture the runtime’s resolved asset report. Assert the report contains the three project-relative references and no path under the temporary authoring root.

- [ ] **Step 4: Register the test with the release labels**

Add a CTest named EditorCookPackage_RoundTrip with labels editor-integration;editor-package-roundtrip. In build.yml, run:

~~~bash
ctest --test-dir build/windows-shipping -C MinSizeRel -L editor-integration --output-on-failure --no-tests=error
ctest --test-dir build/windows-shipping -C MinSizeRel -L editor-package-roundtrip --output-on-failure --no-tests=error
~~~

Upload each JUnit or console result with a name containing the exact GitHub SHA. The job result must be unavailable or failed when the labelled test is absent; it cannot silently accept zero tests.

- [ ] **Step 5: Run the local shipping gate**

Run:

~~~powershell
ctest --test-dir build/windows-shipping -C MinSizeRel -L editor-integration --output-on-failure --no-tests=error
ctest --test-dir build/windows-shipping -C MinSizeRel -L editor-package-roundtrip --output-on-failure --no-tests=error
~~~

Expected: both labels execute at least one test and return zero failures.

- [ ] **Step 6: Update user-facing documentation from observed behavior**

Document only the tested mesh/material/texture formats, the typed drag/drop targets, preview unavailable states, and the fact that audio assignment waits on a packaged sound-name registry. Explain that a packaged runtime resolves project-relative references without the authoring tree.

- [ ] **Step 7: Commit the round-trip evidence slice**

~~~powershell
git add -- Tests/TestEditorCookPackageRoundTrip.cpp Tests/CMakeLists.txt .github/workflows/build.yml wiki/gameplay-tools/SparkEditor.md wiki/gameplay-tools/Asset-Pipeline.md wiki/gameplay-tools/Game-Packaging.md
git commit -m "test(editor): prove asset package round trip"
~~~

## Final Verification

- [ ] Configure and compile source changes with the release toolchain.
- [ ] Run every EditorAssetReference_, EditorPreview_, EditorAssetDrag_, and EditorCookPackage_ test using SPARK_TEST_NAME and --empty-is-error.
- [ ] Run the full SparkTests suite in the affected Windows Release configuration.
- [ ] Run both shipping CTest labels with --no-tests=error.
- [ ] Inspect git diff --check and a targeted git diff before each commit.
- [ ] Obtain exact-SHA successful CI evidence for editor-integration and editor-package-roundtrip before changing EDT-210 status.
