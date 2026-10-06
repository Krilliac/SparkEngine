/**
 * @file TestEditorUndoHierarchyReal.cpp
 * @brief Real-class tests for the World-backed Hierarchy undo surface.
 *
 * The World-mode hierarchy used to offer create/delete only, with rename and
 * duplicate documented as "deferred" — a delete or rename could not be undone.
 * Rename now routes through SceneEditTools::CommitEntityRename and duplicate
 * through SceneEditTools::DuplicateEntity, so every hierarchy mutation is a
 * CommandHistory entry, and so are the drag-drop reparent
 * (SceneEditTools::CommitEntityReparent) and the Scene Import panel's import
 * (SceneEditTools::CommitSceneImport). These tests exercise those production
 * helpers directly; undo and redo must reproduce SerializeWorld byte for byte.
 */

#include "TestFramework.h"

#include "CommandHistory.h"
#include "Gizmos/SceneEditTools.h"
#include "SceneManager/ReflectedSceneSerializer.h"

#include <string>
#include <vector>

namespace
{
    std::string NameOf(::World& world, ::EntityID entity)
    {
        const ::NameComponent* name = world.GetRegistry().try_get<::NameComponent>(entity);
        return name ? name->name : std::string{};
    }

    std::vector<::EntityID> ChildrenOf(::World& world, ::EntityID parent)
    {
        const ::Transform* transform = world.GetComponent<::Transform>(parent);
        return transform ? transform->children : std::vector<::EntityID>{};
    }
} // namespace

TEST(EditorUndoHierarchy_RenameIsUndoableAndRedoable)
{
    Spark::Editor::CommandHistory::GetInstance().Clear();

    ::World world;
    const ::EntityID entity = world.CreateEntity("Soldier");
    EXPECT_EQ(NameOf(world, entity), std::string("Soldier"));

    EXPECT_TRUE(SparkEditor::SceneEditTools::CommitEntityRename(world, entity, "Sniper"));
    EXPECT_EQ(NameOf(world, entity), std::string("Sniper"));
    EXPECT_EQ(Spark::Editor::CommandHistory::GetInstance().UndoCount(), static_cast<size_t>(1));
    EXPECT_STR_CONTAINS(Spark::Editor::CommandHistory::GetInstance().GetUndoDescription(), "Rename");

    EXPECT_TRUE(Spark::Editor::CommandHistory::GetInstance().Undo());
    EXPECT_EQ(NameOf(world, entity), std::string("Soldier"));

    EXPECT_TRUE(Spark::Editor::CommandHistory::GetInstance().Redo());
    EXPECT_EQ(NameOf(world, entity), std::string("Sniper"));

    Spark::Editor::CommandHistory::GetInstance().Clear();
}

TEST(EditorUndoHierarchy_RenameRejectsEmptyAndUnchangedNames)
{
    Spark::Editor::CommandHistory::GetInstance().Clear();

    ::World world;
    const ::EntityID entity = world.CreateEntity("Crate");

    // An empty inline-rename field and a no-op rename must not consume an undo step.
    EXPECT_FALSE(SparkEditor::SceneEditTools::CommitEntityRename(world, entity, ""));
    EXPECT_FALSE(SparkEditor::SceneEditTools::CommitEntityRename(world, entity, "Crate"));
    EXPECT_EQ(Spark::Editor::CommandHistory::GetInstance().UndoCount(), static_cast<size_t>(0));
    EXPECT_EQ(NameOf(world, entity), std::string("Crate"));

    Spark::Editor::CommandHistory::GetInstance().Clear();
}

TEST(EditorUndoHierarchy_RenameOfUnnamedEntityUndoesBackToNoComponent)
{
    Spark::Editor::CommandHistory::GetInstance().Clear();

    ::World world;
    entt::registry& registry = world.GetRegistry();
    const ::EntityID entity = static_cast<::EntityID>(registry.create());
    EXPECT_FALSE(registry.try_get<::NameComponent>(entity) != nullptr);

    EXPECT_TRUE(SparkEditor::SceneEditTools::CommitEntityRename(world, entity, "Named"));
    EXPECT_EQ(NameOf(world, entity), std::string("Named"));

    EXPECT_TRUE(Spark::Editor::CommandHistory::GetInstance().Undo());
    EXPECT_FALSE(registry.try_get<::NameComponent>(entity) != nullptr);

    Spark::Editor::CommandHistory::GetInstance().Clear();
}

TEST(EditorUndoHierarchy_RenameOfDestroyedEntityIsRefused)
{
    Spark::Editor::CommandHistory::GetInstance().Clear();

    ::World world;
    const ::EntityID entity = world.CreateEntity("Gone");
    world.DestroyEntity(entity);

    EXPECT_FALSE(SparkEditor::SceneEditTools::CommitEntityRename(world, entity, "Ghost"));
    EXPECT_EQ(Spark::Editor::CommandHistory::GetInstance().UndoCount(), static_cast<size_t>(0));

    Spark::Editor::CommandHistory::GetInstance().Clear();
}

