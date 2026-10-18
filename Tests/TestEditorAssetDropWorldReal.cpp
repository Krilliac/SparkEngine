/**
 * @file TestEditorAssetDropWorldReal.cpp
 * @brief EDT-210: asset drops onto the World-backed Inspector reach the document and its history.
 *
 * File > Open / Save author the reflected World, and the World-backed
 * Inspector renders MeshRenderer.meshPath / materialPath as plain reflected
 * string fields. SparkEditor::ApplyWorldAssetDrop is the drop target's
 * ImGui-free core: it validates the dragged reference against the field's
 * asset kind, writes the live component, and records the change as exactly
 * one CommandHistory entry through the Inspector's snapshot commit.
 *
 * These tests drive it against a real ::World, the real reflected scene
 * serializer and persistence, and the real CommandHistory. The commit and
 * restore callbacks mirror EditorUI::RecordAppliedDocumentMutation and
 * EditorUI::RestoreWorldSnapshot (both private to EditorUI), as in
 * TestEDT210InspectorEditCommitReal.cpp.
 */

#include "TestFramework.h"

#include "CommandHistory.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/CoreComponents.h"
#include "Panels/InspectorPendingWorldEdit.h"
#include "Panels/InspectorWorldAssetDrop.h"
#include "SceneManager/ReflectedSceneSerializer.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    using SparkEditor::ApplyWorldAssetDrop;
    using SparkEditor::AssetDropResult;
    using SparkEditor::AssetKindForField;
    using SparkEditor::EditorAssetKind;

    constexpr uint32_t kFieldItem = 0x1001u; // ImGui id of the edited Transform field

    Spark::Editor::CommandHistory& History()
    {
        return Spark::Editor::CommandHistory::GetInstance();
    }

    /// Mirrors the EditorUI snapshot-command pair used by the World-backed Inspector.
    struct EditorDocument
    {
        ::World world;
        int snapshotCalls = 0;

        std::string Snapshot() const { return Spark::SerializeWorld(world); }

        /// Order-independent document state (a snapshot restore reverses entity order).
        std::string State()
        {
            entt::registry& reg = world.GetRegistry();
            std::vector<::EntityID> ids;
            for (const auto [e] : reg.storage<entt::entity>().each())
                ids.push_back(e);
            std::sort(ids.begin(), ids.end());
            std::ostringstream out;
            for (const ::EntityID e : ids)
            {
                out << static_cast<uint32_t>(e) << ':';
                if (const auto* n = reg.try_get<::NameComponent>(e))
                    out << n->name;
                if (const auto* t = reg.try_get<::Transform>(e))
                    out << " T(" << t->position.x << ',' << t->position.y << ',' << t->position.z << ')';
                if (const auto* m = reg.try_get<::MeshRenderer>(e))
                    out << " M(" << m->meshPath << '|' << m->materialPath << ')';
                out << ';';
            }
            return out.str();
        }

        bool Restore(const std::string& json)
        {
            auto restored = std::make_unique<::World>();
            if (!Spark::DeserializeInto(*restored, json))
                return false;
            world.GetRegistry() = std::move(restored->GetRegistry());
            return true;
        }

        bool RecordApplied(const std::string& before, const std::string& description)
        {
            if (before.empty())
                return false;
            const std::string after = Snapshot();
            if (after == before)
                return false;
            History().Execute(std::make_unique<Spark::Editor::LambdaCommand>(
                [this, after]() { Restore(after); }, [this, before]() { Restore(before); }, description));
            return true;
        }

        std::function<std::string()> SnapshotFn()
        {
            return [this]()
            {
                ++snapshotCalls;
                return Snapshot();
            };
        }

        SparkEditor::InspectorPendingWorldEdit::CommitFn Commit()
        {
            return [this](const std::string& before, const std::string& description)
            { return RecordApplied(before, description); };
        }
    };

    struct PropScene
    {
        EditorDocument doc;
        ::EntityID prop = entt::null;
        ::EntityID bare = entt::null;
        std::string initial;

        PropScene()
        {
            History().Clear();
            prop = doc.world.CreateEntity("Prop");
            doc.world.AddComponent<::Transform>(prop).position = {1.0f, 2.0f, 3.0f};
            auto& renderer = doc.world.AddComponent<::MeshRenderer>(prop);
            renderer.meshPath = "Assets/Meshes/barrel.obj";
            renderer.materialPath = "Assets/Materials/wood.mat";
            bare = doc.world.CreateEntity("Bare");
            doc.world.AddComponent<::Transform>(bare);
            initial = doc.State();
            History().MarkSaved();
        }

        ~PropScene() { History().Clear(); }

        std::string MeshPath(::EntityID entity)
        {
            const auto* renderer = doc.world.GetRegistry().try_get<::MeshRenderer>(entity);
            return renderer ? renderer->meshPath : std::string("<no MeshRenderer>");
        }

        std::string MaterialPath(::EntityID entity)
        {
            const auto* renderer = doc.world.GetRegistry().try_get<::MeshRenderer>(entity);
            return renderer ? renderer->materialPath : std::string("<no MeshRenderer>");
        }

        AssetDropResult Drop(::EntityID entity, const char* component, const char* field, const std::string& reference)
        {
            return ApplyWorldAssetDrop(doc.world, entity, component, field, reference, doc.SnapshotFn(), doc.Commit());
        }
    };

    class ScopedSceneDirectory
    {
      public:
        ScopedSceneDirectory()
        {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            m_root = std::filesystem::temp_directory_path() / ("spark-edt210-asset-drop-" + std::to_string(stamp));
            std::error_code error;
            std::filesystem::remove_all(m_root, error);
            std::filesystem::create_directories(m_root / "Scenes", error);
        }

        ~ScopedSceneDirectory()
        {
            std::error_code error;
            std::filesystem::remove_all(m_root, error);
        }

        std::string ScenePath() const { return (m_root / "Scenes" / "AssetDrop.sparkscene").string(); }

      private:
        std::filesystem::path m_root;
    };
} // namespace

