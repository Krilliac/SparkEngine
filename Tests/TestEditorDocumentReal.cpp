/**
 * @file TestEditorDocumentReal.cpp
 * @brief EDT-210: the editor's document-level undo commands, driven through the
 *        production SparkEditor::EditorDocument that EditorUI forwards to.
 *
 * Create (every GameObject-menu kind), delete and applied-edit recording each
 * record exactly one CommandHistory entry whose undo reproduces the prior
 * SerializeWorld snapshot byte for byte and whose redo reproduces the
 * post-mutation snapshot byte for byte.
 */

#include "TestFramework.h"

#include "CommandHistory.h"
#include "Core/EditorDocument.h"
#include "SceneManager/ReflectedSceneSerializer.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace
{
    Spark::Editor::CommandHistory& History()
    {
        return Spark::Editor::CommandHistory::GetInstance();
    }

    /// Every name the editor's GameObject menus, palette and placement panel pass to CreateDocumentEntity.
    const std::vector<std::string>& MenuEntityNames()
    {
        static const std::vector<std::string> names = {
            "Empty",
            "Cube",
            "Sphere",
            "Cylinder",
            "Plane",
            "Camera",
            "Directional Light",
            "Point Light",
            "Spot Light",
            "Sprite",
            "Animated Sprite",
            "Tilemap",
            "Camera 2D",
            "Parallax Background",
            "Nine-Slice Sprite",
            "Trigger Volume",
            "Post-Process Volume",
            "Fog Volume",
            "Audio Reverb Zone",
            "Wind Zone",
            "Cinematic Trigger",
            "Area Boundary",
            "Reflection Probe",
            "Light Probe",
            "Water Plane",
            "Spawn Point",
            "NavMesh Obstacle",
            "Occluder",
            "Billboard",
            "Destructible",
            "Dialogue Trigger",
            "Physics Joint",
            "Character Controller",
            "Vehicle",
            "Cover Point",
            "Tactical Point",
            "Nav Region",
            "Nav Link",
            "Skybox",
            "Trail Renderer",
            "Text 3D",
            "Foliage Volume",
            "Ragdoll",
            "Soft Body",
            "Constant Force",
            "Force Region",
            "Buoyancy Volume",
            "Spring Arm",
        };
        return names;
    }

    /// A document with one authored entity, so snapshots are never trivially empty.
    ::EntityID SeedDocument(SparkEditor::EditorDocument& document)
    {
        auto world = std::make_unique<::World>();
        const ::EntityID crate = world->CreateEntity("Crate");
        world->AddComponent<::Transform>(crate).position = {1.0f, 2.0f, 3.0f};
        document.ReplaceWorld(std::move(world));
        return crate;
    }
} // namespace

TEST(EditorUndo_Document_CreateEveryMenuTypeRoundTrips)
{
    History().Clear();
    SparkEditor::EditorDocument document;
    SeedDocument(document);

    for (const std::string& name : MenuEntityNames())
    {
        History().Clear();
        const std::string before = document.Capture();

        const bool created = document.CreateEntity(name);
        if (!created)
            std::printf("  CreateEntity refused menu kind '%s'\n", name.c_str());
        ASSERT_TRUE(created);
        EXPECT_EQ(History().UndoCount(), static_cast<size_t>(1));
        const std::string after = document.Capture();
        EXPECT_TRUE(after != before);
        const ::EntityID createdEntity = document.GetSelectedEntity();
        EXPECT_TRUE(createdEntity != entt::null);

        ASSERT_TRUE(History().Undo());
        EXPECT_TRUE(document.Capture() == before);

        ASSERT_TRUE(History().Redo());
        EXPECT_TRUE(document.Capture() == after);
        EXPECT_TRUE(document.GetSelectedEntity() == createdEntity);
    }

    History().Clear();
}

TEST(EditorUndo_Document_UnsupportedTypeLeavesNoEntityAndNoHistory)
{
    History().Clear();
    SparkEditor::EditorDocument document;
    SeedDocument(document);
    const std::string before = document.Capture();
    const size_t entityCount = document.GetWorld()->GetEntityCount();

    EXPECT_FALSE(document.CreateEntity("Not A Menu Entry"));
    EXPECT_EQ(History().UndoCount(), static_cast<size_t>(0));
    EXPECT_EQ(document.GetWorld()->GetEntityCount(), entityCount);
    EXPECT_TRUE(document.Capture() == before);

    History().Clear();
}

