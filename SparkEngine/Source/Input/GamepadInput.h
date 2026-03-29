/**
 * @file GamepadInput.h
 * @brief Xbox controller / gamepad input support via XInput
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides comprehensive gamepad input handling for up to 4 simultaneous
 * controllers via the Windows XInput API. Features include:
 *
 * - **Thumbstick input** with configurable circular or axial dead zones
 * - **Trigger analog input** with adjustable activation thresholds
 * - **Button state tracking** with press/release edge detection
 * - **Vibration/rumble feedback** with timed duration support
 * - **Action mapping** — bind named game actions to buttons or triggers
 * - **Connection polling** — efficiently detects controller connect/disconnect
 *
 * The GamepadInput class manages per-controller state independently, allowing
 * local multiplayer scenarios. Disconnected controllers are polled at a low
 * frequency (every 2 seconds) to avoid performance overhead.
 *
 * Typical usage:
 * @code
 *   GamepadInput gamepad;
 *   gamepad.BindAction("Jump", GamepadButton::A);
 *   gamepad.BindAction("Shoot", GamepadTrigger::Right, 0.3f);
 *
 *   // In game loop:
 *   gamepad.Update(deltaTime);
 *   if (gamepad.WasActionPressed("Jump")) player.Jump();
 *   auto stick = gamepad.GetLeftStick();  // {x, y} in [-1, 1]
 * @endcode
 *
 * @note Requires the XInput library (linked via #pragma comment).
 * @see InputManager
 */

#pragma once
#include "../Core/Platform.h"

#include "Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#include <xinput.h>
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <unordered_map>
#include <functional>
#include <array>

#ifdef _MSC_VER
#pragma comment(lib, "xinput.lib")
#endif

using DirectX::XMFLOAT2;

/**
 * @brief Gamepad button identifiers matching the Xbox controller layout
 *
 * These correspond to the standard Xbox controller button positions.
 * The Count sentinel is used for array sizing and range validation.
 */
enum class GamepadButton
{
    A,           ///< A button (bottom face button)
    B,           ///< B button (right face button)
    X,           ///< X button (left face button)
    Y,           ///< Y button (top face button)
    LeftBumper,  ///< Left bumper (LB / L1)
    RightBumper, ///< Right bumper (RB / R1)
    Back,        ///< Back / Select / View button
    Start,       ///< Start / Menu button
    LeftStick,   ///< Left thumbstick click (L3)
    RightStick,  ///< Right thumbstick click (R3)
    DPadUp,      ///< D-pad up
    DPadDown,    ///< D-pad down
    DPadLeft,    ///< D-pad left
    DPadRight,   ///< D-pad right
    Count        ///< Sentinel value for array sizing — not a valid button
};

/**
 * @brief Gamepad analog trigger identifiers
 */
enum class GamepadTrigger
{
    Left, ///< Left trigger (LT / L2)
    Right ///< Right trigger (RT / R2)
};

/**
 * @brief Gamepad thumbstick identifiers
 */
enum class GamepadStick
{
    Left, ///< Left thumbstick
    Right ///< Right thumbstick
};

/**
 * @brief Dead zone processing mode for thumbstick input
 *
 * Dead zones prevent drift from thumbstick hardware imprecision.
 * - **Circular**: treats both axes together; magnitude below the threshold
 *   is zeroed. Produces smooth diagonal movement. (Recommended)
 * - **Axial**: treats each axis independently; small X or Y values are
 *   zeroed even if the total magnitude is significant. Simpler but can
 *   feel less smooth.
 */
enum class DeadZoneMode
{
    Circular, ///< Radial dead zone based on combined stick magnitude (recommended)
    Axial     ///< Per-axis dead zone applied to X and Y independently
};

/**
 * @brief Per-controller state snapshot holding raw and processed input
 *
 * Stores both the raw XInput state (for button edge detection) and the
 * processed floating-point values (dead-zone-corrected sticks and
 * normalized triggers) plus vibration motor state.
 */
struct GamepadState
{
    bool connected = false;            ///< Whether this controller is currently connected
    XINPUT_STATE xinputState = {};     ///< Current raw XInput state
    XINPUT_STATE prevXinputState = {}; ///< Previous frame's raw XInput state (for edge detection)

    // Processed values (dead-zone-corrected and normalized)
    XMFLOAT2 leftStick = {0, 0};  ///< Left thumbstick position in [-1, 1] range (dead-zone applied)
    XMFLOAT2 rightStick = {0, 0}; ///< Right thumbstick position in [-1, 1] range (dead-zone applied)
    float leftTrigger = 0.0f;     ///< Left trigger value in [0, 1] range (threshold applied)
    float rightTrigger = 0.0f;    ///< Right trigger value in [0, 1] range (threshold applied)