TEST(EditorAssetDrag_WorldDropRecordsOneUndoStepAndUndoRedoAreExact)
{
    PropScene scene;

    EXPECT_TRUE(AssetKindForField("MeshRenderer", "meshPath") == std::optional<EditorAssetKind>(EditorAssetKind::Mesh));
    EXPECT_TRUE(AssetKindForField("MeshRenderer", "materialPath") ==
                std::optional<EditorAssetKind>(EditorAssetKind::Material));

    EXPECT_EQ(static_cast<int>(scene.Drop(scene.prop, "MeshRenderer", "meshPath", "Assets/Meshes/crate.obj")),
              static_cast<int>(AssetDropResult::Applied));
    EXPECT_EQ(History().UndoCount(), static_cast<size_t>(1));
    EXPECT_TRUE(History().IsModified());
    EXPECT_STR_CONTAINS(History().GetUndoDescription(), "Assign Mesh Asset");
    EXPECT_EQ(scene.MeshPath(scene.prop), std::string("Assets/Meshes/crate.obj"));
    EXPECT_EQ(scene.MaterialPath(scene.prop), std::string("Assets/Materials/wood.mat"));
    const std::string dropped = scene.doc.State();

    EXPECT_TRUE(History().Undo());
    EXPECT_EQ(scene.doc.State(), scene.initial);

    EXPECT_TRUE(History().Redo());
    EXPECT_EQ(scene.doc.State(), dropped);

    // The material slot records its own, separately labelled step.
    EXPECT_EQ(static_cast<int>(scene.Drop(scene.prop, "MeshRenderer", "materialPath", "Assets/Materials/metal.mat")),
              static_cast<int>(AssetDropResult::Applied));
    EXPECT_EQ(History().UndoCount(), static_cast<size_t>(2));
    EXPECT_STR_CONTAINS(History().GetUndoDescription(), "Assign Material Asset");
    EXPECT_TRUE(History().Undo());
    EXPECT_EQ(scene.doc.State(), dropped);
}

TEST(EditorAssetDrag_WorldDropWrongKindOrTraversalRecordsNothing)
{
    PropScene scene;

    struct Case
    {
        ::EntityID entity;
        const char* component;
        const char* field;
        const char* reference;
        AssetDropResult expected;
    };
    const std::vector<Case> cases = {
        {scene.prop, "MeshRenderer", "meshPath", "Assets/Materials/metal.mat", AssetDropResult::Rejected},
        {scene.prop, "MeshRenderer", "materialPath", "Assets/Meshes/crate.obj", AssetDropResult::Rejected},
        {scene.prop, "MeshRenderer", "meshPath", "Assets/../secrets/crate.obj", AssetDropResult::Rejected},
        {scene.prop, "MeshRenderer", "meshPath", "C:/Projects/Game/Assets/Meshes/crate.obj", AssetDropResult::Rejected},
        {scene.prop, "Transform", "position", "Assets/Meshes/crate.obj", AssetDropResult::Rejected},
        {scene.bare, "MeshRenderer", "meshPath", "Assets/Meshes/crate.obj", AssetDropResult::NoComponent},
    };
    for (const Case& c : cases)
    {
        EXPECT_EQ(static_cast<int>(scene.Drop(c.entity, c.component, c.field, c.reference)),
                  static_cast<int>(c.expected));
    }
    EXPECT_FALSE(AssetKindForField("Transform", "position").has_value());

    // Validation happens before anything is captured or written.
    EXPECT_EQ(scene.doc.snapshotCalls, 0);
    EXPECT_EQ(History().UndoCount(), static_cast<size_t>(0));
    EXPECT_FALSE(History().IsModified());
    EXPECT_EQ(scene.doc.State(), scene.initial);

    // A commit that records nothing must not leave an un-undoable write behind.
    const auto refuse = [](const std::string&, const std::string&) { return false; };
    EXPECT_EQ(static_cast<int>(ApplyWorldAssetDrop(scene.doc.world, scene.prop, "MeshRenderer", "meshPath",
                                                   "Assets/Meshes/crate.obj", scene.doc.SnapshotFn(), refuse)),
              static_cast<int>(AssetDropResult::Rejected));
    EXPECT_EQ(scene.MeshPath(scene.prop), std::string("Assets/Meshes/barrel.obj"));
    EXPECT_EQ(scene.doc.State(), scene.initial);
    EXPECT_EQ(History().UndoCount(), static_cast<size_t>(0));
}

