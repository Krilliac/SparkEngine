/**
 * @file TestEDT210InspectorEditCommitReal.cpp
 * @brief EDT-210: World-backed Inspector edits must always reach CommandHistory.
 *
 * The World-backed Inspector writes reflected fields straight into live
 * component memory and records the gesture as one CommandHistory entry after
 * the fact, from a pending pre-edit snapshot. The production commit policy
 * (SparkEditor::InspectorPendingWorldEdit, driven by InspectorPanel) decides
 * when that gesture ends. Before EDT-210 the policy:
 *   - DROPPED the pending baseline when the selection moved to another entity,
 *     leaving an applied edit with no undo entry;
 *   - never settled while the World-backed path was not rendered (selection
 *     cleared, panel hidden), then replayed a stale baseline later;
 *   - treated "some other widget is active" as "the edit is still going";
 *   - committed a stale baseline after another command ran, so undo reverted
 *     that other command too.
 *
 * These tests drive the real policy class against a real ::World, the real
 * reflected scene serializer, and the real CommandHistory. The commit and
 * restore callbacks mirror EditorUI::RecordAppliedDocumentMutation and
 * EditorUI::RestoreWorldSnapshot (both private to EditorUI).
 */

#include "TestFramework.h"

#include "CommandHistory.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/CoreComponents.h"
#include "Panels/InspectorPendingWorldEdit.h"
#include "SceneManager/ReflectedSceneSerializer.h"

#include <algorithm>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    using Outcome = SparkEditor::InspectorPendingWorldEdit::Outcome;

    constexpr uint32_t kFieldItem = 0x1001u; // ImGui id of the edited DragFloat
    constexpr uint32_t kOtherItem = 0x2002u; // some other widget (hierarchy row, gizmo)

    /// Mirrors the EditorUI snapshot-command pair used by the World-backed Inspector.
    struct EditorDocument
    {
        ::World world;

        std::string Snapshot() const { return Spark::SerializeWorld(world); }

        /// Order-independent document state. A snapshot restore rebuilds the
        /// registry, and SerializeWorld then lists entities in the opposite
        /// order, so undo exactness is compared per entity id, not per byte.
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
                {
                    out << " T(" << t->position.x << ',' << t->position.y << ',' << t->position.z << ' '
                        << t->rotation.x << ',' << t->rotation.y << ',' << t->rotation.z << ' ' << t->scale.x << ','
                        << t->scale.y << ',' << t->scale.z << ')';
                }
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
            Spark::Editor::CommandHistory::GetInstance().Execute(std::make_unique<Spark::Editor::LambdaCommand>(
                [this, after]() { Restore(after); }, [this, before]() { Restore(before); }, description));
            return true;
        }

        SparkEditor::InspectorPendingWorldEdit::CommitFn Commit()
        {
            return [this](const std::string& before, const std::string& description)
            { return RecordApplied(before, description); };
        }
    };

    uint64_t Sequence()
    {
        return Spark::Editor::CommandHistory::GetInstance().GetEditSequence();
    }

    size_t UndoCount()
    {
        return Spark::Editor::CommandHistory::GetInstance().UndoCount();
    }

    float PositionX(::World& world, ::EntityID entity)
    {
        const ::Transform* t = world.GetRegistry().try_get<::Transform>(entity);
        return t ? t->position.x : -12345.0f;
    }

    /// One Inspector frame editing entity's Transform.position.x, exactly as
    /// InspectorPanel::RenderWorldBackedInspector does: snapshot, mutate live
    /// memory, note the change with the item that is active afterwards.
    void EditFrame(EditorDocument& doc, SparkEditor::InspectorPendingWorldEdit& pending, ::EntityID entity, float newX,
                   uint32_t activeItemAfter)
    {
        const std::string before = doc.Snapshot();
        doc.world.GetRegistry().get<::Transform>(entity).position.x = newX;
        pending.NoteChange(&doc.world, entity, before, "Edit Transform", activeItemAfter, Sequence());
    }

    struct TwoEntityScene
    {
        EditorDocument doc;
        ::EntityID a = entt::null;
        ::EntityID b = entt::null;
        std::string initial;

        TwoEntityScene()
        {
            Spark::Editor::CommandHistory::GetInstance().Clear();
            a = doc.world.CreateEntity("Alpha");
            doc.world.AddComponent<::Transform>(a).position = {1.0f, 2.0f, 3.0f};
            b = doc.world.CreateEntity("Bravo");
            doc.world.AddComponent<::Transform>(b).position = {7.0f, 8.0f, 9.0f};
            initial = doc.State();
            Spark::Editor::CommandHistory::GetInstance().MarkSaved();
        }

        ~TwoEntityScene() { Spark::Editor::CommandHistory::GetInstance().Clear(); }
    };
} // namespace

