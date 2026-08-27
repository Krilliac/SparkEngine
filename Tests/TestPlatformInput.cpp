// TestPlatformInput.cpp - Tests for Spark::Input::InputActionMap
#include "TestFramework.h"
#include "Input/PlatformInput.h"

using namespace Spark::Input;

// ============================================================================
// RegisterAction and GetAction
// ============================================================================

TEST(PlatformInput_RegisterAction_GetAction)
{
    InputActionMap map;
    map.RegisterAction("Jump", PlatformKeyCode::Space);

    const auto* action = map.GetAction("Jump");
    ASSERT_TRUE(action != nullptr);
    EXPECT_TRUE(action->name == "Jump");
    EXPECT_EQ(static_cast<uint16_t>(action->primaryKey), static_cast<uint16_t>(PlatformKeyCode::Space));
    EXPECT_EQ(static_cast<uint16_t>(action->secondaryKey), static_cast<uint16_t>(PlatformKeyCode::None));
}

TEST(PlatformInput_RegisterAction_WithSecondaryKey)
{
    InputActionMap map;
    map.RegisterAction("Fire", PlatformKeyCode::Mouse_Left, PlatformKeyCode::Gamepad_RT);

    const auto* action = map.GetAction("Fire");
    ASSERT_TRUE(action != nullptr);
    EXPECT_EQ(static_cast<uint16_t>(action->primaryKey), static_cast<uint16_t>(PlatformKeyCode::Mouse_Left));
    EXPECT_EQ(static_cast<uint16_t>(action->secondaryKey), static_cast<uint16_t>(PlatformKeyCode::Gamepad_RT));
}

TEST(PlatformInput_GetAction_NonExistent_ReturnsNull)
{
    InputActionMap map;
    const auto* action = map.GetAction("Nonexistent");
    EXPECT_TRUE(action == nullptr);
}

// ============================================================================
// IsActionPressed with no backend
// ============================================================================

TEST(PlatformInput_IsActionPressed_NoBackend_ReturnsFalse)
{
    InputActionMap map;
    map.RegisterAction("Jump", PlatformKeyCode::Space);

    // Without calling UpdateActions, isPressed stays false
    EXPECT_FALSE(map.IsActionPressed("Jump"));
}

TEST(PlatformInput_IsActionPressed_WithNullBackend_ReturnsFalse)
{
    InputActionMap map;
    map.RegisterAction("Jump", PlatformKeyCode::Space);

    NullInputBackend nullBackend;
    nullBackend.Initialize();
    map.UpdateActions(nullBackend);

    // NullInputBackend always returns false for IsKeyDown
    EXPECT_FALSE(map.IsActionPressed("Jump"));
}

// ============================================================================
// RemoveAction
// ============================================================================

TEST(PlatformInput_RemoveAction)
{
    InputActionMap map;
    map.RegisterAction("Jump", PlatformKeyCode::Space);
    EXPECT_TRUE(map.GetAction("Jump") != nullptr);

    map.RemoveAction("Jump");
    EXPECT_TRUE(map.GetAction("Jump") == nullptr);
}

TEST(PlatformInput_RemoveAction_NonExistent_NoError)
{
    InputActionMap map;
    // Should not crash or error
    map.RemoveAction("Nonexistent");
    EXPECT_TRUE(map.GetAllActions().empty());
}

// ============================================================================
// GetAllActions
// ============================================================================

TEST(PlatformInput_GetAllActions_ReturnsRegistered)
{
    InputActionMap map;
    map.RegisterAction("Jump", PlatformKeyCode::Space);
    map.RegisterAction("Fire", PlatformKeyCode::Mouse_Left);
    map.RegisterAction("Crouch", PlatformKeyCode::Ctrl);

    const auto& all = map.GetAllActions();
    EXPECT_EQ(all.size(), 3u);
    EXPECT_TRUE(all.find("Jump") != all.end());
    EXPECT_TRUE(all.find("Fire") != all.end());
    EXPECT_TRUE(all.find("Crouch") != all.end());
}

TEST(PlatformInput_GetAllActions_EmptyByDefault)
{
    InputActionMap map;
    EXPECT_TRUE(map.GetAllActions().empty());
}

// ============================================================================
// SaveToConfig / LoadFromConfig roundtrip
// ============================================================================

TEST(PlatformInput_SaveLoad_Roundtrip)
{
    InputActionMap original;
    original.RegisterAction("Jump", PlatformKeyCode::Space);
    original.RegisterAction("Fire", PlatformKeyCode::Mouse_Left, PlatformKeyCode::Gamepad_RT);

    std::string config = original.SaveToConfig();
    EXPECT_FALSE(config.empty());

    InputActionMap loaded;
    loaded.LoadFromConfig(config);

    const auto* jump = loaded.GetAction("Jump");
    ASSERT_TRUE(jump != nullptr);
    EXPECT_EQ(static_cast<uint16_t>(jump->primaryKey), static_cast<uint16_t>(PlatformKeyCode::Space));

    const auto* fire = loaded.GetAction("Fire");
    ASSERT_TRUE(fire != nullptr);
    EXPECT_EQ(static_cast<uint16_t>(fire->primaryKey), static_cast<uint16_t>(PlatformKeyCode::Mouse_Left));
    EXPECT_EQ(static_cast<uint16_t>(fire->secondaryKey), static_cast<uint16_t>(PlatformKeyCode::Gamepad_RT));
}

TEST(PlatformInput_SaveToConfig_ContainsActionNames)
{
    InputActionMap map;
    map.RegisterAction("MoveForward", PlatformKeyCode::W);
    map.RegisterAction("MoveBack", PlatformKeyCode::S);

    std::string config = map.SaveToConfig();
    EXPECT_STR_CONTAINS(config, "MoveForward");
    EXPECT_STR_CONTAINS(config, "MoveBack");
    EXPECT_STR_CONTAINS(config, "primary");
    EXPECT_STR_CONTAINS(config, "secondary");
}
