// TestTutorialSystem.cpp - Tests for SparkEditor::TutorialSystem
#include "TestFramework.h"
#include "Core/TutorialSystem.h"

#include <string>

// ============================================================================
// Initialize registers default tutorials
// ============================================================================

TEST(TutorialSystem_Initialize_RegistersDefaults)
{
    auto& sys = SparkEditor::TutorialSystem::GetInstance();
    sys.Initialize();

    auto names = sys.GetAvailableTutorials();
    EXPECT_GE(names.size(), 3u); // Getting Started, Placing Entities, Material Setup

    sys.Shutdown();
}

// ============================================================================
// GetAvailableTutorials
// ============================================================================

TEST(TutorialSystem_GetAvailableTutorials)
{
    auto& sys = SparkEditor::TutorialSystem::GetInstance();
    sys.Initialize();

    auto names = sys.GetAvailableTutorials();
    bool foundGettingStarted = false;
    for (const auto& n : names)
    {
        if (n == "Getting Started")
        {
            foundGettingStarted = true;
        }
    }
    EXPECT_TRUE(foundGettingStarted);

    sys.Shutdown();
}

TEST(TutorialSystem_GetAvailableTutorials_ContainsAll)
{
    auto& sys = SparkEditor::TutorialSystem::GetInstance();
    sys.Initialize();

    auto names = sys.GetAvailableTutorials();
    bool foundPlacing = false;
    bool foundMaterial = false;
    for (const auto& n : names)
    {
        if (n == "Placing Entities")
        {
            foundPlacing = true;
        }
        if (n == "Material Setup")
        {
            foundMaterial = true;
        }
    }
    EXPECT_TRUE(foundPlacing);
    EXPECT_TRUE(foundMaterial);

    sys.Shutdown();
}

// ============================================================================
// StartTutorial and IsTutorialActive
// ============================================================================

TEST(TutorialSystem_StartTutorial)
{
    auto& sys = SparkEditor::TutorialSystem::GetInstance();
    sys.Initialize();

    bool started = sys.StartTutorial("Getting Started");
    EXPECT_TRUE(started);
    EXPECT_TRUE(sys.IsTutorialActive());
    EXPECT_TRUE(sys.GetCurrentTutorialName() == "Getting Started");

    sys.Shutdown();
}

TEST(TutorialSystem_StartTutorial_NonExistent)
{
    auto& sys = SparkEditor::TutorialSystem::GetInstance();
    sys.Initialize();

    bool started = sys.StartTutorial("NonExistent Tutorial");
    EXPECT_FALSE(started);
    EXPECT_FALSE(sys.IsTutorialActive());

    sys.Shutdown();
}

TEST(TutorialSystem_IsTutorialActive_FalseInitially)
{
    auto& sys = SparkEditor::TutorialSystem::GetInstance();
    sys.Initialize();

    EXPECT_FALSE(sys.IsTutorialActive());

    sys.Shutdown();
}

// ============================================================================
// AdvanceStep and GetCurrentStep / GetCurrentStepIndex
// ============================================================================

TEST(TutorialSystem_GetCurrentStep)
{
    auto& sys = SparkEditor::TutorialSystem::GetInstance();
    sys.Initialize();
    sys.StartTutorial("Getting Started");

    const auto* step = sys.GetCurrentStep();
    EXPECT_TRUE(step != nullptr);
    EXPECT_EQ(sys.GetCurrentStepIndex(), 0u);

    sys.Shutdown();
}

TEST(TutorialSystem_AdvanceStep)
{
    auto& sys = SparkEditor::TutorialSystem::GetInstance();
    sys.Initialize();
    sys.StartTutorial("Getting Started");

    EXPECT_EQ(sys.GetCurrentStepIndex(), 0u);
    sys.AdvanceStep();
    EXPECT_EQ(sys.GetCurrentStepIndex(), 1u);

    const auto* step = sys.GetCurrentStep();
    EXPECT_TRUE(step != nullptr);

    sys.Shutdown();
}

TEST(TutorialSystem_AdvanceStep_TotalSteps)
{
    auto& sys = SparkEditor::TutorialSystem::GetInstance();
    sys.Initialize();
    sys.StartTutorial("Getting Started");

    uint32_t total = sys.GetTotalSteps();
    EXPECT_GT(total, 0u);

    // Advance through all steps
    for (uint32_t i = 0; i < total; ++i)
    {
        sys.AdvanceStep();
    }
    // After advancing past the last step, the tutorial should be stopped
    EXPECT_FALSE(sys.IsTutorialActive());

    sys.Shutdown();
}

TEST(TutorialSystem_GetCurrentStep_Null_WhenInactive)
{
    auto& sys = SparkEditor::TutorialSystem::GetInstance();
    sys.Initialize();

    EXPECT_TRUE(sys.GetCurrentStep() == nullptr);

    sys.Shutdown();
}

// ============================================================================
// StopTutorial
// ============================================================================