TEST(EditorUndo_EDT210_DragReleaseRecordsOneExactUndoStep)
{
    TwoEntityScene scene;
    SparkEditor::InspectorPendingWorldEdit pending;
    auto commit = scene.doc.Commit();

    // Three frames of one drag: the field stays active, nothing recorded yet.
    for (float x : {1.5f, 2.5f, 4.0f})
    {
        EditFrame(scene.doc, pending, scene.a, x, kFieldItem);
        EXPECT_EQ(static_cast<int>(pending.Settle(&scene.doc.world, scene.a, kFieldItem, Sequence(), commit)),
                  static_cast<int>(Outcome::Pending));
    }
    EXPECT_EQ(UndoCount(), static_cast<size_t>(0));

    // Mouse released: no active item -> exactly one history entry.
    EXPECT_EQ(static_cast<int>(pending.Settle(&scene.doc.world, scene.a, 0u, Sequence(), commit)),
              static_cast<int>(Outcome::Committed));
    EXPECT_EQ(UndoCount(), static_cast<size_t>(1));
    EXPECT_TRUE(Spark::Editor::CommandHistory::GetInstance().IsModified());
    const std::string edited = scene.doc.State();

    EXPECT_TRUE(Spark::Editor::CommandHistory::GetInstance().Undo());
    EXPECT_EQ(scene.doc.State(), scene.initial);

    EXPECT_TRUE(Spark::Editor::CommandHistory::GetInstance().Redo());
    EXPECT_EQ(scene.doc.State(), edited);
    EXPECT_NEAR(PositionX(scene.doc.world, scene.a), 4.0f, 1e-5f);
}

TEST(EditorUndo_EDT210_SelectingAnotherEntityCommitsInsteadOfDroppingTheEdit)
{
    TwoEntityScene scene;
    SparkEditor::InspectorPendingWorldEdit pending;
    auto commit = scene.doc.Commit();

    // Type a value into A's field; the text field is still active.
    EditFrame(scene.doc, pending, scene.a, 42.0f, kFieldItem);
    EXPECT_EQ(static_cast<int>(pending.Settle(&scene.doc.world, scene.a, kFieldItem, Sequence(), commit)),
              static_cast<int>(Outcome::Pending));

    // Next frame the hierarchy click has already moved selection to B.
    EXPECT_EQ(static_cast<int>(pending.Settle(&scene.doc.world, scene.b, kOtherItem, Sequence(), commit)),
              static_cast<int>(Outcome::Committed));
    EXPECT_FALSE(pending.HasPending());
    EXPECT_EQ(UndoCount(), static_cast<size_t>(1));
    EXPECT_TRUE(Spark::Editor::CommandHistory::GetInstance().IsModified());

    EXPECT_TRUE(Spark::Editor::CommandHistory::GetInstance().Undo());
    EXPECT_EQ(scene.doc.State(), scene.initial);
    EXPECT_NEAR(PositionX(scene.doc.world, scene.a), 1.0f, 1e-5f);
}

TEST(EditorUndo_EDT210_ClearedSelectionOrHiddenPanelStillCommits)
{
    TwoEntityScene scene;
    SparkEditor::InspectorPendingWorldEdit pending;
    auto commit = scene.doc.Commit();

    EditFrame(scene.doc, pending, scene.a, -3.0f, kFieldItem);

    // Selection cleared / Inspector hidden: the World-backed path renders no entity.
    EXPECT_EQ(static_cast<int>(pending.Settle(&scene.doc.world, entt::null, 0u, Sequence(), commit)),
              static_cast<int>(Outcome::Committed));
    EXPECT_EQ(UndoCount(), static_cast<size_t>(1));

    EXPECT_TRUE(Spark::Editor::CommandHistory::GetInstance().Undo());
    EXPECT_EQ(scene.doc.State(), scene.initial);
}

TEST(EditorUndo_EDT210_ActivatingAnotherWidgetEndsTheGesture)
{
    TwoEntityScene scene;
    SparkEditor::InspectorPendingWorldEdit pending;
    auto commit = scene.doc.Commit();

    EditFrame(scene.doc, pending, scene.a, 11.0f, kFieldItem);

    // Same entity still inspected, but a different widget (e.g. a gizmo
    // handle or another row) grabbed the active id: the field edit is over and
    // must be recorded before that widget can execute its own command.
    EXPECT_EQ(static_cast<int>(pending.Settle(&scene.doc.world, scene.a, kOtherItem, Sequence(), commit)),
              static_cast<int>(Outcome::Committed));
    EXPECT_EQ(UndoCount(), static_cast<size_t>(1));
    EXPECT_STR_CONTAINS(Spark::Editor::CommandHistory::GetInstance().GetUndoDescription(), "Edit Transform");
}