TEST(EditorUndoHierarchy_DuplicateIsUndoableAndRedoable)
{
    Spark::Editor::CommandHistory::GetInstance().Clear();

    ::World world;
    const ::EntityID source = world.CreateEntity("Barrel");
    world.AddComponent<::Transform>(source);
    const size_t before = world.GetRegistry().storage<entt::entity>().size();

    const ::EntityID copy = SparkEditor::SceneEditTools::DuplicateEntity(world, source);
    EXPECT_TRUE(copy != entt::null);
    EXPECT_GT(world.GetRegistry().storage<entt::entity>().size(), before);
    EXPECT_TRUE(world.GetRegistry().valid(copy));

    EXPECT_TRUE(Spark::Editor::CommandHistory::GetInstance().Undo());
    EXPECT_FALSE(world.GetRegistry().valid(copy));

    EXPECT_TRUE(Spark::Editor::CommandHistory::GetInstance().Redo());
    EXPECT_TRUE(world.GetRegistry().valid(copy));
    EXPECT_STR_CONTAINS(NameOf(world, copy), "Barrel");

    Spark::Editor::CommandHistory::GetInstance().Clear();
}

TEST(EditorUndoHierarchy_RenameThenDuplicateUndoInReverseOrder)
{
    Spark::Editor::CommandHistory::GetInstance().Clear();

    ::World world;
    const ::EntityID entity = world.CreateEntity("Alpha");
    world.AddComponent<::Transform>(entity);

    EXPECT_TRUE(SparkEditor::SceneEditTools::CommitEntityRename(world, entity, "Beta"));
    const ::EntityID copy = SparkEditor::SceneEditTools::DuplicateEntity(world, entity);
    EXPECT_TRUE(copy != entt::null);
    EXPECT_EQ(Spark::Editor::CommandHistory::GetInstance().UndoCount(), static_cast<size_t>(2));

    EXPECT_TRUE(Spark::Editor::CommandHistory::GetInstance().Undo()); // undo duplicate
    EXPECT_FALSE(world.GetRegistry().valid(copy));
    EXPECT_EQ(NameOf(world, entity), std::string("Beta"));

    EXPECT_TRUE(Spark::Editor::CommandHistory::GetInstance().Undo()); // undo rename
    EXPECT_EQ(NameOf(world, entity), std::string("Alpha"));

    Spark::Editor::CommandHistory::GetInstance().Clear();
}

TEST(EditorUndoHierarchy_ReparentRoundTripsBothChildrenLists)
{
    Spark::Editor::CommandHistory::GetInstance().Clear();

    ::World world;
    const ::EntityID oldParent = world.CreateEntity("OldParent");
    world.AddComponent<::Transform>(oldParent);
    const ::EntityID newParent = world.CreateEntity("NewParent");
    world.AddComponent<::Transform>(newParent);
    const ::EntityID first = world.CreateEntity("First");
    const ::EntityID moved = world.CreateEntity("Moved");
    const ::EntityID last = world.CreateEntity("Last");
    const ::EntityID resident = world.CreateEntity("Resident");
    for (const ::EntityID child : {first, moved, last})
        EXPECT_TRUE(world.SetParent(child, oldParent));
    EXPECT_TRUE(world.SetParent(resident, newParent));
    const std::string before = Spark::SerializeWorld(world);

    ASSERT_TRUE(SparkEditor::SceneEditTools::CommitEntityReparent(world, moved, newParent));
    EXPECT_EQ(Spark::Editor::CommandHistory::GetInstance().UndoCount(), static_cast<size_t>(1));
    EXPECT_TRUE(world.GetComponent<::Transform>(moved)->parent == newParent);
    EXPECT_TRUE(ChildrenOf(world, oldParent) == std::vector<::EntityID>({first, last}));
    EXPECT_TRUE(ChildrenOf(world, newParent) == std::vector<::EntityID>({resident, moved}));
    const std::string after = Spark::SerializeWorld(world);

    // Undo puts the child back in its original slot, between its siblings.
    ASSERT_TRUE(Spark::Editor::CommandHistory::GetInstance().Undo());
    EXPECT_TRUE(world.GetComponent<::Transform>(moved)->parent == oldParent);
    EXPECT_TRUE(ChildrenOf(world, oldParent) == std::vector<::EntityID>({first, moved, last}));
    EXPECT_TRUE(ChildrenOf(world, newParent) == std::vector<::EntityID>({resident}));
    EXPECT_TRUE(Spark::SerializeWorld(world) == before);

    ASSERT_TRUE(Spark::Editor::CommandHistory::GetInstance().Redo());
    EXPECT_TRUE(ChildrenOf(world, oldParent) == std::vector<::EntityID>({first, last}));
    EXPECT_TRUE(ChildrenOf(world, newParent) == std::vector<::EntityID>({resident, moved}));
    EXPECT_TRUE(Spark::SerializeWorld(world) == after);

    // Parenting an entity that had no Transform adds one; undo removes it again.
    const ::EntityID bare = world.CreateEntity("Bare");
    const std::string beforeBare = Spark::SerializeWorld(world);
    ASSERT_TRUE(SparkEditor::SceneEditTools::CommitEntityReparent(world, bare, newParent));
    EXPECT_TRUE(world.GetRegistry().all_of<::Transform>(bare));
    ASSERT_TRUE(Spark::Editor::CommandHistory::GetInstance().Undo());
    EXPECT_FALSE(world.GetRegistry().all_of<::Transform>(bare));
    EXPECT_TRUE(Spark::SerializeWorld(world) == beforeBare);

    Spark::Editor::CommandHistory::GetInstance().Clear();
}