TEST(EditorUndo_Document_DeleteRoundTripsAndRestoresSelection)
{
    History().Clear();
    SparkEditor::EditorDocument document;
    SeedDocument(document);
    ASSERT_TRUE(document.CreateEntity("Point Light"));
    const ::EntityID light = document.GetSelectedEntity();
    ASSERT_TRUE(light != entt::null);
    History().Clear();

    // Nothing selected: nothing to delete, nothing recorded.
    document.SetSelectedEntity(entt::null);
    EXPECT_FALSE(document.DeleteSelected());
    EXPECT_EQ(History().UndoCount(), static_cast<size_t>(0));

    document.SetSelectedEntity(light);
    const std::string before = document.Capture();
    ASSERT_TRUE(document.DeleteSelected());
    const std::string after = document.Capture();
    EXPECT_EQ(History().UndoCount(), static_cast<size_t>(1));
    EXPECT_FALSE(document.GetWorld()->GetRegistry().valid(light));
    EXPECT_TRUE(document.GetSelectedEntity() == entt::null);

    ASSERT_TRUE(History().Undo());
    EXPECT_TRUE(document.Capture() == before);
    EXPECT_TRUE(document.GetSelectedEntity() == light);

    ASSERT_TRUE(History().Redo());
    EXPECT_TRUE(document.Capture() == after);
    EXPECT_TRUE(document.GetSelectedEntity() == entt::null);

    History().Clear();
}

TEST(EditorUndo_Document_RecordAppliedUnchangedIsNoop)
{
    History().Clear();
    SparkEditor::EditorDocument document;
    const ::EntityID crate = SeedDocument(document);
    std::vector<std::string> recorded;
    document.SetHooks([&recorded](const std::string& description) { recorded.push_back(description); }, {});

    const std::string before = document.Capture();
    EXPECT_FALSE(document.RecordApplied(before, "Edit Nothing"));
    EXPECT_FALSE(document.RecordApplied(std::string{}, "Edit Without Baseline"));
    EXPECT_EQ(History().UndoCount(), static_cast<size_t>(0));
    EXPECT_TRUE(recorded.empty());

    // The Inspector commit path: the edit is applied first, then recorded.
    document.GetWorld()->GetComponent<::Transform>(crate)->position.y = 9.0f;
    const std::string after = document.Capture();
    ASSERT_TRUE(document.RecordApplied(before, "Edit Transform"));
    EXPECT_EQ(History().UndoCount(), static_cast<size_t>(1));
    ASSERT_EQ(recorded.size(), static_cast<size_t>(1));
    EXPECT_EQ(recorded.front(), std::string("Edit Transform"));

    ASSERT_TRUE(History().Undo());
    EXPECT_TRUE(document.Capture() == before);
    ASSERT_TRUE(History().Redo());
    EXPECT_TRUE(document.Capture() == after);

    History().Clear();
}

TEST(EditorUndo_Document_RestoreRejectsMalformedSnapshot)
{
    History().Clear();
    SparkEditor::EditorDocument document;
    EXPECT_FALSE(document.Restore(std::string("{}"), entt::null)); // no World yet

    SeedDocument(document);
    int restoredCalls = 0;
    document.SetHooks({}, [&restoredCalls]() { ++restoredCalls; });
    const ::World* worldBefore = document.GetWorld();
    const std::string before = document.Capture();

    EXPECT_FALSE(document.Restore(std::string("{ not json"), entt::null));
    EXPECT_EQ(restoredCalls, 0);
    EXPECT_TRUE(document.GetWorld() == worldBefore);
    EXPECT_TRUE(document.Capture() == before);

    // A well-formed snapshot restores in place and keeps the World's address.
    EXPECT_TRUE(document.Restore(before, entt::null));
    EXPECT_EQ(restoredCalls, 1);
    EXPECT_TRUE(document.GetWorld() == worldBefore);
    EXPECT_TRUE(document.Capture() == before);

    History().Clear();
}
