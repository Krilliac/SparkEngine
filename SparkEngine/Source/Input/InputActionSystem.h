/**
 * @file InputActionSystem.h
 * @brief Data-driven input action system with rebindable controls
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a high-level input abstraction layer on top of InputManager.
 * Actions are named, rebindable at runtime, support composite bindings
 * (e.g. WASD → Vector2), and can be organized into push/pop contexts.
 *
 * ## Usage
 * @code
 *   auto& actions = Spark::Input::InputActionSystem::GetInstance();
 *   actions.Initialize();
 *
 *   // Define actions
 *   actions.RegisterAction("Jump", ActionType::Button);
 *   actions.RegisterAction("Move", ActionType::Axis2D);
 *
 *   // Bind keys
 *   actions.BindKey("Jump", VK_SPACE, InputTrigger::Pressed);
 *   actions.BindComposite2D("Move", VK_W, VK_S, VK_A, VK_D);
 *
 *   // Create contexts
 *   actions.CreateContext("Gameplay");
 *   actions.AddActionToContext("Gameplay", "Jump");
 *   actions.AddActionToContext("Gameplay", "Move");
 *   actions.PushContext("Gameplay");
 *
 *   // Query each frame (after Update)
 *   if (actions.IsActionTriggered("Jump"))
 *       player.Jump();
 *   auto move = actions.GetAxis2D("Move");
 * @endcode
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Spark::Input
{

    /** @brief Type of input action */
    enum class ActionType
    {
        Button, ///< Digital on/off (e.g. jump, fire)
        Axis1D, ///< Single-axis analog (e.g. throttle)
        Axis2D  ///< Two-axis analog (e.g. movement, look)
    };

    /** @brief When a button action triggers */
    enum class InputTrigger
    {
        Pressed,  ///< Fires once on key down
        Released, ///< Fires once on key up
        Held      ///< Fires every frame while held
    };

    /** @brief A 2D axis value */
    struct Axis2DValue
    {
        float x = 0.0f; ///< Horizontal axis (-1 to 1)
        float y = 0.0f; ///< Vertical axis (-1 to 1)
    };

    /** @brief A single key/button binding to an action */
    struct InputBinding
    {
        int keyCode = 0;                              ///< Virtual key code or button code
        InputTrigger trigger = InputTrigger::Pressed; ///< When this binding activates
        float scale = 1.0f;                           ///< Axis scale multiplier
        int modifierKey = 0;                          ///< Required modifier (0 = none)
    };

    /** @brief Composite binding for 2D axes (e.g. WASD) */
    struct CompositeBinding2D
    {
        int positiveY = 0; ///< Key for +Y (forward/up, e.g. W)
        int negativeY = 0; ///< Key for -Y (backward/down, e.g. S)
        int negativeX = 0; ///< Key for -X (left, e.g. A)
        int positiveX = 0; ///< Key for +X (right, e.g. D)
    };

    /** @brief Runtime state of a registered action */
    struct ActionState
    {
        std::string name;                     ///< Action name
        ActionType type = ActionType::Button; ///< Action type
        bool triggered = false;               ///< Whether the action was triggered this frame
        bool active = false;                  ///< Whether the action is currently active (held)
        float axis1D = 0.0f;                  ///< Current 1D axis value
        Axis2DValue axis2D;                   ///< Current 2D axis value
        std::vector<InputBinding> bindings;   ///< Key bindings for this action
        CompositeBinding2D composite{};       ///< Composite 2D binding (if Axis2D)
        bool hasComposite = false;            ///< Whether composite binding is set
    };

    /** @brief An input context grouping related actions */
    struct InputContext
    {
        std::string name;                        ///< Context name
        std::unordered_set<std::string> actions; ///< Actions active in this context
        bool consumeInput = true;                ///< Whether this context blocks lower contexts
    };

    /** @brief Callback for action state queries (used by external key state provider) */
    using KeyStateProvider = std::function<bool(int keyCode)>;
    using KeyPressedProvider = std::function<bool(int keyCode)>;
    using KeyReleasedProvider = std::function<bool(int keyCode)>;

    /**
     * @brief Data-driven input action system with rebindable controls
     *
     * Sits on top of InputManager, providing named actions, composite bindings,
     * runtime rebinding, and context-based activation. InputManager handles raw
     * input; this system interprets it as game-level actions.
     *
     * Thread safety: Not thread-safe. Call from main thread only.
     */
    class InputActionSystem
    {
      public:
        static InputActionSystem& GetInstance()
        {
            static InputActionSystem instance;
            return instance;
        }

        /** @brief Initialize the input action system */
        void Initialize()
        {
            m_actions.clear();
            m_contexts.clear();
            m_contextStack.clear();
            m_initialized = true;
        }

        /** @brief Shut down and clear all actions and contexts */
        void Shutdown()
        {
            m_actions.clear();
            m_contexts.clear();
            m_contextStack.clear();
            m_initialized = false;
        }

        /**
         * @brief Set the key state provider (connects to InputManager)
         * @param isDown Returns true if key is currently down
         * @param wasPressed Returns true if key was pressed this frame
         * @param wasReleased Returns true if key was released this frame
         */
        void SetKeyStateProviders(KeyStateProvider isDown, KeyPressedProvider wasPressed,
                                  KeyReleasedProvider wasReleased)
        {
            m_isKeyDown = std::move(isDown);
            m_wasKeyPressed = std::move(wasPressed);
            m_wasKeyReleased = std::move(wasReleased);
        }

        /**
         * @brief Register a new named action
         * @param name Unique action name
         * @param type Action type (Button, Axis1D, Axis2D)
         * @return true if registered successfully
         */
        bool RegisterAction(const std::string& name, ActionType type)
        {
            if (name.empty() || m_actions.count(name))
                return false;

            ActionState state;
            state.name = name;
            state.type = type;
            m_actions[name] = std::move(state);
            return true;
        }

        /** @brief Unregister an action */
        void UnregisterAction(const std::string& name) { m_actions.erase(name); }

        /**
         * @brief Bind a key to an action
         * @param actionName Action to bind to
         * @param keyCode Virtual key code
         * @param trigger When the binding activates
         * @param modifierKey Optional modifier key (0 = none)
         * @return true if bound successfully
         */
        bool BindKey(const std::string& actionName, int keyCode, InputTrigger trigger = InputTrigger::Pressed,
                     int modifierKey = 0)
        {
            auto it = m_actions.find(actionName);
            if (it == m_actions.end())
                return false;

            InputBinding binding;
            binding.keyCode = keyCode;
            binding.trigger = trigger;
            binding.modifierKey = modifierKey;
            it->second.bindings.push_back(binding);
            return true;
        }

        /**
         * @brief Bind a composite 2D axis (e.g. WASD)
         * @param actionName Action to bind to (must be Axis2D type)
         * @param upKey Key for +Y
         * @param downKey Key for -Y
         * @param leftKey Key for -X
         * @param rightKey Key for +X
         * @return true if bound successfully
         */
        bool BindComposite2D(const std::string& actionName, int upKey, int downKey, int leftKey, int rightKey)
        {
            auto it = m_actions.find(actionName);
            if (it == m_actions.end() || it->second.type != ActionType::Axis2D)
                return false;

            it->second.composite = {upKey, downKey, leftKey, rightKey};
            it->second.hasComposite = true;
            return true;
        }

        /**
         * @brief Clear all bindings for an action
         * @param actionName Action to clear
         */
        void ClearBindings(const std::string& actionName)
        {
            auto it = m_actions.find(actionName);
            if (it != m_actions.end())
            {
                it->second.bindings.clear();
                it->second.hasComposite = false;
                it->second.composite = {};
            }
        }

        /**
         * @brief Rebind a specific key for an action (replaces first binding)
         * @param actionName Action to rebind
         * @param newKeyCode New key code
         * @return true if rebound successfully
         */
        bool Rebind(const std::string& actionName, int newKeyCode)
        {
            auto it = m_actions.find(actionName);
            if (it == m_actions.end() || it->second.bindings.empty())
                return false;

            it->second.bindings[0].keyCode = newKeyCode;
            return true;
        }

        /**
         * @brief Create a named input context
         * @param name Context name
         * @param consumeInput Whether this context blocks lower contexts
         * @return true if created successfully
         */
        bool CreateContext(const std::string& name, bool consumeInput = true)
        {
            if (name.empty() || m_contexts.count(name))
                return false;

            InputContext ctx;
            ctx.name = name;
            ctx.consumeInput = consumeInput;
            m_contexts[name] = std::move(ctx);
            return true;
        }

        /**
         * @brief Add an action to a context
         * @param contextName Context to add to
         * @param actionName Action to include
         * @return true if added successfully
         */
        bool AddActionToContext(const std::string& contextName, const std::string& actionName)
        {
            auto it = m_contexts.find(contextName);
            if (it == m_contexts.end() || !m_actions.count(actionName))
                return false;

            it->second.actions.insert(actionName);
            return true;
        }

        /** @brief Push a context onto the stack (activates it) */
        bool PushContext(const std::string& name)
        {
            if (!m_contexts.count(name))
                return false;
            m_contextStack.push_back(name);
            return true;
        }

        /** @brief Pop the top context from the stack */
        void PopContext()
        {
            if (!m_contextStack.empty())
                m_contextStack.pop_back();
        }

        /** @brief Get the name of the active (top) context */
        std::string GetActiveContext() const { return m_contextStack.empty() ? "" : m_contextStack.back(); }

        /**
         * @brief Update all action states (call once per frame)
         *
         * Evaluates all bindings for actions in the active context stack
         * and updates their triggered/active/axis states.
         */
        void Update()
        {
            if (!m_initialized)
                return;

            // Build set of active actions from context stack
            std::unordered_set<std::string> activeActions;
            for (auto it = m_contextStack.rbegin(); it != m_contextStack.rend(); ++it)
            {
                auto ctxIt = m_contexts.find(*it);
                if (ctxIt == m_contexts.end())
                    continue;

                for (const auto& actionName : ctxIt->second.actions)
                    activeActions.insert(actionName);

                if (ctxIt->second.consumeInput)
                    break; // This context consumes — don't process lower contexts
            }

            // If no contexts, all actions are active
            if (m_contextStack.empty())
            {
                for (const auto& [name, state] : m_actions)
                    activeActions.insert(name);
            }

            // Evaluate each active action
            for (auto& [name, state] : m_actions)
            {
                bool isActive = activeActions.count(name) > 0;

                // Reset per-frame state
                state.triggered = false;
                state.axis1D = 0.0f;
                state.axis2D = {0.0f, 0.0f};

                if (!isActive)
                {
                    state.active = false;
                    continue;
                }

                // Evaluate button bindings
                for (const auto& binding : state.bindings)
                {
                    // Check modifier
                    if (binding.modifierKey != 0 && m_isKeyDown && !m_isKeyDown(binding.modifierKey))
                        continue;

                    switch (binding.trigger)
                    {
                    case InputTrigger::Pressed:
                        if (m_wasKeyPressed && m_wasKeyPressed(binding.keyCode))
                            state.triggered = true;
                        break;
                    case InputTrigger::Released:
                        if (m_wasKeyReleased && m_wasKeyReleased(binding.keyCode))
                            state.triggered = true;
                        break;
                    case InputTrigger::Held:
                        if (m_isKeyDown && m_isKeyDown(binding.keyCode))
                        {
                            state.triggered = true;
                            state.active = true;
                        }
                        break;
                    }

                    // 1D axis from held state
                    if (state.type == ActionType::Axis1D && m_isKeyDown && m_isKeyDown(binding.keyCode))
                    {
                        state.axis1D += binding.scale;
                    }
                }

                // Evaluate composite 2D binding
                if (state.hasComposite && m_isKeyDown)
                {
                    if (m_isKeyDown(state.composite.positiveY))
                        state.axis2D.y += 1.0f;
                    if (m_isKeyDown(state.composite.negativeY))
                        state.axis2D.y -= 1.0f;
                    if (m_isKeyDown(state.composite.positiveX))
                        state.axis2D.x += 1.0f;
                    if (m_isKeyDown(state.composite.negativeX))
                        state.axis2D.x -= 1.0f;

                    // Normalize diagonal movement
                    float len = std::sqrt(state.axis2D.x * state.axis2D.x + state.axis2D.y * state.axis2D.y);
                    if (len > 1.0f)
                    {
                        state.axis2D.x /= len;
                        state.axis2D.y /= len;
                    }

                    if (len > 0.0f)
                        state.active = true;
                }

                // Update active state for button types
                if (state.type == ActionType::Button)
                {
                    bool anyHeld = false;
                    for (const auto& binding : state.bindings)
                    {
                        if (m_isKeyDown && m_isKeyDown(binding.keyCode))
                        {
                            anyHeld = true;
                            break;
                        }
                    }
                    state.active = anyHeld;
                }
            }
        }

        /** @brief Check if a button action was triggered this frame */
        bool IsActionTriggered(const std::string& name) const
        {
            auto it = m_actions.find(name);
            return (it != m_actions.end()) ? it->second.triggered : false;
        }

        /** @brief Check if a button action is currently held */
        bool IsActionActive(const std::string& name) const
        {
            auto it = m_actions.find(name);
            return (it != m_actions.end()) ? it->second.active : false;
        }

        /** @brief Get the current 1D axis value for an action */
        float GetAxis1D(const std::string& name) const
        {
            auto it = m_actions.find(name);
            return (it != m_actions.end()) ? it->second.axis1D : 0.0f;
        }

        /** @brief Get the current 2D axis value for an action */
        Axis2DValue GetAxis2D(const std::string& name) const
        {
            auto it = m_actions.find(name);
            return (it != m_actions.end()) ? it->second.axis2D : Axis2DValue{};
        }

        /** @brief Get the number of registered actions */
        size_t GetActionCount() const { return m_actions.size(); }

        /** @brief Get the number of registered contexts */
        size_t GetContextCount() const { return m_contexts.size(); }

        /** @brief Get all registered action names */
        std::vector<std::string> GetActionNames() const
        {
            std::vector<std::string> names;
            names.reserve(m_actions.size());
            for (const auto& [name, state] : m_actions)
                names.push_back(name);
            return names;
        }

        /** @brief Get the bindings for an action (for UI display / rebind screens) */
        const std::vector<InputBinding>* GetBindings(const std::string& name) const
        {
            auto it = m_actions.find(name);
            return (it != m_actions.end()) ? &it->second.bindings : nullptr;
        }

        /** @brief Get console-friendly status string */
        std::string Console_GetStatus() const
        {
            if (!m_initialized)
                return "[InputActions] Not initialized";

            std::string status = "[InputActions] Actions: " + std::to_string(m_actions.size());
            status += " | Contexts: " + std::to_string(m_contexts.size());
            status += " | Stack depth: " + std::to_string(m_contextStack.size());
            if (!m_contextStack.empty())
                status += " | Active: " + m_contextStack.back();
            return status;
        }

      private:
        InputActionSystem() = default;

        bool m_initialized = false;
        std::unordered_map<std::string, ActionState> m_actions;
        std::unordered_map<std::string, InputContext> m_contexts;
        std::vector<std::string> m_contextStack;

        KeyStateProvider m_isKeyDown;
        KeyPressedProvider m_wasKeyPressed;
        KeyReleasedProvider m_wasKeyReleased;
    };

} // namespace Spark::Input
