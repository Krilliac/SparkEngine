/**
 * @file TestEditorUndoWorldMatrixReal.cpp
 * @brief EDT-210: every command-class World mutation entry point, run as one
 *        scripted edit session on one document, must round-trip through undo
 *        and redo without divergence.
 *
 * SparkEditor/world-mutation-inventory.json classifies every World/registry
 * mutation site in SparkEditor/Source; each "command" entry names
 * EditorUndo_WorldMatrix_EveryCommandEntryPointRoundTrips, and
 * Tests/Tools/test_editor_world_mutation_inventory.py fails when a site is
 * unlisted. This test drives those production entry points: the document's
 * create/delete and applied-edit commit (the Inspector path, including its
 * component add/remove and asset drop), and the SceneEditTools rename,
 * position/rotation/scale, align, reparent, duplicate and scene import commits.
 * After each step it snapshots the document; undo-all must reproduce each
 * snapshot in reverse byte for byte and redo-all must reproduce them forward.
 */

#include "TestFramework.h"

#include "CommandHistory.h"
#include "Core/EditorDocument.h"
#include "Core/Reflection.h"
#include "Gizmos/SceneEditTools.h"
#include "Panels/InspectorWorldAssetDrop.h"
#include "SceneManager/ReflectedSceneSerializer.h"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace
{
    Spark::Editor::CommandHistory& History()
    {
        return Spark::Editor::CommandHistory::GetInstance();
    }

    /// Records the document snapshot after every step of the scripted session.
    class EditSession
    {
      public:
        EditSession()
        {
            auto world = std::make_unique<::World>();
            const ::EntityID crate = world->CreateEntity("Crate");
            world->AddComponent<::Transform>(crate).position = {1.0f, 2.0f, 3.0f};
            m_document.ReplaceWorld(std::move(world));
            m_snapshots.push_back(m_document.Capture());
        }

        SparkEditor::EditorDocument& Document() { return m_document; }
        ::World& WorldRef() { return *m_document.GetWorld(); }
        const std::vector<std::string>& Snapshots() const { return m_snapshots; }

        /// A step must record exactly one history entry; its snapshot is kept.
        bool Step(const char* label, bool recorded)
        {
            m_labels.emplace_back(label);
            const bool oneEntry = recorded && History().UndoCount() == m_snapshots.size();
            if (!oneEntry)
                std::printf("  step '%s' did not record exactly one undo entry\n", label);
            m_snapshots.push_back(m_document.Capture());
            return oneEntry;
        }

        const std::string& Label(size_t step) const { return m_labels[step - 1]; }

        /// The Inspector commit path: the edit is applied, then recorded as one step.
        bool ApplyThenRecord(const std::function<void()>& edit, const std::string& description)
        {
            const std::string before = m_document.Capture();
            edit();
            return m_document.RecordApplied(before, description);
        }

      private:
        SparkEditor::EditorDocument m_document;
        std::vector<std::string> m_snapshots;
        std::vector<std::string> m_labels;
    };

    ::EntityID CreateAndSelect(SparkEditor::EditorDocument& document, const std::string& menuName, bool& recorded)
    {
        recorded = document.CreateEntity(menuName);
        return document.GetSelectedEntity();
    }

    bool FactoryEdit(::World& world, ::EntityID entity, const std::string& type, bool add)
    {
        auto& factory = Spark::ComponentFactory::Get();
        void* worldHandle = &world;
        const uint32_t rawEntity = static_cast<uint32_t>(entity);
        return add ? factory.AddComponent(type, worldHandle, rawEntity)
                   : factory.RemoveComponent(type, worldHandle, rawEntity);
    }
} // namespace