TEST(EditorUndoHierarchy_ReparentRefusesCycle)
{
    Spark::Editor::CommandHistory::GetInstance().Clear();

    ::World world;
    const ::EntityID root = world.CreateEntity("Root");
    world.AddComponent<::Transform>(root);
    const ::EntityID child = world.CreateEntity("Child");
    const ::EntityID grandchild = world.CreateEntity("Grandchild");
    EXPECT_TRUE(world.SetParent(child, root));
    EXPECT_TRUE(world.SetParent(grandchild, child));
    const ::EntityID gone = world.CreateEntity("Gone");
    world.DestroyEntity(gone);
    const std::string before = Spark::SerializeWorld(world);

    EXPECT_FALSE(SparkEditor::SceneEditTools::CommitEntityReparent(world, root, grandchild)); // descendant
    EXPECT_FALSE(SparkEditor::SceneEditTools::CommitEntityReparent(world, root, root));       // self
    EXPECT_FALSE(SparkEditor::SceneEditTools::CommitEntityReparent(world, child, root));      // no-op
    EXPECT_FALSE(SparkEditor::SceneEditTools::CommitEntityReparent(world, child, gone));      // invalid parent
    EXPECT_FALSE(SparkEditor::SceneEditTools::CommitEntityReparent(world, gone, root));       // invalid child
    EXPECT_EQ(Spark::Editor::CommandHistory::GetInstance().UndoCount(), static_cast<size_t>(0));
    EXPECT_TRUE(Spark::SerializeWorld(world) == before);

    Spark::Editor::CommandHistory::GetInstance().Clear();
}

TEST(EditorUndoHierarchy_ImportRoundTripsAndRedoReusesIds)
{
    Spark::Editor::CommandHistory::GetInstance().Clear();

    ::World world;
    world.CreateEntity("Existing");
    const std::string before = Spark::SerializeWorld(world);

    SparkEditor::SceneEditTools::SceneObjectRecord cube;
    cube.type = "cube";
    cube.name = "Wall";
    cube.position[1] = 2.0f;
    cube.scale[0] = 4.0f;
    SparkEditor::SceneEditTools::SceneObjectRecord model;
    model.type = "model";
    model.name = "Crate";
    model.model = "Models/crate.obj";
    model.material = "Materials/crate.json";

    const std::vector<::EntityID> created =
        SparkEditor::SceneEditTools::CommitSceneImport(world, {cube, model}, "Import Scene 'test.scene' (2 entities)");
    ASSERT_EQ(created.size(), static_cast<size_t>(2));
    EXPECT_EQ(Spark::Editor::CommandHistory::GetInstance().UndoCount(), static_cast<size_t>(1));
    EXPECT_EQ(NameOf(world, created[0]), std::string("Wall"));
    EXPECT_EQ(world.GetComponent<::MeshRenderer>(created[0])->meshPath, std::string("__spark_primitive_Cube.obj"));
    EXPECT_EQ(world.GetComponent<::MeshRenderer>(created[1])->meshPath, std::string("Models/crate.obj"));
    EXPECT_EQ(world.GetComponent<::MeshRenderer>(created[1])->materialPath, std::string("Materials/crate.json"));
    EXPECT_NEAR(world.GetComponent<::Transform>(created[0])->position.y, 2.0f, 1e-6f);
    const std::string after = Spark::SerializeWorld(world);

    ASSERT_TRUE(Spark::Editor::CommandHistory::GetInstance().Undo());
    EXPECT_FALSE(world.GetRegistry().valid(created[0]));
    EXPECT_FALSE(world.GetRegistry().valid(created[1]));
    EXPECT_TRUE(Spark::SerializeWorld(world) == before);

    // Redo recreates the same identifiers, so the document matches byte for byte.
    ASSERT_TRUE(Spark::Editor::CommandHistory::GetInstance().Redo());
    EXPECT_TRUE(world.GetRegistry().valid(created[0]));
    EXPECT_TRUE(world.GetRegistry().valid(created[1]));
    EXPECT_TRUE(Spark::SerializeWorld(world) == after);

    // Nothing to import records nothing.
    EXPECT_TRUE(SparkEditor::SceneEditTools::CommitSceneImport(world, {}, "Import nothing").empty());
    EXPECT_EQ(Spark::Editor::CommandHistory::GetInstance().UndoCount(), static_cast<size_t>(1));

    Spark::Editor::CommandHistory::GetInstance().Clear();
}
