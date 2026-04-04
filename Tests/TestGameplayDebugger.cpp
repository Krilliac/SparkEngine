// TestGameplayDebugger.cpp - Tests for Spark::Utils::GameplayDebugger
#include "TestFramework.h"
#include "Utils/GameplayDebugger.h"

// ============================================================================
// Initialization
// ============================================================================

TEST(GameplayDebugger_Initialize)
{
    auto& dbg = Spark::Utils::GameplayDebugger::GetInstance();
    dbg.Initialize();
    EXPECT_EQ(dbg.GetTotalEntryCount(), static_cast<size_t>(0));
    EXPECT_EQ(dbg.GetEnabledCategoryCount(), static_cast<size_t>(0));
    dbg.Shutdown();
}

// ============================================================================
// Category management
// ============================================================================

TEST(GameplayDebugger_EnableCategory)
{
    auto& dbg = Spark::Utils::GameplayDebugger::GetInstance();
    dbg.Initialize();

    EXPECT_FALSE(dbg.IsCategoryEnabled(Spark::Utils::DebugCategory::AI));
    dbg.SetCategoryEnabled(Spark::Utils::DebugCategory::AI, true);
    EXPECT_TRUE(dbg.IsCategoryEnabled(Spark::Utils::DebugCategory::AI));
    EXPECT_EQ(dbg.GetEnabledCategoryCount(), static_cast<size_t>(1));
    dbg.Shutdown();
}

TEST(GameplayDebugger_ToggleCategory)
{
    auto& dbg = Spark::Utils::GameplayDebugger::GetInstance();
    dbg.Initialize();

    dbg.ToggleCategory(Spark::Utils::DebugCategory::Physics);
    EXPECT_TRUE(dbg.IsCategoryEnabled(Spark::Utils::DebugCategory::Physics));
    dbg.ToggleCategory(Spark::Utils::DebugCategory::Physics);
    EXPECT_FALSE(dbg.IsCategoryEnabled(Spark::Utils::DebugCategory::Physics));
    dbg.Shutdown();
}

// ============================================================================
// Debug entries
// ============================================================================

TEST(GameplayDebugger_AddEntityText)
{
    auto& dbg = Spark::Utils::GameplayDebugger::GetInstance();
    dbg.Initialize();

    dbg.AddEntityText(1, Spark::Utils::DebugCategory::AI, "State: Patrol", 0xFFFFFFFF, 1.0f);
    EXPECT_EQ(dbg.GetTotalEntryCount(), static_cast<size_t>(1));
    dbg.Shutdown();
}

TEST(GameplayDebugger_AddEntitySphere)
{
    auto& dbg = Spark::Utils::GameplayDebugger::GetInstance();
    dbg.Initialize();

    dbg.AddEntitySphere(1, Spark::Utils::DebugCategory::AI, 0.0f, 0.0f, 0.0f, 5.0f, 0xFF00FF00, 2.0f);
    EXPECT_EQ(dbg.GetTotalEntryCount(), static_cast<size_t>(1));
    dbg.Shutdown();
}

TEST(GameplayDebugger_AddLine)
{
    auto& dbg = Spark::Utils::GameplayDebugger::GetInstance();
    dbg.Initialize();

    dbg.AddLine(1, Spark::Utils::DebugCategory::Navigation, 0, 0, 0, 10, 0, 10, 0xFFFF0000, 1.0f);
    EXPECT_EQ(dbg.GetTotalEntryCount(), static_cast<size_t>(1));
    dbg.Shutdown();
}

// ============================================================================
// Visibility filtering
// ============================================================================

TEST(GameplayDebugger_GetVisibleEntries_FiltersByCategory)
{
    auto& dbg = Spark::Utils::GameplayDebugger::GetInstance();
    dbg.Initialize();

    dbg.AddEntityText(1, Spark::Utils::DebugCategory::AI, "AI Text", 0xFFFFFFFF, 1.0f);
    dbg.AddEntityText(2, Spark::Utils::DebugCategory::Physics, "Physics Text", 0xFFFFFFFF, 1.0f);

    // No categories enabled — nothing visible
    auto visible = dbg.GetVisibleTextEntries();
    EXPECT_EQ(visible.size(), static_cast<size_t>(0));

    // Enable AI only
    dbg.SetCategoryEnabled(Spark::Utils::DebugCategory::AI, true);
    visible = dbg.GetVisibleTextEntries();
    EXPECT_EQ(visible.size(), static_cast<size_t>(1));
    dbg.Shutdown();
}

// ============================================================================
// Lifetime expiry
// ============================================================================

TEST(GameplayDebugger_Update_ExpiresEntries)
{
    auto& dbg = Spark::Utils::GameplayDebugger::GetInstance();
    dbg.Initialize();

    dbg.AddEntityText(1, Spark::Utils::DebugCategory::Gameplay, "Temp", 0xFFFFFFFF, 0.5f);
    EXPECT_EQ(dbg.GetTotalEntryCount(), static_cast<size_t>(1));

    // After 0.3s, still alive
    dbg.Update(0.3f);
    EXPECT_EQ(dbg.GetTotalEntryCount(), static_cast<size_t>(1));

    // After another 0.3s, expired
    dbg.Update(0.3f);
    EXPECT_EQ(dbg.GetTotalEntryCount(), static_cast<size_t>(0));
    dbg.Shutdown();
}

TEST(GameplayDebugger_Update_ZeroLifetime_ExpiresImmediately)
{
    auto& dbg = Spark::Utils::GameplayDebugger::GetInstance();
    dbg.Initialize();

    dbg.AddEntityText(1, Spark::Utils::DebugCategory::AI, "OneFrame", 0xFFFFFFFF, 0.0f);
    EXPECT_EQ(dbg.GetTotalEntryCount(), static_cast<size_t>(1));

    dbg.Update(0.016f);
    EXPECT_EQ(dbg.GetTotalEntryCount(), static_cast<size_t>(0));
    dbg.Shutdown();
}

// ============================================================================
// Category names
// ============================================================================

TEST(GameplayDebugger_CategoryNames)
{
    EXPECT_EQ(std::string(Spark::Utils::GetCategoryName(Spark::Utils::DebugCategory::AI)), std::string("AI"));
    EXPECT_EQ(std::string(Spark::Utils::GetCategoryName(Spark::Utils::DebugCategory::Physics)), std::string("Physics"));
    EXPECT_EQ(std::string(Spark::Utils::GetCategoryName(Spark::Utils::DebugCategory::Navigation)),
              std::string("Navigation"));
}

TEST(GameplayDebugger_ConsoleStatus)
{
    auto& dbg = Spark::Utils::GameplayDebugger::GetInstance();
    dbg.Initialize();
    std::string status = dbg.Console_GetStatus();
    EXPECT_TRUE(status.find("GameplayDebugger") != std::string::npos);
    dbg.Shutdown();
}