TEST(EditorUndo_WorldMatrix_EveryCommandEntryPointRoundTrips)
{
    History().Clear();
    EditSession session;
    SparkEditor::EditorDocument& document = session.Document();
    namespace Tools = SparkEditor::SceneEditTools;

    bool recorded = false;
    const ::EntityID cube = CreateAndSelect(document, "Cube", recorded);
    ASSERT_TRUE(session.Step("create primitive", recorded));
    const ::EntityID light = CreateAndSelect(document, "Point Light", recorded);
    ASSERT_TRUE(session.Step("create light", recorded));
    CreateAndSelect(document, "Sprite", recorded);
    ASSERT_TRUE(session.Step("create factory component", recorded));
    CreateAndSelect(document, "Camera", recorded);
    ASSERT_TRUE(session.Step("create camera", recorded));

    // Inspector: Add Component, field edit, Remove Component.
    ASSERT_TRUE(session.Step(
        "inspector add component",
        session.ApplyThenRecord([&]() { FactoryEdit(session.WorldRef(), cube, "TriggerVolumeComponent", true); },
                                "Add Component")));
    ASSERT_TRUE(session.Step(
        "inspector field edit",
        session.ApplyThenRecord([&]() { session.WorldRef().GetComponent<::Transform>(cube)->position.x = 7.5f; },
                                "Edit Transform")));
    ASSERT_TRUE(session.Step(
        "inspector remove component",
        session.ApplyThenRecord([&]() { FactoryEdit(session.WorldRef(), cube, "TriggerVolumeComponent", false); },
                                "Remove Component")));

    // Hierarchy and viewport commits.
    ASSERT_TRUE(session.Step("rename", Tools::CommitEntityRename(session.WorldRef(), cube, "Crate Stack")));
    const DirectX::XMFLOAT3 position = session.WorldRef().GetComponent<::Transform>(cube)->position;
    ASSERT_TRUE(session.Step(
        "position", Tools::CommitEntityPosition(session.WorldRef(), cube, position, {position.x, 5.0f, position.z})));
    ASSERT_TRUE(session.Step(
        "rotation", Tools::CommitEntityRotation(session.WorldRef(), cube, {0.0f, 0.0f, 0.0f}, {0.0f, 45.0f, 0.0f})));
    ASSERT_TRUE(session.Step(
        "scale", Tools::CommitEntityScale(session.WorldRef(), cube, {1.0f, 1.0f, 1.0f}, {2.0f, 1.0f, 2.0f})));
    ASSERT_TRUE(session.Step("align to ground", Tools::AlignEntityToGround(session.WorldRef(), cube)));
    ASSERT_TRUE(session.Step("reparent", Tools::CommitEntityReparent(session.WorldRef(), light, cube)));
    const ::EntityID duplicate = Tools::DuplicateEntity(session.WorldRef(), cube);
    ASSERT_TRUE(session.Step("duplicate", duplicate != entt::null));

    Tools::SceneObjectRecord record;
    record.type = "model";
    record.name = "Imported Barrel";
    record.model = "Assets/Meshes/barrel.obj";
    record.position[2] = -4.0f;
    ASSERT_TRUE(session.Step(
        "scene import",
        Tools::CommitSceneImport(session.WorldRef(), {record}, "Import Scene 'matrix.scene'").size() == 1));

    const SparkEditor::AssetDropResult drop = SparkEditor::ApplyWorldAssetDrop(
        session.WorldRef(), cube, "MeshRenderer", "meshPath", "Assets/Meshes/crate.obj",
        [&]() { return document.Capture(); }, [&](const std::string& before, const std::string& description)
        { return document.RecordApplied(before, description); });
    ASSERT_TRUE(session.Step("asset drop", drop == SparkEditor::AssetDropResult::Applied));

    document.SetSelectedEntity(duplicate);
    ASSERT_TRUE(session.Step("delete", document.DeleteSelected()));

    // Undo everything: each undo reproduces the previous snapshot exactly.
    const std::vector<std::string>& snapshots = session.Snapshots();
    const size_t steps = snapshots.size() - 1;
    EXPECT_EQ(History().UndoCount(), steps);
    for (size_t step = steps; step > 0; --step)
    {
        ASSERT_TRUE(History().Undo());
        const bool matches = document.Capture() == snapshots[step - 1];
        if (!matches)
            std::printf("  undo of step '%s' diverged\n", session.Label(step).c_str());
        EXPECT_TRUE(matches);
    }
    EXPECT_EQ(History().UndoCount(), static_cast<size_t>(0));

    // Redo everything: each redo reproduces the recorded snapshot exactly.
    for (size_t step = 1; step <= steps; ++step)
    {
        ASSERT_TRUE(History().Redo());
        const bool matches = document.Capture() == snapshots[step];
        if (!matches)
            std::printf("  redo of step '%s' diverged\n", session.Label(step).c_str());
        EXPECT_TRUE(matches);
    }
    EXPECT_EQ(History().RedoCount(), static_cast<size_t>(0));

    History().Clear();
}

TEST(EditorUndo_WorldMatrix_UndoThenNewEditTruncatesRedo)
{
    History().Clear();
    EditSession session;
    SparkEditor::EditorDocument& document = session.Document();

    bool recorded = false;
    const ::EntityID first = CreateAndSelect(document, "Cube", recorded);
    ASSERT_TRUE(session.Step("create first", recorded));
    const ::EntityID second = CreateAndSelect(document, "Sphere", recorded);
    ASSERT_TRUE(session.Step("create second", recorded));

    ASSERT_TRUE(History().Undo());
    EXPECT_FALSE(session.WorldRef().GetRegistry().valid(second));
    EXPECT_EQ(History().RedoCount(), static_cast<size_t>(1));

    // A new edit after an undo discards the undone step for good.
    ASSERT_TRUE(SparkEditor::SceneEditTools::CommitEntityRename(session.WorldRef(), first, "Renamed"));
    const std::string afterRename = document.Capture();
    EXPECT_EQ(History().RedoCount(), static_cast<size_t>(0));
    EXPECT_FALSE(History().Redo());
    EXPECT_TRUE(document.Capture() == afterRename);
    EXPECT_FALSE(session.WorldRef().GetRegistry().valid(second));

    const std::vector<std::string>& snapshots = session.Snapshots();
    ASSERT_TRUE(History().Undo()); // the rename
    EXPECT_TRUE(document.Capture() == snapshots[1]);
    ASSERT_TRUE(History().Undo()); // the first create
    EXPECT_TRUE(document.Capture() == snapshots[0]);
    EXPECT_FALSE(History().Undo());

    History().Clear();
}