    // Vibration state
    float leftMotor = 0.0f;      ///< Left vibration motor intensity [0, 1] (low-frequency rumble)
    float rightMotor = 0.0f;     ///< Right vibration motor intensity [0, 1] (high-frequency buzz)
    float vibrationTimer = 0.0f; ///< Remaining vibration duration in seconds (0 = indefinite)
};

/**
 * @brief Gamepad input manager supporting up to 4 simultaneous Xbox controllers
 *
 * GamepadInput wraps the Windows XInput API and provides processed, game-ready
 * input values. It handles dead zone processing, trigger normalization, button
 * edge detection, vibration timing, and a named action binding system.
 *
 * The class polls XInput at full frame rate for connected controllers and at a
 * reduced rate (CONNECTION_POLL_INTERVAL) for disconnected slots to minimize
 * overhead.
 *
 * @note All methods accept an optional controllerIndex parameter (0–3) that
 *       defaults to 0 (the first controller). Out-of-range indices return
 *       safe default values rather than crashing.
 */
class GamepadInput
{
  public:
    static constexpr int MAX_CONTROLLERS = 4; ///< Maximum number of simultaneous controllers (XInput limit)

    /** @brief Initialize dead zone and sensitivity settings to platform defaults */
    GamepadInput();

    /** @brief Stop all vibrations on destruction */
    ~GamepadInput();

    /**
     * @brief Poll all controllers and update processed input values
     *
     * Reads the raw XInput state for each connected controller, applies dead
     * zone processing to thumbsticks, normalizes trigger values, updates
     * vibration timers, and polls for newly connected controllers at a
     * reduced interval.
     *
     * @param deltaTime Time elapsed since the last frame in seconds, used
     *                  for vibration duration countdown and connection polling
     */
    void Update(float deltaTime);

    // ============================================================================
    // Connection Queries
    // ============================================================================

    /**
     * @brief Check if a specific controller is connected
     * @param controllerIndex Controller slot index (0–3, default: 0)
     * @return true if the controller is connected, false otherwise
     */
    bool IsConnected(int controllerIndex = 0) const;

    /**
     * @brief Get the number of currently connected controllers
     * @return Count of connected controllers (0–4)
     */
    int GetConnectedCount() const;

    // ============================================================================
    // Button Queries (with edge detection)
    // ============================================================================

    /**
     * @brief Check if a button is currently held down
     * @param button       The button to query
     * @param controllerIndex Controller slot index (0–3, default: 0)
     * @return true if the button is currently pressed
     */
    bool IsButtonDown(GamepadButton button, int controllerIndex = 0) const;

    /**
     * @brief Check if a button was pressed this frame (rising edge)
     *
     * Returns true only on the frame the button transitions from released to pressed.
     *
     * @param button       The button to query
     * @param controllerIndex Controller slot index (0–3, default: 0)
     * @return true if the button was just pressed this frame
     */
    bool WasButtonPressed(GamepadButton button, int controllerIndex = 0) const;

    /**
     * @brief Check if a button was released this frame (falling edge)
     *
     * Returns true only on the frame the button transitions from pressed to released.
     *
     * @param button       The button to query
     * @param controllerIndex Controller slot index (0–3, default: 0)
     * @return true if the button was just released this frame
     */
    bool WasButtonReleased(GamepadButton button, int controllerIndex = 0) const;

    // ============================================================================
    // Thumbstick Queries (dead-zone-corrected, range [-1, 1])
    // ============================================================================

    /**
     * @brief Get the left thumbstick position with dead zone applied
     * @param controllerIndex Controller slot index (0–3, default: 0)
     * @return 2D vector with X and Y in the range [-1, 1]. Returns {0,0} if
     *         within the dead zone or if the controller is disconnected.
     */
    XMFLOAT2 GetLeftStick(int controllerIndex = 0) const;

    /**
     * @brief Get the right thumbstick position with dead zone applied
     * @param controllerIndex Controller slot index (0–3, default: 0)
     * @return 2D vector with X and Y in the range [-1, 1]
     */
    XMFLOAT2 GetRightStick(int controllerIndex = 0) const;

