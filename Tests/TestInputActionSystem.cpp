// TestInputActionSystem.cpp - Tests for Spark::Input::InputActionSystem
#include "TestFramework.h"
#include "Input/InputActionSystem.h"

#include <unordered_set>

// Helper: simulated key state
static std::unordered_set<int> s_keysDown;
static std::unordered_set<int> s_keysPressed;
static std::unordered_set<int> s_keysReleased;

static void ResetKeys()
{
    s_keysDown.clear();
    s_keysPressed.clear();
    s_keysReleased.clear();
}

static void SetupProviders(Spark::Input::InputActionSystem& sys)
{
    sys.SetKeyStateProviders([](int k) { return s_keysDown.count(k) > 0; }, [](int k)
                             { return s_keysPressed.count(k) > 0; }, [](int k) { return s_keysReleased.count(k) > 0; });
}

// ============================================================================
// Initialization
// ============================================================================

TEST(InputAction_Initialize)
{
    auto& sys = Spark::Input::InputActionSystem::GetInstance();
    sys.Initialize();
    EXPECT_EQ(sys.GetActionCount(), static_cast<size_t>(0));
    EXPECT_EQ(sys.GetContextCount(), static_cast<size_t>(0));
    sys.Shutdown();
}

// ============================================================================
// Action registration
// ============================================================================

TEST(InputAction_RegisterAction)
{
    auto& sys = Spark::Input::InputActionSystem::GetInstance();
    sys.Initialize();
    EXPECT_TRUE(sys.RegisterAction("Jump", Spark::Input::ActionType::Button));
    EXPECT_EQ(sys.GetActionCount(), static_cast<size_t>(1));
    sys.Shutdown();
}

TEST(InputAction_RegisterAction_Duplicate_Fails)
{
    auto& sys = Spark::Input::InputActionSystem::GetInstance();
    sys.Initialize();
    sys.RegisterAction("Jump", Spark::Input::ActionType::Button);
    EXPECT_FALSE(sys.RegisterAction("Jump", Spark::Input::ActionType::Button));
    sys.Shutdown();
}

TEST(InputAction_UnregisterAction)
{
    auto& sys = Spark::Input::InputActionSystem::GetInstance();
    sys.Initialize();
    sys.RegisterAction("Jump", Spark::Input::ActionType::Button);
    sys.UnregisterAction("Jump");
    EXPECT_EQ(sys.GetActionCount(), static_cast<size_t>(0));
    sys.Shutdown();
}

// ============================================================================
// Key binding
// ============================================================================

TEST(InputAction_BindKey)
{
    auto& sys = Spark::Input::InputActionSystem::GetInstance();
    sys.Initialize();
    sys.RegisterAction("Jump", Spark::Input::ActionType::Button);
    EXPECT_TRUE(sys.BindKey("Jump", 0x20)); // VK_SPACE
    auto* bindings = sys.GetBindings("Jump");
    EXPECT_TRUE(bindings != nullptr);
    EXPECT_EQ(bindings->size(), static_cast<size_t>(1));
    sys.Shutdown();
}

TEST(InputAction_BindKey_UnknownAction_Fails)
{
    auto& sys = Spark::Input::InputActionSystem::GetInstance();
    sys.Initialize();
    EXPECT_FALSE(sys.BindKey("NoAction", 0x20));
    sys.Shutdown();
}

TEST(InputAction_Rebind)
{
    auto& sys = Spark::Input::InputActionSystem::GetInstance();
    sys.Initialize();
    sys.RegisterAction("Fire", Spark::Input::ActionType::Button);
    sys.BindKey("Fire", 0x01);             // Mouse left
    EXPECT_TRUE(sys.Rebind("Fire", 0x02)); // Change to mouse right
    auto* bindings = sys.GetBindings("Fire");
    EXPECT_EQ((*bindings)[0].keyCode, 0x02);
    sys.Shutdown();
}

// ============================================================================
// Button evaluation
// ============================================================================

