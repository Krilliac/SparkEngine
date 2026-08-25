/**
 * @file MMOLoginUI.h
 * @brief ImGui-based login, character select, and character creation screens
 * @author Spark Engine Team
 * @date 2026
 *
 * Manages the client-side UI flow before entering the game world:
 * 1. Login/Register screen (username + password)
 * 2. Character Select screen (list existing characters, enter world or delete)
 * 3. Character Creation screen (name, race, class, appearance)
 *
 * The UI state machine drives which panel is visible. Once a character is
 * selected and "Enter World" is pressed, the module transitions to gameplay.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Utils/SecureMemory.h"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace MMO
{

    class MMOAccountSystem;
    class MMOCharacterSystem;

    /// @brief UI flow state machine
    enum class LoginUIState : uint8_t
    {
        Login,           ///< Login/register form
        CharacterSelect, ///< Character list
        CharacterCreate, ///< Character creation form
        EnteringWorld,   ///< Transition to gameplay
        InGame           ///< UI hidden, game is running
    };

    /// @brief Callback when a character enters the world
    using EnterWorldCallback = std::function<void(uint32_t accountId, uint32_t characterId)>;

    /**
     * @brief Login, character select, and character creation UI panels
     */
    class MMOLoginUI
    {
      public:
        MMOLoginUI() = default;
        ~MMOLoginUI() = default;

        bool Initialize(Spark::IEngineContext* context, MMOAccountSystem* accountSys, MMOCharacterSystem* charSys);
        void Update(float deltaTime);
        void Shutdown();
        void RenderUI();

        void SetEnterWorldCallback(EnterWorldCallback cb) { m_enterWorldCallback = std::move(cb); }

        LoginUIState GetState() const { return m_state; }
        void SetState(LoginUIState state)
        {
            m_loginPassword.Clear();
            m_state = state;
            m_enterWorldTimer = state == LoginUIState::EnteringWorld ? ENTER_WORLD_DELAY : 0.0f;
        }
        void ReturnToCharacterSelect();
        void ReturnToLogin();

      private:
        void RenderLoginScreen();
        void RenderCharacterSelectScreen();
        void RenderCharacterCreateScreen();
        void RenderEnteringWorldScreen();

        Spark::IEngineContext* m_context{nullptr};
        MMOAccountSystem* m_accountSys{nullptr};
        MMOCharacterSystem* m_charSys{nullptr};
        EnterWorldCallback m_enterWorldCallback;

        LoginUIState m_state{LoginUIState::Login};

        // Login form state
        char m_loginUsername[64]{};
        Spark::SensitiveCharBuffer<64> m_loginPassword;
        bool m_isRegistering{false};
        char m_registerEmail[128]{};
        std::string m_loginError;
        std::string m_loginSuccess;

        // Session data
        std::string m_sessionToken;
        uint32_t m_loggedInAccountId{0};

        // Character select state
        int m_selectedCharIndex{-1};
        bool m_confirmDelete{false};
        float m_enterWorldTimer{0.0f};

        // Character creation state
        char m_createName[32]{};
        int m_selectedRace{0};
        int m_selectedClass{0};
        int m_faceIndex{0};
        int m_hairIndex{0};
        int m_hairColorIndex{0};
        int m_skinColorIndex{0};
        int m_bodyTypeIndex{0};
        int m_markingIndex{0};
        std::string m_createError;

        static constexpr float ENTER_WORLD_DELAY = 2.0f;
    };

} // namespace MMO