TEST(TutorialSystem_StopTutorial)
{
    auto& sys = SparkEditor::TutorialSystem::GetInstance();
    sys.Initialize();
    sys.StartTutorial("Getting Started");

    EXPECT_TRUE(sys.IsTutorialActive());
    sys.StopTutorial();
    EXPECT_FALSE(sys.IsTutorialActive());
    EXPECT_EQ(sys.GetCurrentStepIndex(), 0u);

    sys.Shutdown();
}

// ============================================================================
// MarkCompleted and IsTutorialCompleted
// ============================================================================

TEST(TutorialSystem_MarkCompleted)
{
    auto& sys = SparkEditor::TutorialSystem::GetInstance();
    sys.Initialize();

    EXPECT_FALSE(sys.IsTutorialCompleted("Getting Started"));
    sys.MarkCompleted("Getting Started");
    EXPECT_TRUE(sys.IsTutorialCompleted("Getting Started"));

    sys.Shutdown();
}

TEST(TutorialSystem_CompletedByAdvancing)
{
    auto& sys = SparkEditor::TutorialSystem::GetInstance();
    sys.Initialize();
    sys.StartTutorial("Getting Started");

    uint32_t total = sys.GetTotalSteps();
    for (uint32_t i = 0; i < total; ++i)
    {
        sys.AdvanceStep();
    }

    // Advancing past the end marks the tutorial completed
    EXPECT_TRUE(sys.IsTutorialCompleted("Getting Started"));

    sys.Shutdown();
}

TEST(TutorialSystem_IsTutorialCompleted_False_Initially)
{
    auto& sys = SparkEditor::TutorialSystem::GetInstance();
    sys.Initialize();

    EXPECT_FALSE(sys.IsTutorialCompleted("Getting Started"));
    EXPECT_FALSE(sys.IsTutorialCompleted("Placing Entities"));

    sys.Shutdown();
}

// ============================================================================
// Custom tutorial registration
// ============================================================================

TEST(TutorialSystem_RegisterCustomTutorial)
{
    auto& sys = SparkEditor::TutorialSystem::GetInstance();
    sys.Initialize();

    SparkEditor::TutorialSequence seq;
    seq.name = "Custom Test";
    seq.description = "A test tutorial";
    seq.steps.push_back(
        {SparkEditor::TutorialStepType::ShowMessage, {}, "Step 1", SparkEditor::TooltipPosition::Bottom, {}, 0.f});
    seq.steps.push_back(
        {SparkEditor::TutorialStepType::ShowMessage, {}, "Step 2", SparkEditor::TooltipPosition::Bottom, {}, 0.f});
    sys.RegisterTutorial(std::move(seq));

    bool started = sys.StartTutorial("Custom Test");
    EXPECT_TRUE(started);
    EXPECT_EQ(sys.GetTotalSteps(), 2u);

    sys.Shutdown();
}

// ============================================================================
// Additional coverage for update loop, callbacks, goto, status
// ============================================================================

TEST(TutorialSystem_GoToStep_JumpsWithinSequence)
{
    auto& sys = SparkEditor::TutorialSystem::GetInstance();
    sys.Initialize();

    SparkEditor::TutorialSequence seq;
    seq.name = "JumpTest";
    seq.description = "goto test";
    seq.steps.push_back(
        {SparkEditor::TutorialStepType::ShowMessage, {}, "A", SparkEditor::TooltipPosition::Bottom, {}, 0.f});
    seq.steps.push_back(
        {SparkEditor::TutorialStepType::ShowMessage, {}, "B", SparkEditor::TooltipPosition::Bottom, {}, 0.f});
    seq.steps.push_back(
        {SparkEditor::TutorialStepType::ShowMessage, {}, "C", SparkEditor::TooltipPosition::Bottom, {}, 0.f});
    sys.RegisterTutorial(std::move(seq));
    sys.StartTutorial("JumpTest");

    sys.GoToStep(2);
    EXPECT_EQ(sys.GetCurrentStepIndex(), 2u);
    EXPECT_EQ(sys.GetCurrentStep()->message, std::string("C"));

    // Out-of-range goto is ignored.
    sys.GoToStep(99);
    EXPECT_EQ(sys.GetCurrentStepIndex(), 2u);

    sys.Shutdown();
}

TEST(TutorialSystem_Update_AutoAdvancesAfterDelay)
{
    auto& sys = SparkEditor::TutorialSystem::GetInstance();
    sys.Initialize();

    SparkEditor::TutorialSequence seq;
    seq.name = "AutoAdvance";
    seq.description = "auto-advance test";
    seq.steps.push_back(
        {SparkEditor::TutorialStepType::ShowMessage, {}, "First", SparkEditor::TooltipPosition::Bottom, {}, 1.0f});
    seq.steps.push_back(
        {SparkEditor::TutorialStepType::ShowMessage, {}, "Second", SparkEditor::TooltipPosition::Bottom, {}, 0.0f});
    sys.RegisterTutorial(std::move(seq));
    sys.StartTutorial("AutoAdvance");
    EXPECT_EQ(sys.GetCurrentStepIndex(), 0u);

    // Half a second: no advance yet.
    sys.Update(0.5f);
    EXPECT_EQ(sys.GetCurrentStepIndex(), 0u);

    // Another 0.6s crosses the 1.0s threshold.
    sys.Update(0.6f);
    EXPECT_EQ(sys.GetCurrentStepIndex(), 1u);

    // Step 2 has autoAdvanceDelay=0 — Update should be a no-op.
    sys.Update(100.0f);
    EXPECT_EQ(sys.GetCurrentStepIndex(), 1u);

    sys.Shutdown();
}

