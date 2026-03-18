/**
 * @file PlatformInputWin32.cpp
 * @brief Win32 + XInput input backend implementation
 *
 * Implements the Win32InputBackend using Win32 raw input messages
 * and XInput for gamepad support.
 */

#include "../Core/Platform.h"

#ifdef _WIN32

#include "PlatformInput.h"
#include "../Utils/Validate.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <Windows.h>
#include <Xinput.h>
#endif // SPARK_PLATFORM_WINDOWS
#pragma comment(lib, "xinput.lib")

namespace Spark::Input
{

    bool Win32InputBackend::Initialize(void* windowHandle)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Input);
        SPARK_VALIDATE_NOT_NULL_RET(Spark::LogCategory::Input, windowHandle, false);
        m_windowHandle = windowHandle;
        m_keyStates.fill(false);
        m_prevKeyStates.fill(false);
        SPARK_LOG_INFO(Spark::LogCategory::Input, "Win32InputBackend initialized");
        return true;
    }

    void Win32InputBackend::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Input, "Win32InputBackend shutting down");
        if (m_mouseCaptured)
        {
            SetMouseCapture(false);
        }
    }

    void Win32InputBackend::PollEvents()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Input);
        // Save previous states
        m_prevKeyStates = m_keyStates;
        for (auto& gp : m_gamepads)
        {
            gp.prevButtons = gp.buttons;
        }

        // Reset per-frame values
        m_mouseDeltaX = 0;
        m_mouseDeltaY = 0;
        m_mouseScroll = 0.0f;

        // Poll XInput gamepads
        for (int i = 0; i < MAX_GAMEPADS; ++i)
        {
            XINPUT_STATE state{};
            DWORD result = XInputGetState(i, &state);

            m_gamepads[i].connected = (result == ERROR_SUCCESS);
            if (!m_gamepads[i].connected)
                continue;

            m_gamepads[i].name = "XInput Controller " + std::to_string(i);

            // Axes (normalized -1 to 1)
            auto normalize = [](short raw) -> float { return static_cast<float>(raw) / 32767.0f; };
            m_gamepads[i].axes[static_cast<size_t>(GamepadAxis::LeftStickX)] = normalize(state.Gamepad.sThumbLX);
            m_gamepads[i].axes[static_cast<size_t>(GamepadAxis::LeftStickY)] = normalize(state.Gamepad.sThumbLY);
            m_gamepads[i].axes[static_cast<size_t>(GamepadAxis::RightStickX)] = normalize(state.Gamepad.sThumbRX);
            m_gamepads[i].axes[static_cast<size_t>(GamepadAxis::RightStickY)] = normalize(state.Gamepad.sThumbRY);
            m_gamepads[i].axes[static_cast<size_t>(GamepadAxis::LeftTrigger)] = state.Gamepad.bLeftTrigger / 255.0f;
            m_gamepads[i].axes[static_cast<size_t>(GamepadAxis::RightTrigger)] = state.Gamepad.bRightTrigger / 255.0f;

            // Buttons
            auto checkBtn = [&](GamepadBtn btn, WORD xinputFlag)
            { m_gamepads[i].buttons[static_cast<size_t>(btn)] = (state.Gamepad.wButtons & xinputFlag) != 0; };
            checkBtn(GamepadBtn::A, XINPUT_GAMEPAD_A);
            checkBtn(GamepadBtn::B, XINPUT_GAMEPAD_B);
            checkBtn(GamepadBtn::X, XINPUT_GAMEPAD_X);
            checkBtn(GamepadBtn::Y, XINPUT_GAMEPAD_Y);
            checkBtn(GamepadBtn::LeftBumper, XINPUT_GAMEPAD_LEFT_SHOULDER);
            checkBtn(GamepadBtn::RightBumper, XINPUT_GAMEPAD_RIGHT_SHOULDER);
            checkBtn(GamepadBtn::Back, XINPUT_GAMEPAD_BACK);
            checkBtn(GamepadBtn::Start, XINPUT_GAMEPAD_START);
            checkBtn(GamepadBtn::LeftStick, XINPUT_GAMEPAD_LEFT_THUMB);
            checkBtn(GamepadBtn::RightStick, XINPUT_GAMEPAD_RIGHT_THUMB);
            checkBtn(GamepadBtn::DPadUp, XINPUT_GAMEPAD_DPAD_UP);
            checkBtn(GamepadBtn::DPadDown, XINPUT_GAMEPAD_DPAD_DOWN);
            checkBtn(GamepadBtn::DPadLeft, XINPUT_GAMEPAD_DPAD_LEFT);
            checkBtn(GamepadBtn::DPadRight, XINPUT_GAMEPAD_DPAD_RIGHT);
        }
    }

    bool Win32InputBackend::IsKeyDown(KeyCode key) const
    {
        auto idx = static_cast<size_t>(key);
        if (idx >= m_keyStates.size())
            return false;
        return m_keyStates[idx];
    }

    bool Win32InputBackend::WasKeyPressed(KeyCode key) const
    {
        auto idx = static_cast<size_t>(key);
        if (idx >= m_keyStates.size())
            return false;
        return m_keyStates[idx] && !m_prevKeyStates[idx];
    }

    bool Win32InputBackend::WasKeyReleased(KeyCode key) const
    {
        auto idx = static_cast<size_t>(key);
        if (idx >= m_keyStates.size())
            return false;
        return !m_keyStates[idx] && m_prevKeyStates[idx];
    }

    MousePoint Win32InputBackend::GetMousePosition() const
    {
        return {m_mouseX, m_mouseY};
    }

    MousePoint Win32InputBackend::GetMouseDelta() const
    {
        return {m_mouseDeltaX, m_mouseDeltaY};
    }

    float Win32InputBackend::GetMouseScroll() const
    {
        return m_mouseScroll;
    }

    void Win32InputBackend::SetMouseCapture(bool capture)
    {
        m_mouseCaptured = capture;
        HWND hwnd = static_cast<HWND>(m_windowHandle);
        if (capture)
        {
            SetCapture(hwnd);
            ShowCursor(FALSE);
        }
        else
        {
            ReleaseCapture();
            ShowCursor(TRUE);
        }
    }

    bool Win32InputBackend::IsMouseCaptured() const
    {
        return m_mouseCaptured;
    }

    int Win32InputBackend::GetGamepadCount() const
    {
        int count = 0;
        for (const auto& gp : m_gamepads)
        {
            if (gp.connected)
                count++;
        }
        return count;
    }

    const GamepadState* Win32InputBackend::GetGamepadState(int index) const
    {
        if (index < 0 || index >= MAX_GAMEPADS)
            return nullptr;
        return &m_gamepads[index];
    }

    void Win32InputBackend::SetVibration(int gamepadIndex, float leftMotor, float rightMotor, float /*duration*/)
    {
        if (gamepadIndex < 0 || gamepadIndex >= MAX_GAMEPADS)
            return;
        XINPUT_VIBRATION vibration{};
        vibration.wLeftMotorSpeed = static_cast<WORD>(leftMotor * 65535.0f);
        vibration.wRightMotorSpeed = static_cast<WORD>(rightMotor * 65535.0f);
        XInputSetState(gamepadIndex, &vibration);
    }

    void Win32InputBackend::HandleWindowMessage(uint32_t message, uintptr_t wParam, intptr_t lParam)
    {
        switch (message)
        {
        case WM_KEYDOWN:
        {
            KeyCode kc = VirtualKeyToKeyCode(static_cast<int>(wParam));
            if (kc != KeyCode::Unknown)
            {
                m_keyStates[static_cast<size_t>(kc)] = true;
            }
            break;
        }
        case WM_KEYUP:
        {
            KeyCode kc = VirtualKeyToKeyCode(static_cast<int>(wParam));
            if (kc != KeyCode::Unknown)
            {
                m_keyStates[static_cast<size_t>(kc)] = false;
            }
            break;
        }
        case WM_LBUTTONDOWN:
            m_keyStates[static_cast<size_t>(KeyCode::MouseLeft)] = true;
            break;
        case WM_LBUTTONUP:
            m_keyStates[static_cast<size_t>(KeyCode::MouseLeft)] = false;
            break;
        case WM_RBUTTONDOWN:
            m_keyStates[static_cast<size_t>(KeyCode::MouseRight)] = true;
            break;
        case WM_RBUTTONUP:
            m_keyStates[static_cast<size_t>(KeyCode::MouseRight)] = false;
            break;
        case WM_MBUTTONDOWN:
            m_keyStates[static_cast<size_t>(KeyCode::MouseMiddle)] = true;
            break;
        case WM_MBUTTONUP:
            m_keyStates[static_cast<size_t>(KeyCode::MouseMiddle)] = false;
            break;
        case WM_MOUSEMOVE:
        {
            int newX = static_cast<int>(lParam & 0xFFFF);
            int newY = static_cast<int>((lParam >> 16) & 0xFFFF);
            m_mouseDeltaX = newX - m_mouseX;
            m_mouseDeltaY = newY - m_mouseY;
            m_mouseX = newX;
            m_mouseY = newY;
            break;
        }
        case WM_MOUSEWHEEL:
            m_mouseScroll = static_cast<float>(static_cast<short>(HIWORD(wParam))) / 120.0f;
            break;
        }
    }

    KeyCode Win32InputBackend::VirtualKeyToKeyCode(int vk) const
    {
        // Letters and numbers map directly
        if (vk >= 'A' && vk <= 'Z')
            return static_cast<KeyCode>(vk);
        if (vk >= '0' && vk <= '9')
            return static_cast<KeyCode>(vk);

        switch (vk)
        {
        case VK_F1:
            return KeyCode::F1;
        case VK_F2:
            return KeyCode::F2;
        case VK_F3:
            return KeyCode::F3;
        case VK_F4:
            return KeyCode::F4;
        case VK_F5:
            return KeyCode::F5;
        case VK_F6:
            return KeyCode::F6;
        case VK_F7:
            return KeyCode::F7;
        case VK_F8:
            return KeyCode::F8;
        case VK_F9:
            return KeyCode::F9;
        case VK_F10:
            return KeyCode::F10;
        case VK_F11:
            return KeyCode::F11;
        case VK_F12:
            return KeyCode::F12;
        case VK_ESCAPE:
            return KeyCode::Escape;
        case VK_RETURN:
            return KeyCode::Enter;
        case VK_TAB:
            return KeyCode::Tab;
        case VK_BACK:
            return KeyCode::Backspace;
        case VK_INSERT:
            return KeyCode::Insert;
        case VK_DELETE:
            return KeyCode::Delete;
        case VK_RIGHT:
            return KeyCode::Right;
        case VK_LEFT:
            return KeyCode::Left;
        case VK_DOWN:
            return KeyCode::Down;
        case VK_UP:
            return KeyCode::Up;
        case VK_PRIOR:
            return KeyCode::PageUp;
        case VK_NEXT:
            return KeyCode::PageDown;
        case VK_HOME:
            return KeyCode::Home;
        case VK_END:
            return KeyCode::End;
        case VK_LSHIFT:
            return KeyCode::LeftShift;
        case VK_RSHIFT:
            return KeyCode::RightShift;
        case VK_LCONTROL:
            return KeyCode::LeftCtrl;
        case VK_RCONTROL:
            return KeyCode::RightCtrl;
        case VK_LMENU:
            return KeyCode::LeftAlt;
        case VK_RMENU:
            return KeyCode::RightAlt;
        case VK_SPACE:
            return KeyCode::Space;
        case VK_NUMPAD0:
            return KeyCode::Numpad0;
        case VK_NUMPAD1:
            return KeyCode::Numpad1;
        case VK_NUMPAD2:
            return KeyCode::Numpad2;
        case VK_NUMPAD3:
            return KeyCode::Numpad3;
        case VK_NUMPAD4:
            return KeyCode::Numpad4;
        case VK_NUMPAD5:
            return KeyCode::Numpad5;
        case VK_NUMPAD6:
            return KeyCode::Numpad6;
        case VK_NUMPAD7:
            return KeyCode::Numpad7;
        case VK_NUMPAD8:
            return KeyCode::Numpad8;
        case VK_NUMPAD9:
            return KeyCode::Numpad9;
        case VK_DECIMAL:
            return KeyCode::NumpadDecimal;
        case VK_DIVIDE:
            return KeyCode::NumpadDivide;
        case VK_MULTIPLY:
            return KeyCode::NumpadMultiply;
        case VK_SUBTRACT:
            return KeyCode::NumpadSubtract;
        case VK_ADD:
            return KeyCode::NumpadAdd;
        default:
            return KeyCode::Unknown;
        }
    }

} // namespace Spark::Input

#endif // _WIN32
