/**
 * @file TestEditorGizmoTransformReal.cpp
 * @brief Real-class tests for the viewport rotate/scale gizmo commit path.
 *
 * The gizmos themselves are ImGui-driven, but every drag ends in the undoable
 * commit helpers below (SceneEditTools::CommitEntityRotation / CommitEntityScale),
 * which are the part that has to be correct: one history entry per drag, and an
 * undo that puts the transform back exactly where the drag started.
 */

#include "TestFramework.h"

#include "CommandHistory.h"
#include "Gizmos/SceneEditTools.h"

namespace
{
    ::EntityID MakeTransformEntity(::World& world, const char* name)
    {
        const ::EntityID entity = world.CreateEntity(name);
        world.AddComponent<::Transform>(entity);
        return entity;
    }

    ::Transform& TransformOf(::World& world, ::EntityID entity)
    {
        return *world.GetRegistry().try_get<::Transform>(entity);
    }
} // namespace

TEST(EditorGizmo_RotateCommitIsOneUndoableStep)
{
    Spark::Editor::CommandHistory::GetInstance().Clear();

    ::World world;
    const ::EntityID entity = MakeTransformEntity(world, "Rotatable");
    const DirectX::XMFLOAT3 start{0.0f, 0.0f, 0.0f};
    const DirectX::XMFLOAT3 dragged{0.0f, 90.0f, 0.0f};

    // The gizmo previews live during the drag, then commits both endpoints.
    TransformOf(world, entity).rotation = dragged;
    EXPECT_TRUE(SparkEditor::SceneEditTools::CommitEntityRotation(world, entity, start, dragged));

    EXPECT_EQ(Spark::Editor::CommandHistory::GetInstance().UndoCount(), static_cast<size_t>(1));
    EXPECT_STR_CONTAINS(Spark::Editor::CommandHistory::GetInstance().GetUndoDescription(), "Rotate");
    EXPECT_NEAR(TransformOf(world, entity).rotation.y, 90.0f, 1e-4f);

    EXPECT_TRUE(Spark::Editor::CommandHistory::GetInstance().Undo());
    EXPECT_NEAR(TransformOf(world, entity).rotation.y, 0.0f, 1e-4f);

    EXPECT_TRUE(Spark::Editor::CommandHistory::GetInstance().Redo());
    EXPECT_NEAR(TransformOf(world, entity).rotation.y, 90.0f, 1e-4f);

    Spark::Editor::CommandHistory::GetInstance().Clear();
}

TEST(EditorGizmo_RotateCommitPreservesUntouchedAxes)
{
    Spark::Editor::CommandHistory::GetInstance().Clear();

    ::World world;
    const ::EntityID entity = MakeTransformEntity(world, "Compound");
    const DirectX::XMFLOAT3 start{15.0f, -40.0f, 5.0f};
    TransformOf(world, entity).rotation = start;

    const DirectX::XMFLOAT3 dragged{15.0f, -25.0f, 5.0f}; // only the yaw ring was dragged
    TransformOf(world, entity).rotation = dragged;
    EXPECT_TRUE(SparkEditor::SceneEditTools::CommitEntityRotation(world, entity, start, dragged));

    EXPECT_TRUE(Spark::Editor::CommandHistory::GetInstance().Undo());
    const ::Transform& restored = TransformOf(world, entity);
    EXPECT_NEAR(restored.rotation.x, 15.0f, 1e-4f);
    EXPECT_NEAR(restored.rotation.y, -40.0f, 1e-4f);
    EXPECT_NEAR(restored.rotation.z, 5.0f, 1e-4f);

    Spark::Editor::CommandHistory::GetInstance().Clear();
}