TEST(TutorialSystem_Update_InactiveIsNoOp)
{
    auto& sys = SparkEditor::TutorialSystem::GetInstance();
    sys.Initialize();
    EXPECT_FALSE(sys.IsTutorialActive());
    sys.Update(10.0f); // must not crash, must not change anything
    EXPECT_FALSE(sys.IsTutorialActive());
    sys.Shutdown();
}

TEST(TutorialSystem_OnStepChanged_CallbackFires)
{
    auto& sys = SparkEditor::TutorialSystem::GetInstance();
    sys.Initialize();

    SparkEditor::TutorialSequence seq;
    seq.name = "CallbackTest";
    seq.description = "callback";
    seq.steps.push_back(
        {SparkEditor::TutorialStepType::ShowMessage, {}, "One", SparkEditor::TooltipPosition::Bottom, {}, 0.f});
    seq.steps.push_back(
        {SparkEditor::TutorialStepType::ShowMessage, {}, "Two", SparkEditor::TooltipPosition::Bottom, {}, 0.f});
    sys.RegisterTutorial(std::move(seq));

    int callCount = 0;
    uint32_t lastIndex = 999;
    std::string lastMessage;
    sys.OnStepChanged(
        [&](uint32_t idx, const SparkEditor::TutorialStep& step)
        {
            ++callCount;
            lastIndex = idx;
            lastMessage = step.message;
        });

    sys.StartTutorial("CallbackTest");
    EXPECT_EQ(callCount, 1);
    EXPECT_EQ(lastIndex, 0u);
    EXPECT_EQ(lastMessage, std::string("One"));

    sys.AdvanceStep();
    EXPECT_EQ(callCount, 2);
    EXPECT_EQ(lastIndex, 1u);
    EXPECT_EQ(lastMessage, std::string("Two"));

    sys.Shutdown();
}

TEST(TutorialSystem_GetCurrentTutorialName)
{
    auto& sys = SparkEditor::TutorialSystem::GetInstance();
    sys.Initialize();

    // No active tutorial → empty name.
    EXPECT_TRUE(sys.GetCurrentTutorialName().empty());

    SparkEditor::TutorialSequence seq;
    seq.name = "NameTest";
    seq.description = "name";
    seq.steps.push_back(
        {SparkEditor::TutorialStepType::ShowMessage, {}, "M", SparkEditor::TooltipPosition::Bottom, {}, 0.f});
    sys.RegisterTutorial(std::move(seq));
    sys.StartTutorial("NameTest");
    EXPECT_EQ(std::string(sys.GetCurrentTutorialName()), std::string("NameTest"));

    sys.StopTutorial();
    EXPECT_TRUE(sys.GetCurrentTutorialName().empty());

    sys.Shutdown();
}

TEST(TutorialSystem_ConsoleStatusContainsActiveName)
{
    auto& sys = SparkEditor::TutorialSystem::GetInstance();
    sys.Initialize();

    const std::string inactive = sys.Console_GetStatus();
    EXPECT_TRUE(inactive.find("[TutorialSystem]") != std::string::npos);
    EXPECT_TRUE(inactive.find("active=none") != std::string::npos);

    SparkEditor::TutorialSequence seq;
    seq.name = "StatusTest";
    seq.description = "status";
    seq.steps.push_back(
        {SparkEditor::TutorialStepType::ShowMessage, {}, "X", SparkEditor::TooltipPosition::Bottom, {}, 0.f});
    sys.RegisterTutorial(std::move(seq));
    sys.StartTutorial("StatusTest");

    const std::string active = sys.Console_GetStatus();
    EXPECT_TRUE(active.find("StatusTest") != std::string::npos);
    EXPECT_TRUE(active.find("step=1/1") != std::string::npos);

    sys.Shutdown();
}

TEST(TutorialSystem_StartEmptyTutorialReturnsFalse)
{
    auto& sys = SparkEditor::TutorialSystem::GetInstance();
    sys.Initialize();

    SparkEditor::TutorialSequence seq;
    seq.name = "Empty";
    seq.description = "no steps";
    // Deliberately no steps.
    sys.RegisterTutorial(std::move(seq));

    EXPECT_FALSE(sys.StartTutorial("Empty"));
    EXPECT_FALSE(sys.IsTutorialActive());

    sys.Shutdown();
}