TEST(InputAction_ButtonPressed)
{
    auto& sys = Spark::Input::InputActionSystem::GetInstance();
    sys.Initialize();
    SetupProviders(sys);
    ResetKeys();

    sys.RegisterAction("Jump", Spark::Input::ActionType::Button);
    sys.BindKey("Jump", 0x20, Spark::Input::InputTrigger::Pressed);

    s_keysPressed.insert(0x20);
    sys.Update();
    EXPECT_TRUE(sys.IsActionTriggered("Jump"));

    ResetKeys();
    sys.Update();
    EXPECT_FALSE(sys.IsActionTriggered("Jump"));
    sys.Shutdown();
}

TEST(InputAction_ButtonHeld)
{
    auto& sys = Spark::Input::InputActionSystem::GetInstance();
    sys.Initialize();
    SetupProviders(sys);
    ResetKeys();

    sys.RegisterAction("Sprint", Spark::Input::ActionType::Button);
    sys.BindKey("Sprint", 0x10, Spark::Input::InputTrigger::Held); // Shift

    s_keysDown.insert(0x10);
    sys.Update();
    EXPECT_TRUE(sys.IsActionActive("Sprint"));

    ResetKeys();
    sys.Update();
    EXPECT_FALSE(sys.IsActionActive("Sprint"));
    sys.Shutdown();
}

// ============================================================================
// Composite 2D
// ============================================================================

TEST(InputAction_Composite2D_WASD)
{
    auto& sys = Spark::Input::InputActionSystem::GetInstance();
    sys.Initialize();
    SetupProviders(sys);
    ResetKeys();

    sys.RegisterAction("Move", Spark::Input::ActionType::Axis2D);
    sys.BindComposite2D("Move", 0x57, 0x53, 0x41, 0x44); // W, S, A, D

    // Press W (forward = +Y)
    s_keysDown.insert(0x57);
    sys.Update();
    auto val = sys.GetAxis2D("Move");
    EXPECT_TRUE(val.y > 0.0f);
    EXPECT_TRUE(std::abs(val.x) < 0.01f);

    // Press W + D (diagonal, should be normalized)
    s_keysDown.insert(0x44);
    sys.Update();
    val = sys.GetAxis2D("Move");
    EXPECT_TRUE(val.x > 0.0f);
    EXPECT_TRUE(val.y > 0.0f);
    float len = std::sqrt(val.x * val.x + val.y * val.y);
    EXPECT_TRUE(std::abs(len - 1.0f) < 0.01f); // Normalized

    ResetKeys();
    sys.Shutdown();
}

// ============================================================================
// Contexts
// ============================================================================

TEST(InputAction_Context_PushPop)
{
    auto& sys = Spark::Input::InputActionSystem::GetInstance();
    sys.Initialize();
    SetupProviders(sys);

    sys.RegisterAction("Jump", Spark::Input::ActionType::Button);
    sys.BindKey("Jump", 0x20, Spark::Input::InputTrigger::Pressed);

    sys.CreateContext("Gameplay");
    sys.AddActionToContext("Gameplay", "Jump");

    sys.CreateContext("Menu", true);
    // Menu doesn't include Jump

    sys.PushContext("Gameplay");
    EXPECT_EQ(sys.GetActiveContext(), std::string("Gameplay"));

    // Jump should work in Gameplay context
    s_keysPressed.insert(0x20);
    sys.Update();
    EXPECT_TRUE(sys.IsActionTriggered("Jump"));

    // Push Menu context — Jump should be blocked (consume = true)
    s_keysPressed.clear();
    sys.PushContext("Menu");
    s_keysPressed.insert(0x20);
    sys.Update();
    EXPECT_FALSE(sys.IsActionTriggered("Jump"));

    sys.PopContext(); // Back to Gameplay
    EXPECT_EQ(sys.GetActiveContext(), std::string("Gameplay"));

    ResetKeys();
    sys.Shutdown();
}

TEST(InputAction_ConsoleGetStatus)
{
    auto& sys = Spark::Input::InputActionSystem::GetInstance();
    sys.Initialize();
    sys.RegisterAction("Test", Spark::Input::ActionType::Button);
    sys.CreateContext("Main");
    auto status = sys.Console_GetStatus();
    EXPECT_TRUE(status.find("Actions: 1") != std::string::npos);
    EXPECT_TRUE(status.find("Contexts: 1") != std::string::npos);
    sys.Shutdown();
}
