/**
 * @file PlatformInputSDL2.cpp
 * @brief SDL2 input backend implementation
 *
 * Implements the SDL2InputBackend using SDL2 event polling and
 * SDL_GameController for gamepad support. Used on Linux and optionally
 * on other platforms when SPARK_SDL2_AVAILABLE is defined.
 */

#include "../Core/Platform.h"

#ifdef SPARK_SDL2_AVAILABLE

#include "PlatformInput.h"
#include "../Utils/Validate.h"
#include <SDL.h>

namespace Spark::Input
{

    bool SDL2InputBackend::Initialize(void* windowHandle)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Input);
        if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC) < 0)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Input, "SDL2 initialization failed");
            return false;
        }
        m_window = windowHandle;
        m_keyStates.fill(false);
        m_prevKeyStates.fill(false);
        m_sdlControllers.fill(nullptr);

        // Open any connected game controllers
        for (int i = 0; i < SDL_NumJoysticks() && i < MAX_GAMEPADS; ++i)
        {
            if (SDL_IsGameController(i))
            {
                m_sdlControllers[i] = SDL_GameControllerOpen(i);
                m_gamepads[i].connected = (m_sdlControllers[i] != nullptr);
                if (m_gamepads[i].connected)
                {
                    m_gamepads[i].name = SDL_GameControllerName(static_cast<SDL_GameController*>(m_sdlControllers[i]));
                }
            }
        }
        return true;
    }

    void SDL2InputBackend::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Input, "SDL2InputBackend shutting down");
        for (int i = 0; i < MAX_GAMEPADS; ++i)
        {
            if (m_sdlControllers[i])
            {
                SDL_GameControllerClose(static_cast<SDL_GameController*>(m_sdlControllers[i]));
                m_sdlControllers[i] = nullptr;
            }
        }
        SDL_Quit();
    }

    void SDL2InputBackend::PollEvents()
    {
        m_prevKeyStates = m_keyStates;
        for (auto& gp : m_gamepads)
            gp.prevButtons = gp.buttons;

        m_mouseDeltaX = 0;
        m_mouseDeltaY = 0;
        m_mouseScroll = 0.0f;

        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            switch (event.type)
            {
            case SDL_KEYDOWN:
            {
                KeyCode kc = SDLKeyToKeyCode(event.key.keysym.sym);
                if (kc != KeyCode::Unknown)
                    m_keyStates[static_cast<size_t>(kc)] = true;
                break;
            }
            case SDL_KEYUP:
            {
                KeyCode kc = SDLKeyToKeyCode(event.key.keysym.sym);
                if (kc != KeyCode::Unknown)
                    m_keyStates[static_cast<size_t>(kc)] = false;
                break;
            }
            case SDL_MOUSEMOTION:
                m_mouseX = event.motion.x;
                m_mouseY = event.motion.y;
                m_mouseDeltaX = event.motion.xrel;
                m_mouseDeltaY = event.motion.yrel;
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (event.button.button == SDL_BUTTON_LEFT)
                    m_keyStates[static_cast<size_t>(KeyCode::MouseLeft)] = true;
                else if (event.button.button == SDL_BUTTON_RIGHT)
                    m_keyStates[static_cast<size_t>(KeyCode::MouseRight)] = true;
                else if (event.button.button == SDL_BUTTON_MIDDLE)
                    m_keyStates[static_cast<size_t>(KeyCode::MouseMiddle)] = true;
                break;
            case SDL_MOUSEBUTTONUP:
                if (event.button.button == SDL_BUTTON_LEFT)
                    m_keyStates[static_cast<size_t>(KeyCode::MouseLeft)] = false;
                else if (event.button.button == SDL_BUTTON_RIGHT)
                    m_keyStates[static_cast<size_t>(KeyCode::MouseRight)] = false;
                else if (event.button.button == SDL_BUTTON_MIDDLE)
                    m_keyStates[static_cast<size_t>(KeyCode::MouseMiddle)] = false;
                break;
            case SDL_MOUSEWHEEL:
                m_mouseScroll = static_cast<float>(event.wheel.y);
                break;
            case SDL_CONTROLLERDEVICEADDED:
            {
                int idx = event.cdevice.which;
                if (idx < MAX_GAMEPADS)
                {
                    m_sdlControllers[idx] = SDL_GameControllerOpen(idx);
                    m_gamepads[idx].connected = (m_sdlControllers[idx] != nullptr);
                }
                break;
            }
            case SDL_CONTROLLERDEVICEREMOVED:
            {
                for (int i = 0; i < MAX_GAMEPADS; ++i)
                {
                    if (m_sdlControllers[i] &&
                        !SDL_GameControllerGetAttached(static_cast<SDL_GameController*>(m_sdlControllers[i])))
                    {
                        SDL_GameControllerClose(static_cast<SDL_GameController*>(m_sdlControllers[i]));
                        m_sdlControllers[i] = nullptr;
                        m_gamepads[i].connected = false;
                    }
                }
                break;
            }
            }
        }

        // Update gamepad axes and buttons
        for (int i = 0; i < MAX_GAMEPADS; ++i)
        {
            if (!m_sdlControllers[i])
                continue;
            auto* ctrl = static_cast<SDL_GameController*>(m_sdlControllers[i]);

            auto readAxis = [&](SDL_GameControllerAxis sdlAxis) -> float
            { return static_cast<float>(SDL_GameControllerGetAxis(ctrl, sdlAxis)) / 32767.0f; };

            m_gamepads[i].axes[static_cast<size_t>(GamepadAxis::LeftStickX)] = readAxis(SDL_CONTROLLER_AXIS_LEFTX);
            m_gamepads[i].axes[static_cast<size_t>(GamepadAxis::LeftStickY)] = readAxis(SDL_CONTROLLER_AXIS_LEFTY);
            m_gamepads[i].axes[static_cast<size_t>(GamepadAxis::RightStickX)] = readAxis(SDL_CONTROLLER_AXIS_RIGHTX);
            m_gamepads[i].axes[static_cast<size_t>(GamepadAxis::RightStickY)] = readAxis(SDL_CONTROLLER_AXIS_RIGHTY);
            m_gamepads[i].axes[static_cast<size_t>(GamepadAxis::LeftTrigger)] =
                readAxis(SDL_CONTROLLER_AXIS_TRIGGERLEFT);
            m_gamepads[i].axes[static_cast<size_t>(GamepadAxis::RightTrigger)] =
                readAxis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);

            auto readBtn = [&](SDL_GameControllerButton sdlBtn) -> bool
            { return SDL_GameControllerGetButton(ctrl, sdlBtn) != 0; };

            m_gamepads[i].buttons[static_cast<size_t>(GamepadBtn::A)] = readBtn(SDL_CONTROLLER_BUTTON_A);
            m_gamepads[i].buttons[static_cast<size_t>(GamepadBtn::B)] = readBtn(SDL_CONTROLLER_BUTTON_B);
            m_gamepads[i].buttons[static_cast<size_t>(GamepadBtn::X)] = readBtn(SDL_CONTROLLER_BUTTON_X);
            m_gamepads[i].buttons[static_cast<size_t>(GamepadBtn::Y)] = readBtn(SDL_CONTROLLER_BUTTON_Y);
            m_gamepads[i].buttons[static_cast<size_t>(GamepadBtn::LeftBumper)] =
                readBtn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
            m_gamepads[i].buttons[static_cast<size_t>(GamepadBtn::RightBumper)] =
                readBtn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
            m_gamepads[i].buttons[static_cast<size_t>(GamepadBtn::Back)] = readBtn(SDL_CONTROLLER_BUTTON_BACK);
            m_gamepads[i].buttons[static_cast<size_t>(GamepadBtn::Start)] = readBtn(SDL_CONTROLLER_BUTTON_START);
            m_gamepads[i].buttons[static_cast<size_t>(GamepadBtn::Guide)] = readBtn(SDL_CONTROLLER_BUTTON_GUIDE);
            m_gamepads[i].buttons[static_cast<size_t>(GamepadBtn::LeftStick)] =
                readBtn(SDL_CONTROLLER_BUTTON_LEFTSTICK);
            m_gamepads[i].buttons[static_cast<size_t>(GamepadBtn::RightStick)] =
                readBtn(SDL_CONTROLLER_BUTTON_RIGHTSTICK);
            m_gamepads[i].buttons[static_cast<size_t>(GamepadBtn::DPadUp)] = readBtn(SDL_CONTROLLER_BUTTON_DPAD_UP);
            m_gamepads[i].buttons[static_cast<size_t>(GamepadBtn::DPadDown)] = readBtn(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
            m_gamepads[i].buttons[static_cast<size_t>(GamepadBtn::DPadLeft)] = readBtn(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
            m_gamepads[i].buttons[static_cast<size_t>(GamepadBtn::DPadRight)] =
                readBtn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        }
    }

    bool SDL2InputBackend::IsKeyDown(KeyCode key) const
    {
        auto idx = static_cast<size_t>(key);
        return (idx < m_keyStates.size()) ? m_keyStates[idx] : false;
    }

    bool SDL2InputBackend::WasKeyPressed(KeyCode key) const
    {
        auto idx = static_cast<size_t>(key);
        return (idx < m_keyStates.size()) ? (m_keyStates[idx] && !m_prevKeyStates[idx]) : false;
    }

    bool SDL2InputBackend::WasKeyReleased(KeyCode key) const
    {
        auto idx = static_cast<size_t>(key);
        return (idx < m_keyStates.size()) ? (!m_keyStates[idx] && m_prevKeyStates[idx]) : false;
    }

    MousePoint SDL2InputBackend::GetMousePosition() const
    {
        return {m_mouseX, m_mouseY};
    }

    MousePoint SDL2InputBackend::GetMouseDelta() const
    {
        return {m_mouseDeltaX, m_mouseDeltaY};
    }

    float SDL2InputBackend::GetMouseScroll() const
    {
        return m_mouseScroll;
    }

    void SDL2InputBackend::SetMouseCapture(bool capture)
    {
        m_mouseCaptured = capture;
        SDL_SetRelativeMouseMode(capture ? SDL_TRUE : SDL_FALSE);
    }

    bool SDL2InputBackend::IsMouseCaptured() const
    {
        return m_mouseCaptured;
    }

    int SDL2InputBackend::GetGamepadCount() const
    {
        int count = 0;
        for (const auto& gp : m_gamepads)
            if (gp.connected)
                count++;
        return count;
    }

    const GamepadState* SDL2InputBackend::GetGamepadState(int index) const
    {
        if (index < 0 || index >= MAX_GAMEPADS)
            return nullptr;
        return &m_gamepads[index];
    }

    void SDL2InputBackend::SetVibration(int gamepadIndex, float leftMotor, float rightMotor, float /*duration*/)
    {
        if (gamepadIndex < 0 || gamepadIndex >= MAX_GAMEPADS || !m_sdlControllers[gamepadIndex])
            return;
        SDL_GameControllerRumble(static_cast<SDL_GameController*>(m_sdlControllers[gamepadIndex]),
                                 static_cast<Uint16>(leftMotor * 65535.0f), static_cast<Uint16>(rightMotor * 65535.0f),
                                 100 // duration in ms
        );
    }

    KeyCode SDL2InputBackend::SDLKeyToKeyCode(int sdlKey) const
    {
        if (sdlKey >= SDLK_a && sdlKey <= SDLK_z)
            return static_cast<KeyCode>('A' + (sdlKey - SDLK_a));
        if (sdlKey >= SDLK_0 && sdlKey <= SDLK_9)
            return static_cast<KeyCode>('0' + (sdlKey - SDLK_0));

        switch (sdlKey)
        {
        case SDLK_F1:
            return KeyCode::F1;
        case SDLK_F2:
            return KeyCode::F2;
        case SDLK_F3:
            return KeyCode::F3;
        case SDLK_F4:
            return KeyCode::F4;
        case SDLK_F5:
            return KeyCode::F5;
        case SDLK_F6:
            return KeyCode::F6;
        case SDLK_F7:
            return KeyCode::F7;
        case SDLK_F8:
            return KeyCode::F8;
        case SDLK_F9:
            return KeyCode::F9;
        case SDLK_F10:
            return KeyCode::F10;
        case SDLK_F11:
            return KeyCode::F11;
        case SDLK_F12:
            return KeyCode::F12;
        case SDLK_ESCAPE:
            return KeyCode::Escape;
        case SDLK_RETURN:
            return KeyCode::Enter;
        case SDLK_TAB:
            return KeyCode::Tab;
        case SDLK_BACKSPACE:
            return KeyCode::Backspace;
        case SDLK_INSERT:
            return KeyCode::Insert;
        case SDLK_DELETE:
            return KeyCode::Delete;
        case SDLK_RIGHT:
            return KeyCode::Right;
        case SDLK_LEFT:
            return KeyCode::Left;
        case SDLK_DOWN:
            return KeyCode::Down;
        case SDLK_UP:
            return KeyCode::Up;
        case SDLK_PAGEUP:
            return KeyCode::PageUp;
        case SDLK_PAGEDOWN:
            return KeyCode::PageDown;
        case SDLK_HOME:
            return KeyCode::Home;
        case SDLK_END:
            return KeyCode::End;
        case SDLK_LSHIFT:
            return KeyCode::LeftShift;
        case SDLK_RSHIFT:
            return KeyCode::RightShift;
        case SDLK_LCTRL:
            return KeyCode::LeftCtrl;
        case SDLK_RCTRL:
            return KeyCode::RightCtrl;
        case SDLK_LALT:
            return KeyCode::LeftAlt;
        case SDLK_RALT:
            return KeyCode::RightAlt;
        case SDLK_SPACE:
            return KeyCode::Space;
        default:
            return KeyCode::Unknown;
        }
    }

} // namespace Spark::Input

#endif // SPARK_SDL2_AVAILABLE
