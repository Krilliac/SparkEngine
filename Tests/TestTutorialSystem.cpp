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