TEST(EditorAssetDrag_WorldDropSameValueRecordsNothing)
{
    PropScene scene;

    EXPECT_EQ(static_cast<int>(scene.Drop(scene.prop, "MeshRenderer", "meshPath", "Assets/Meshes/barrel.obj")),
              static_cast<int>(AssetDropResult::Unchanged));
    EXPECT_EQ(static_cast<int>(scene.Drop(scene.prop, "MeshRenderer", "materialPath", "Assets/Materials/wood.mat")),
              static_cast<int>(AssetDropResult::Unchanged));
    EXPECT_EQ(scene.doc.snapshotCalls, 0);
    EXPECT_EQ(History().UndoCount(), static_cast<size_t>(0));
    EXPECT_FALSE(History().IsModified());
    EXPECT_EQ(scene.doc.State(), scene.initial);
}

TEST(EditorAssetDrag_WorldDropAfterPendingFieldEditKeepsBothStepsOrdered)
{
    PropScene scene;
    SparkEditor::InspectorPendingWorldEdit pending;
    auto commit = scene.doc.Commit();

    // A Transform field is still being typed into when the asset is dropped.
    const std::string beforeTyping = scene.doc.Snapshot();
    scene.doc.world.GetRegistry().get<::Transform>(scene.prop).position.x = 42.0f;
    pending.NoteChange(&scene.doc.world, scene.prop, beforeTyping, "Edit Transform", kFieldItem,
                       History().GetEditSequence());
    const std::string afterTyping = scene.doc.State();

    // InspectorPanel flushes the open gesture before applying the drop.
    EXPECT_EQ(static_cast<int>(pending.Flush(&scene.doc.world, History().GetEditSequence(), commit)),
              static_cast<int>(SparkEditor::InspectorPendingWorldEdit::Outcome::Committed));
    EXPECT_EQ(static_cast<int>(scene.Drop(scene.prop, "MeshRenderer", "meshPath", "Assets/Meshes/crate.obj")),
              static_cast<int>(AssetDropResult::Applied));
    EXPECT_EQ(History().UndoCount(), static_cast<size_t>(2));
    const std::string afterDrop = scene.doc.State();

    EXPECT_TRUE(History().Undo());
    EXPECT_EQ(scene.doc.State(), afterTyping);
    EXPECT_EQ(scene.MeshPath(scene.prop), std::string("Assets/Meshes/barrel.obj"));
    EXPECT_STR_CONTAINS(History().GetUndoDescription(), "Edit Transform");

    EXPECT_TRUE(History().Undo());
    EXPECT_EQ(scene.doc.State(), scene.initial);

    EXPECT_TRUE(History().Redo());
    EXPECT_TRUE(History().Redo());
    EXPECT_EQ(scene.doc.State(), afterDrop);
}

TEST(EditorAssetDrag_WorldDropSurvivesReflectedSaveAndReload)
{
    PropScene scene;
    ScopedSceneDirectory directory;

    EXPECT_EQ(static_cast<int>(scene.Drop(scene.prop, "MeshRenderer", "meshPath", "Assets/Meshes/crate.obj")),
              static_cast<int>(AssetDropResult::Applied));
    EXPECT_EQ(static_cast<int>(scene.Drop(scene.prop, "MeshRenderer", "materialPath", "Assets/Materials/metal.mat")),
              static_cast<int>(AssetDropResult::Applied));
    ASSERT_TRUE(Spark::SaveWorld(scene.doc.world, directory.ScenePath()));

    ::World reloaded;
    std::string error;
    ASSERT_TRUE(Spark::LoadWorld(reloaded, directory.ScenePath(), &error));

    int renderers = 0;
    for (const auto [entity, renderer] : reloaded.GetRegistry().view<::MeshRenderer>().each())
    {
        ++renderers;
        EXPECT_EQ(renderer.meshPath, std::string("Assets/Meshes/crate.obj"));
        EXPECT_EQ(renderer.materialPath, std::string("Assets/Materials/metal.mat"));
        const auto* name = reloaded.GetRegistry().try_get<::NameComponent>(entity);
        EXPECT_TRUE(name != nullptr && name->name == "Prop");
    }
    EXPECT_EQ(renderers, 1);
}