TEST(EditorGizmo_ScaleCommitRoundTrips)
{
    Spark::Editor::CommandHistory::GetInstance().Clear();

    ::World world;
    const ::EntityID entity = MakeTransformEntity(world, "Scalable");
    const DirectX::XMFLOAT3 start{1.0f, 1.0f, 1.0f};
    const DirectX::XMFLOAT3 dragged{1.0f, 2.5f, 1.0f};

    TransformOf(world, entity).scale = dragged;
    EXPECT_TRUE(SparkEditor::SceneEditTools::CommitEntityScale(world, entity, start, dragged));

    EXPECT_EQ(Spark::Editor::CommandHistory::GetInstance().UndoCount(), static_cast<size_t>(1));
    EXPECT_STR_CONTAINS(Spark::Editor::CommandHistory::GetInstance().GetUndoDescription(), "Scale");

    EXPECT_TRUE(Spark::Editor::CommandHistory::GetInstance().Undo());
    EXPECT_NEAR(TransformOf(world, entity).scale.y, 1.0f, 1e-4f);

    EXPECT_TRUE(Spark::Editor::CommandHistory::GetInstance().Redo());
    EXPECT_NEAR(TransformOf(world, entity).scale.y, 2.5f, 1e-4f);

    Spark::Editor::CommandHistory::GetInstance().Clear();
}

TEST(EditorGizmo_ZeroLengthDragRecordsNothing)
{
    Spark::Editor::CommandHistory::GetInstance().Clear();

    ::World world;
    const ::EntityID entity = MakeTransformEntity(world, "Untouched");
    const DirectX::XMFLOAT3 same{1.0f, 1.0f, 1.0f};
    const DirectX::XMFLOAT3 zero{0.0f, 0.0f, 0.0f};

    // A click that does not move the mouse must not push an undo step.
    EXPECT_FALSE(SparkEditor::SceneEditTools::CommitEntityScale(world, entity, same, same));
    EXPECT_FALSE(SparkEditor::SceneEditTools::CommitEntityRotation(world, entity, zero, zero));
    EXPECT_EQ(Spark::Editor::CommandHistory::GetInstance().UndoCount(), static_cast<size_t>(0));

    Spark::Editor::CommandHistory::GetInstance().Clear();
}

TEST(EditorGizmo_CommitRefusesEntityWithoutTransform)
{
    Spark::Editor::CommandHistory::GetInstance().Clear();

    ::World world;
    const ::EntityID entity = world.CreateEntity("NoTransform");
    const DirectX::XMFLOAT3 zero{0.0f, 0.0f, 0.0f};
    const DirectX::XMFLOAT3 turned{0.0f, 45.0f, 0.0f};

    EXPECT_FALSE(SparkEditor::SceneEditTools::CommitEntityRotation(world, entity, zero, turned));
    EXPECT_FALSE(SparkEditor::SceneEditTools::CommitEntityScale(world, entity, zero, turned));
    EXPECT_EQ(Spark::Editor::CommandHistory::GetInstance().UndoCount(), static_cast<size_t>(0));

    Spark::Editor::CommandHistory::GetInstance().Clear();
}

TEST(EditorGizmo_CommitSurvivesEntityDestroyedBeforeUndo)
{
    Spark::Editor::CommandHistory::GetInstance().Clear();

    ::World world;
    const ::EntityID entity = MakeTransformEntity(world, "Doomed");
    const DirectX::XMFLOAT3 start{1.0f, 1.0f, 1.0f};
    const DirectX::XMFLOAT3 dragged{3.0f, 1.0f, 1.0f};

    TransformOf(world, entity).scale = dragged;
    EXPECT_TRUE(SparkEditor::SceneEditTools::CommitEntityScale(world, entity, start, dragged));

    // Commands capture entity ids, never component pointers, so undoing after the
    // entity is gone must be a safe no-op rather than a use-after-free.
    world.DestroyEntity(entity);
    EXPECT_TRUE(Spark::Editor::CommandHistory::GetInstance().Undo());
    EXPECT_FALSE(world.GetRegistry().valid(entity));

    Spark::Editor::CommandHistory::GetInstance().Clear();
}
