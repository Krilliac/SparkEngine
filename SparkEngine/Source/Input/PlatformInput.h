/**
 * @file PlatformInput.h
 * @brief Cross-platform input abstraction with action mapping and backend factory
 * @author Spark Engine Team
 * @date 2026
 *
 * @details
 * Provides a platform-agnostic input layer that decouples game logic from
 * OS-specific input APIs. Features:
 * - Unified key codes for keyboard, mouse, and gamepad
 * - Named action mapping with primary/secondary bindings
 * - Pluggable backends (Win32, SDL2, Null) via factory
 * - Analog input with configurable deadzones
 * - JSON serialization for user key remapping
 *
 * @code
 *   auto backend = Spark::Input::PlatformInputFactory::CreateBackend();
 *   backend->Initialize();
 *   Spark::Input::InputActionMap actions;
 *   actions.RegisterAction("Jump", PlatformKeyCode::Space);
 *   actions.RegisterAction("Fire", PlatformKeyCode::Mouse_Left);
 *   // Per frame:
 *   backend->PollEvents();
 *   if (actions.IsActionJustPressed("Jump")) { ... }
 * @endcode
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Spark::Input
{

    // ============================================================================
    // Platform Key Codes
    // ============================================================================

    /**
     * @brief Unified key/button codes across all input devices.
     *
     * Values are engine-defined and do not map 1:1 to OS virtual key codes.
     * Backends translate native codes to/from PlatformKeyCode.
     */
    enum class PlatformKeyCode : uint16_t
    {
        None = 0,

        // --- Letters ---
        A = 1,
        B,
        C,
        D,
        E,
        F,
        G,
        H,
        I,
        J,
        K,
        L,
        M,
        N,
        O,
        P,
        Q,
        R,
        S,
        T,
        U,
        V,
        W,
        X,
        Y,
        Z,

        // --- Digits ---
        Num0 = 30,
        Num1,
        Num2,
        Num3,
        Num4,
        Num5,
        Num6,
        Num7,
        Num8,
        Num9,

        // --- Function keys ---
        F1 = 50,
        F2,
        F3,
        F4,
        F5,
        F6,
        F7,
        F8,
        F9,
        F10,
        F11,
        F12,

        // --- Navigation ---
        Space = 70,
        Enter,
        Escape,
        Tab,
        Backspace,
        Delete,
        Home,
        End,
        PageUp,
        PageDown,
        Left,
        Right,
        Up,
        Down,

        // --- Modifiers ---
        Shift = 90,
        Ctrl,
        Alt,

        // --- Mouse buttons ---
        Mouse_Left = 110,
        Mouse_Right,
        Mouse_Middle,
        Mouse_X1,
        Mouse_X2,

        // --- Gamepad ---
        Gamepad_A = 130,
        Gamepad_B,
        Gamepad_X,
        Gamepad_Y,
        Gamepad_LB,
        Gamepad_RB,
        Gamepad_LT,
        Gamepad_RT,
        Gamepad_Start,
        Gamepad_Back,
        Gamepad_LeftStick,
        Gamepad_RightStick,
        Gamepad_DPadUp,
        Gamepad_DPadDown,
        Gamepad_DPadLeft,
        Gamepad_DPadRight,
        Gamepad_LeftStickX,
        Gamepad_LeftStickY,
        Gamepad_RightStickX,
        Gamepad_RightStickY,

        Count
    };

    // ============================================================================
    // Input Action
    // ============================================================================

    /**
     * @brief A named input action bound to one or two physical keys/buttons.
     */
    struct InputAction
    {
        std::string name;                                     ///< Human-readable action name
        PlatformKeyCode primaryKey = PlatformKeyCode::None;   ///< Primary binding
        PlatformKeyCode secondaryKey = PlatformKeyCode::None; ///< Alternate binding
        float deadzone = 0.15f;                               ///< Analog deadzone (0-1)
        bool isPressed = false;                               ///< Currently held
        bool isJustPressed = false;                           ///< Pressed this frame
        bool isJustReleased = false;                          ///< Released this frame
        float analogValue = 0.0f;                             ///< Analog axis value (-1 to 1)
    };

    // ============================================================================
    // Backend Interface
    // ============================================================================

    /**
     * @brief Abstract interface for platform-specific input backends.
     *
     * Implementations translate OS events into the engine's PlatformKeyCode space.
     */
    class IPlatformInputBackend
    {
      public:
        virtual ~IPlatformInputBackend() = default;

        /** @brief Initialize the backend. @return true on success. */
        virtual bool Initialize() = 0;

        /** @brief Release backend resources. */
        virtual void Shutdown() = 0;

        /** @brief Poll OS events and update internal key states. */
        virtual void PollEvents() = 0;

        /**
         * @brief Check if a key/button is currently held.
         * @param key The platform key code to query.
         */
        virtual bool IsKeyDown(PlatformKeyCode key) const = 0;

        /**
         * @brief Get the analog value of a key/axis.
         * @param key The platform key code (meaningful for triggers/sticks).
         * @return Value in range [-1, 1] for axes, [0, 1] for triggers, 0/1 for buttons.
         */
        virtual float GetAnalogValue(PlatformKeyCode key) const = 0;

        /**
         * @brief Get the current mouse cursor position.
         * @param x Output: horizontal position in pixels.
         * @param y Output: vertical position in pixels.
         */
        virtual void GetMousePosition(float& x, float& y) const = 0;

        /**
         * @brief Get mouse movement since last poll.
         * @param dx Output: horizontal delta in pixels.
         * @param dy Output: vertical delta in pixels.
         */
        virtual void GetMouseDelta(float& dx, float& dy) const = 0;

        /**
         * @brief Show or hide the OS cursor.
         * @param visible true to show, false to hide.
         */
        virtual void SetCursorVisible(bool visible) = 0;

        /** @brief Human-readable backend name (e.g. "Win32", "SDL2", "Null"). */
        virtual std::string_view GetBackendName() const = 0;
    };

    // ============================================================================
    // Null Backend (headless / testing fallback)
    // ============================================================================

    /**
     * @brief No-op input backend for headless mode and unit tests.
     */
    class NullInputBackend final : public IPlatformInputBackend
    {
      public:
        bool Initialize() override { return true; }
        void Shutdown() override {}
        void PollEvents() override {}
        bool IsKeyDown([[maybe_unused]] PlatformKeyCode key) const override
        {
            return false;
        } // Intentional: null backend
        float GetAnalogValue([[maybe_unused]] PlatformKeyCode key) const override
        {
            return 0.0f;
        } // Intentional: null backend
        void GetMousePosition(float& x, float& y) const override
        {
            x = 0.0f;
            y = 0.0f;
        }
        void GetMouseDelta(float& dx, float& dy) const override
        {
            dx = 0.0f;
            dy = 0.0f;
        }
        void SetCursorVisible([[maybe_unused]] bool visible) override {} // Intentional: null backend
        std::string_view GetBackendName() const override { return "Null"; }
    };

    // ============================================================================
    // Input Action Map
    // ============================================================================

    /**
     * @brief Maps named actions to physical key bindings with frame-edge detection.
     *
     * Call UpdateActions() once per frame after the backend has polled events.
     */
    class InputActionMap
    {
      public:
        /**
         * @brief Register a named input action with key bindings.
         * @param name      Unique action name (e.g. "Jump", "Fire").
         * @param primary   Primary key binding.
         * @param secondary Optional secondary key binding.
         */
        void RegisterAction(const std::string& name, PlatformKeyCode primary,
                            PlatformKeyCode secondary = PlatformKeyCode::None)
        {
            InputAction action;
            action.name = name;
            action.primaryKey = primary;
            action.secondaryKey = secondary;
            m_actions[name] = std::move(action);
        }

        /**
         * @brief Update all action states from the current backend state.
         * @param backend The active input backend to query.
         *
         * Must be called once per frame after IPlatformInputBackend::PollEvents().
         */
        void UpdateActions(const IPlatformInputBackend& backend)
        {
            for (auto& [name, action] : m_actions)
            {
                bool wasPressed = action.isPressed;

                bool primaryDown = (action.primaryKey != PlatformKeyCode::None) && backend.IsKeyDown(action.primaryKey);
                bool secondaryDown =
                    (action.secondaryKey != PlatformKeyCode::None) && backend.IsKeyDown(action.secondaryKey);

                action.isPressed = primaryDown || secondaryDown;
                action.isJustPressed = action.isPressed && !wasPressed;
                action.isJustReleased = !action.isPressed && wasPressed;

                // Read analog from the primary key; apply deadzone
                float raw = backend.GetAnalogValue(action.primaryKey);
                action.analogValue = (raw > -action.deadzone && raw < action.deadzone) ? 0.0f : raw;
            }
        }

        /** @brief Check if an action is currently held. */
        bool IsActionPressed(const std::string& name) const
        {
            auto it = m_actions.find(name);
            return it != m_actions.end() && it->second.isPressed;
        }

        /** @brief Check if an action was just pressed this frame. */
        bool IsActionJustPressed(const std::string& name) const
        {
            auto it = m_actions.find(name);
            return it != m_actions.end() && it->second.isJustPressed;
        }

        /**
         * @brief Get the analog value for an action.
         * @return Value with deadzone applied, or 0 if action not found.
         */
        float GetActionAnalog(const std::string& name) const
        {
            auto it = m_actions.find(name);
            return it != m_actions.end() ? it->second.analogValue : 0.0f;
        }

        /**
         * @brief Get a read-only pointer to an action.
         * @return Pointer to the InputAction, or nullptr if not found.
         */
        const InputAction* GetAction(const std::string& name) const
        {
            auto it = m_actions.find(name);
            return it != m_actions.end() ? &it->second : nullptr;
        }

        /** @brief Remove a registered action by name. */
        void RemoveAction(const std::string& name) { m_actions.erase(name); }

        /** @brief Get all registered actions. */
        const std::unordered_map<std::string, InputAction>& GetAllActions() const { return m_actions; }

        /**
         * @brief Serialize all action bindings to a JSON string.
         * @return JSON representation of the action map.
         */
        std::string SaveToConfig() const
        {
            std::string json = "{\n";
            bool first = true;
            for (const auto& [name, action] : m_actions)
            {
                if (!first)
                    json += ",\n";
                first = false;
                json += "  \"" + name + "\": { ";
                json += "\"primary\": " + std::to_string(static_cast<uint16_t>(action.primaryKey));
                json += ", \"secondary\": " + std::to_string(static_cast<uint16_t>(action.secondaryKey));
                json += ", \"deadzone\": " + std::to_string(action.deadzone);
                json += " }";
            }
            json += "\n}";
            return json;
        }

        /**
         * @brief Deserialize action bindings from a JSON string.
         * @param json JSON string previously produced by SaveToConfig().
         *
         * Minimal parser — expects the exact format produced by SaveToConfig().
         * Existing actions not present in the JSON are preserved.
         */
        void LoadFromConfig(std::string_view json)
        {
            // Lightweight parse: scan for "name": { "primary": N, "secondary": N, "deadzone": F }
            size_t pos = 0;
            while (pos < json.size())
            {
                // Find next action name
                auto nameStart = json.find('"', pos);
                if (nameStart == std::string_view::npos)
                    break;
                nameStart++;
                auto nameEnd = json.find('"', nameStart);
                if (nameEnd == std::string_view::npos)
                    break;
                std::string name(json.substr(nameStart, nameEnd - nameStart));

                // Find "primary":
                auto primaryPos = json.find("\"primary\":", nameEnd);
                if (primaryPos == std::string_view::npos)
                    break;
                primaryPos += 10; // skip past "primary":
                while (primaryPos < json.size() && json[primaryPos] == ' ')
                    primaryPos++;
                auto primaryEnd = json.find_first_of(",}", primaryPos);
                uint16_t primaryVal =
                    static_cast<uint16_t>(std::stoi(std::string(json.substr(primaryPos, primaryEnd - primaryPos))));

                // Find "secondary":
                auto secondaryPos = json.find("\"secondary\":", primaryEnd);
                if (secondaryPos == std::string_view::npos)
                    break;
                secondaryPos += 12;
                while (secondaryPos < json.size() && json[secondaryPos] == ' ')
                    secondaryPos++;
                auto secondaryEnd = json.find_first_of(",}", secondaryPos);
                uint16_t secondaryVal = static_cast<uint16_t>(
                    std::stoi(std::string(json.substr(secondaryPos, secondaryEnd - secondaryPos))));

                // Find "deadzone":
                auto dzPos = json.find("\"deadzone\":", secondaryEnd);
                float dzVal = 0.15f;
                if (dzPos != std::string_view::npos && dzPos < json.find('}', secondaryEnd) + 1)
                {
                    dzPos += 11;
                    while (dzPos < json.size() && json[dzPos] == ' ')
                        dzPos++;
                    auto dzEnd = json.find_first_of(",} ", dzPos);
                    dzVal = std::stof(std::string(json.substr(dzPos, dzEnd - dzPos)));
                }

                InputAction action;
                action.name = name;
                action.primaryKey = static_cast<PlatformKeyCode>(primaryVal);
                action.secondaryKey = static_cast<PlatformKeyCode>(secondaryVal);
                action.deadzone = dzVal;
                m_actions[name] = std::move(action);

                // Advance past this action's closing brace
                pos = json.find('}', secondaryEnd);
                if (pos == std::string_view::npos)
                    break;
                pos++;
            }
        }

      private:
        std::unordered_map<std::string, InputAction> m_actions;
    };

    // ============================================================================
    // Platform Input Factory
    // ============================================================================

    /**
     * @brief Factory that creates the appropriate input backend for the current platform.
     */
    class PlatformInputFactory
    {
      public:
        /**
         * @brief Create a platform-appropriate input backend.
         * @return A unique_ptr to the backend (Win32 on Windows, SDL2 on Linux, Null if unavailable).
         *
         * Falls back to NullInputBackend when no native backend is compiled in.
         */
        static std::unique_ptr<IPlatformInputBackend> CreateBackend()
        {
            // Platform selection at compile time — backends are defined in their own .cpp files.
            // This factory returns the null backend; platform-specific .cpp files can override
            // via a registration pattern or by replacing this default.
            return std::make_unique<NullInputBackend>();
        }

        PlatformInputFactory() = delete;
    };

} // namespace Spark::Input