TEST(EditorUndo_EDT210_StaleBaselineIsDiscardedNotReplayedOverAnotherCommand)
{
    TwoEntityScene scene;
    SparkEditor::InspectorPendingWorldEdit pending;
    auto commit = scene.doc.Commit();

    EditFrame(scene.doc, pending, scene.a, 5.0f, kFieldItem);

    // While the baseline is pending, another command lands (e.g. Ctrl+Z
    // during a drag, or a snapshot command from another panel).
    const std::string beforeOther = scene.doc.Snapshot();
    const std::string stateBeforeOther = scene.doc.State();
    scene.doc.world.GetRegistry().get<::Transform>(scene.b).position.x = 70.0f;
    EXPECT_TRUE(scene.doc.RecordApplied(beforeOther, "Move Bravo"));
    EXPECT_EQ(UndoCount(), static_cast<size_t>(1));

    // Replaying the old baseline would make undo revert "Move Bravo" as well.
    EXPECT_EQ(static_cast<int>(pending.Settle(&scene.doc.world, scene.a, 0u, Sequence(), commit)),
              static_cast<int>(Outcome::Discarded));
    EXPECT_EQ(UndoCount(), static_cast<size_t>(1));

    EXPECT_TRUE(Spark::Editor::CommandHistory::GetInstance().Undo());
    EXPECT_EQ(scene.doc.State(), stateBeforeOther);
}

TEST(EditorUndo_EDT210_ReplacedDocumentDiscardsForeignBaseline)
{
    TwoEntityScene scene;
    SparkEditor::InspectorPendingWorldEdit pending;
    auto commit = scene.doc.Commit();

    EditFrame(scene.doc, pending, scene.a, 9.0f, kFieldItem);

    // File -> Open swapped in another World (history cleared, sequence kept).
    EditorDocument opened;
    opened.world.CreateEntity("Opened");
    Spark::Editor::CommandHistory::GetInstance().Clear();

    EXPECT_EQ(static_cast<int>(pending.Settle(&opened.world, entt::null, 0u, Sequence(), opened.Commit())),
              static_cast<int>(Outcome::Discarded));
    EXPECT_EQ(UndoCount(), static_cast<size_t>(0));
}

TEST(EditorUndo_EDT210_FlushOrdersFieldEditBeforeImmediateComponentCommand)
{
    TwoEntityScene scene;
    SparkEditor::InspectorPendingWorldEdit pending;
    auto commit = scene.doc.Commit();

    EditFrame(scene.doc, pending, scene.a, 21.0f, kFieldItem);
    const std::string afterField = scene.doc.State();

    // The Inspector's "Remove"/"Add Component" buttons record immediately;
    // the open field gesture must be flushed ahead of them.
    EXPECT_EQ(static_cast<int>(pending.Flush(&scene.doc.world, Sequence(), commit)),
              static_cast<int>(Outcome::Committed));
    const std::string beforeRemove = scene.doc.Snapshot();
    scene.doc.world.GetRegistry().remove<::Transform>(scene.b);
    EXPECT_TRUE(scene.doc.RecordApplied(beforeRemove, "Remove Transform"));
    EXPECT_EQ(UndoCount(), static_cast<size_t>(2));

    EXPECT_TRUE(Spark::Editor::CommandHistory::GetInstance().Undo());
    EXPECT_EQ(scene.doc.State(), afterField);
    EXPECT_TRUE(Spark::Editor::CommandHistory::GetInstance().Undo());
    EXPECT_EQ(scene.doc.State(), scene.initial);
}

TEST(EditorUndo_EDT210_NoOpGestureRecordsNothing)
{
    TwoEntityScene scene;
    SparkEditor::InspectorPendingWorldEdit pending;
    auto commit = scene.doc.Commit();

    // Drag away and back to the original value before release.
    EditFrame(scene.doc, pending, scene.a, 6.0f, kFieldItem);
    EditFrame(scene.doc, pending, scene.a, 1.0f, kFieldItem);
    pending.Settle(&scene.doc.world, scene.a, 0u, Sequence(), commit);

    EXPECT_FALSE(pending.HasPending());
    EXPECT_EQ(UndoCount(), static_cast<size_t>(0));
    EXPECT_FALSE(Spark::Editor::CommandHistory::GetInstance().IsModified());
}
