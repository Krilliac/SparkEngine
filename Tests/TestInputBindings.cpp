/**
 * @file TestInputBindings.cpp
 * @brief Tests for Spark::InputBindingManager
 */

#include "TestFramework.h"
#include "../SparkEngine/Source/Input/InputBindings.h"

TEST(InputBindings_SetAndGet)
{
    Spark::InputBindingManager mgr;

    Spark::InputBinding binding;
    binding.primaryKey = 0x57; // W
    binding.alternateKey = 0;
    mgr.SetBinding("MoveForward", binding);

    auto result = mgr.GetBinding("MoveForward");
    EXPECT_EQ(result.primaryKey, 0x57);
}

TEST(InputBindings_Remove)
{
    Spark::InputBindingManager mgr;

    Spark::InputBinding binding;
    binding.primaryKey = 0x20;
    mgr.SetBinding("Jump", binding);
    mgr.RemoveBinding("Jump");

    auto result = mgr.GetBinding("Jump");
    EXPECT_EQ(result.primaryKey, 0); // Default empty binding
}

TEST(InputBindings_ConflictDetection)
{
    Spark::InputBindingManager mgr;

    Spark::InputBinding b1;
    b1.primaryKey = 0x57;
    mgr.SetBinding("MoveForward", b1);

    Spark::InputBinding b2;
    b2.primaryKey = 0x57; // Same key!
    mgr.SetBinding("Jump", b2);

    std::string conflict = mgr.FindConflict("Jump");
    EXPECT_EQ(conflict, std::string("MoveForward"));
}

TEST(InputBindings_Presets)
{
    Spark::InputBindingManager mgr;

    auto presets = mgr.GetPresetNames();
    EXPECT_TRUE(presets.size() >= static_cast<size_t>(2)); // Default + Left-Handed

    EXPECT_TRUE(mgr.ApplyPreset("Default"));
    auto binding = mgr.GetBinding("MoveForward");
    EXPECT_EQ(binding.primaryKey, 0x57); // W
}

TEST(InputBindings_Accessibility)
{
    Spark::InputBindingManager mgr;
    mgr.SetAccessibility(Spark::AccessibilityFlags::HoldToToggle | Spark::AccessibilityFlags::ColorblindDeuteranopia);

    EXPECT_TRUE(mgr.IsAccessibilityEnabled(Spark::AccessibilityFlags::HoldToToggle));
    EXPECT_TRUE(mgr.IsAccessibilityEnabled(Spark::AccessibilityFlags::ColorblindDeuteranopia));
    EXPECT_FALSE(mgr.IsAccessibilityEnabled(Spark::AccessibilityFlags::ReducedMotion));
}