    /** @brief Get the left stick X-axis value @param controllerIndex Controller index @return Value in [-1, 1] */
    float GetLeftStickX(int controllerIndex = 0) const;
    /** @brief Get the left stick Y-axis value @param controllerIndex Controller index @return Value in [-1, 1] */
    float GetLeftStickY(int controllerIndex = 0) const;
    /** @brief Get the right stick X-axis value @param controllerIndex Controller index @return Value in [-1, 1] */
    float GetRightStickX(int controllerIndex = 0) const;
    /** @brief Get the right stick Y-axis value @param controllerIndex Controller index @return Value in [-1, 1] */
    float GetRightStickY(int controllerIndex = 0) const;

    // ============================================================================
    // Trigger Queries (normalized to [0.0, 1.0])
    // ============================================================================

    /**
     * @brief Get the left trigger's analog value
     * @param controllerIndex Controller slot index (0–3, default: 0)
     * @return Normalized value in [0.0, 1.0] where 0.0 is fully released
     *         and 1.0 is fully pressed
     */
    float GetLeftTrigger(int controllerIndex = 0) const;

    /**
     * @brief Get the right trigger's analog value
     * @param controllerIndex Controller slot index (0–3, default: 0)
     * @return Normalized value in [0.0, 1.0]
     */
    float GetRightTrigger(int controllerIndex = 0) const;

    /**
     * @brief Check if a trigger is pressed beyond a threshold
     *
     * Useful for treating analog triggers as digital buttons.
     *
     * @param trigger         Which trigger to check
     * @param threshold       Activation threshold in [0.0, 1.0] (default: 0.5)
     * @param controllerIndex Controller slot index (0–3, default: 0)
     * @return true if the trigger value exceeds the threshold
     */
    bool IsTriggerDown(GamepadTrigger trigger, float threshold = 0.5f, int controllerIndex = 0) const;

    // ============================================================================
    // Vibration / Rumble
    // ============================================================================

    /**
     * @brief Set vibration intensity for a controller with optional duration
     *
     * The left motor produces a low-frequency rumble and the right motor
     * produces a high-frequency buzz. Combining both creates rich haptic
     * feedback. If duration is 0, vibration continues until explicitly stopped.
     *
     * @param leftMotor       Left motor intensity in [0.0, 1.0] (low-frequency rumble)
     * @param rightMotor      Right motor intensity in [0.0, 1.0] (high-frequency buzz)
     * @param duration        Duration in seconds, or 0.0 for indefinite (default: 0.0)
     * @param controllerIndex Controller slot index (0–3, default: 0)
     */
    void SetVibration(float leftMotor, float rightMotor, float duration = 0.0f, int controllerIndex = 0);

    /**
     * @brief Stop vibration on a specific controller
     * @param controllerIndex Controller slot index (0–3, default: 0)
     */
    void StopVibration(int controllerIndex = 0);

    /**
     * @brief Stop all vibrations on all connected controllers
     *
     * Convenience method that iterates all controller slots and sets
     * both motors to zero intensity.
     */
    void StopAllVibrations();

    // ============================================================================
    // Dead Zone and Sensitivity Configuration
    // ============================================================================

    /**
     * @brief Set the dead zone radius for a specific thumbstick
     *
     * Values within the dead zone radius are treated as zero to compensate
     * for hardware drift. The XInput default dead zones are approximately
     * 0.24 for the left stick and 0.26 for the right stick.
     *
     * @param stick    Which thumbstick to configure
     * @param deadZone Dead zone radius in [0.0, 1.0] (e.g., 0.24 = 24% of full range)
     */
    void SetDeadZone(GamepadStick stick, float deadZone);

    /**
     * @brief Get the current dead zone radius for a thumbstick
     * @param stick Which thumbstick to query
     * @return Current dead zone radius in [0.0, 1.0]
     */
    float GetDeadZone(GamepadStick stick) const;

    /**
     * @brief Set the dead zone processing mode (circular vs. axial)
     * @param mode The dead zone mode to use
     * @see DeadZoneMode
     */
    void SetDeadZoneMode(DeadZoneMode mode) { m_deadZoneMode = mode; }

    /**
     * @brief Set the minimum trigger value to register input
     * @param threshold Trigger activation threshold in [0.0, 1.0] (default: 0.12)
     */
    void SetTriggerThreshold(float threshold) { m_triggerThreshold = threshold; }

    /**
     * @brief Set the global thumbstick sensitivity multiplier
     * @param sensitivity Multiplier applied after dead zone correction (default: 1.0)
     */
    void SetStickSensitivity(float sensitivity) { m_stickSensitivity = sensitivity; }

    // ============================================================================
    // Action Mapping (maps gamepad inputs to named game actions)
    // ============================================================================

