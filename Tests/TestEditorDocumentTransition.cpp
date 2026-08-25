// TestEditorDocumentTransition.cpp - unsaved-document transition state machine

#include "TestFramework.h"
#include "Core/EditorUI.h"

using SparkEditor::DocumentTransitionAction;
using SparkEditor::DocumentTransitionGuard;
using SparkEditor::UnsavedChangesDecision;

TEST(DocumentTransition_CleanDocumentExecutesImmediately)
{
    DocumentTransitionGuard guard;

    EXPECT_TRUE(guard.Request(DocumentTransitionAction::NewScene, false));
    EXPECT_FALSE(guard.HasPending());
}

TEST(DocumentTransition_DirtyDocumentWaitsForDecision)
{
    DocumentTransitionGuard guard;

    EXPECT_FALSE(guard.Request(DocumentTransitionAction::OpenSceneDialog, true));
    EXPECT_TRUE(guard.HasPending());
    EXPECT_TRUE(guard.GetPending() == DocumentTransitionAction::OpenSceneDialog);
}

TEST(DocumentTransition_CancelPreservesDocumentAndClearsRequest)
{
    DocumentTransitionGuard guard;
    guard.Request(DocumentTransitionAction::Exit, true);

    const auto ready = guard.Resolve(UnsavedChangesDecision::Cancel);

    EXPECT_TRUE(ready == DocumentTransitionAction::None);
    EXPECT_FALSE(guard.HasPending());
}

TEST(DocumentTransition_FailedSaveKeepsPendingAction)
{
    DocumentTransitionGuard guard;
    guard.Request(DocumentTransitionAction::NewScene, true);

    const auto ready = guard.Resolve(UnsavedChangesDecision::Save, false);

    EXPECT_TRUE(ready == DocumentTransitionAction::None);
    EXPECT_TRUE(guard.HasPending());
    EXPECT_TRUE(guard.GetPending() == DocumentTransitionAction::NewScene);
}

TEST(DocumentTransition_SaveOrDiscardReleasesExactlyOneAction)
{
    DocumentTransitionGuard saveGuard;
    saveGuard.Request(DocumentTransitionAction::OpenSceneDialog, true);
    EXPECT_TRUE(saveGuard.Resolve(UnsavedChangesDecision::Save, true) == DocumentTransitionAction::OpenSceneDialog);
    EXPECT_FALSE(saveGuard.HasPending());

    DocumentTransitionGuard discardGuard;
    discardGuard.Request(DocumentTransitionAction::Exit, true);
    EXPECT_TRUE(discardGuard.Resolve(UnsavedChangesDecision::Discard) == DocumentTransitionAction::Exit);
    EXPECT_FALSE(discardGuard.HasPending());
}

TEST(DocumentTransition_SecondRequestCannotReplacePendingAction)
{
    DocumentTransitionGuard guard;
    guard.Request(DocumentTransitionAction::NewScene, true);

    EXPECT_FALSE(guard.Request(DocumentTransitionAction::Exit, false));
    EXPECT_TRUE(guard.GetPending() == DocumentTransitionAction::NewScene);
}

TEST(DocumentTransition_ProjectOpenAndCreationUseTheDirtyDocumentGate)
{
    DocumentTransitionGuard openGuard;
    EXPECT_FALSE(openGuard.Request(DocumentTransitionAction::OpenProject, true));
    EXPECT_TRUE(openGuard.GetPending() == DocumentTransitionAction::OpenProject);
    EXPECT_TRUE(openGuard.Resolve(UnsavedChangesDecision::Discard) == DocumentTransitionAction::OpenProject);

    DocumentTransitionGuard createGuard;
    EXPECT_FALSE(createGuard.Request(DocumentTransitionAction::CreateProject, true));
    EXPECT_TRUE(createGuard.GetPending() == DocumentTransitionAction::CreateProject);
    EXPECT_TRUE(createGuard.Resolve(UnsavedChangesDecision::Save, true) == DocumentTransitionAction::CreateProject);
}