    /**
     * @brief Bind a named game action to a gamepad button
     *
     * @code
     *   gamepad.BindAction("Jump", GamepadButton::A);
     *   gamepad.BindAction("Crouch", GamepadButton::B);
     * @endcode
     *
     * @param action Name of the game action (case-sensitive)
     * @param button Gamepad button to bind to the action
     */
    void BindAction(const std::string& action, GamepadButton button);

    /**
     * @brief Bind a named game action to a gamepad trigger
     *
     * @code
     *   gamepad.BindAction("Shoot", GamepadTrigger::Right, 0.3f);
     *   gamepad.BindAction("AimDownSights", GamepadTrigger::Left, 0.2f);
     * @endcode
     *
     * @param action    Name of the game action (case-sensitive)
     * @param trigger   Which analog trigger to bind
     * @param threshold Activation threshold in [0.0, 1.0] (default: 0.5)
     */
    void BindAction(const std::string& action, GamepadTrigger trigger, float threshold = 0.5f);

    /**
     * @brief Check if a named action is currently active (held down)
     * @param action          Name of the game action
     * @param controllerIndex Controller slot index (0–3, default: 0)
     * @return true if the bound input is active
     */
    bool IsActionDown(const std::string& action, int controllerIndex = 0) const;

    /**
     * @brief Check if a named action was activated this frame (rising edge)
     * @param action          Name of the game action
     * @param controllerIndex Controller slot index (0–3, default: 0)
     * @return true if the bound input was just pressed this frame
     */
    bool WasActionPressed(const std::string& action, int controllerIndex = 0) const;

    // ============================================================================
    // Console Integration
    // ============================================================================

    /**
     * @brief Get a human-readable status string for console display
     * @return Multi-line string showing connection status, stick values, and trigger states
     */
    std::string Console_GetStatus() const;

    /**
     * @brief Get a formatted list of all action bindings for console display
     * @return Multi-line string listing each action and its bound input
     */
    std::string Console_ListBindings() const;

  private:
    /**
     * @brief Convert a GamepadButton enum to the corresponding XInput WORD bitmask
     * @param button The GamepadButton to convert
     * @return XInput button bitmask (e.g., XINPUT_GAMEPAD_A)
     */
    WORD GetXInputButton(GamepadButton button) const;

    /**
     * @brief Apply dead zone processing to raw thumbstick values
     *
     * Converts raw XInput SHORT values to normalized floats in [-1, 1]
     * with the configured dead zone mode (circular or axial) applied.
     *
     * @param rawX     Raw X-axis value from XInput (-32768 to 32767)
     * @param rawY     Raw Y-axis value from XInput (-32768 to 32767)
     * @param deadZone Dead zone radius in [0, 1]
     * @return Processed 2D stick position in [-1, 1] range
     */
    XMFLOAT2 ApplyDeadZone(short rawX, short rawY, float deadZone) const;

    /**
     * @brief Normalize a raw trigger BYTE value to [0.0, 1.0]
     * @param raw Raw trigger value from XInput (0–255)
     * @return Normalized trigger value in [0.0, 1.0], or 0.0 if below threshold
     */
    float NormalizeTrigger(BYTE raw) const;

    std::array<GamepadState, MAX_CONTROLLERS> m_states; ///< Per-controller state arrays

    // Dead zone configuration
    float m_leftStickDeadZone = 0.24f;                    ///< Left stick dead zone (~7849 / 32767, XInput default)
    float m_rightStickDeadZone = 0.26f;                   ///< Right stick dead zone (~8689 / 32767, XInput default)
    float m_triggerThreshold = 0.12f;                     ///< Minimum trigger value to register (~30 / 255)
    DeadZoneMode m_deadZoneMode = DeadZoneMode::Circular; ///< Active dead zone processing mode
    float m_stickSensitivity = 1.0f;                      ///< Global stick sensitivity multiplier

    /**
     * @brief Internal action binding record linking a game action name to a button or trigger
     */
    struct ActionBinding
    {
        enum class Type
        {
            Button,
            Trigger
        }; ///< Whether this binding targets a button or trigger
        Type type;              ///< Type of input bound to this action
        GamepadButton button;   ///< Button identifier (valid when type == Button)
        GamepadTrigger trigger; ///< Trigger identifier (valid when type == Trigger)
        float triggerThreshold; ///< Activation threshold for trigger bindings
    };
    std::unordered_map<std::string, ActionBinding> m_actionBindings; ///< Map of action names to their bindings

    // Connection polling (don't check every frame for disconnected controllers)
    float m_connectionPollTimer = 0.0f;                     ///< Time since last disconnected controller poll
    static constexpr float CONNECTION_POLL_INTERVAL = 2.0f; ///< Seconds between disconnected controller polls
};
