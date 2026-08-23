/**
 * @file MMOLoginUI.cpp
 * @brief Login, character select, and character creation ImGui panels
 */

#include "MMOLoginUI.h"
#include "Account/MMOAccountSystem.h"
#include "Character/MMOCharacterSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <cstring>

namespace MMO
{

    bool MMOLoginUI::Initialize(Spark::IEngineContext* context, MMOAccountSystem* accountSys,
                                MMOCharacterSystem* charSys)
    {
        m_context = context;
        m_accountSys = accountSys;
        m_charSys = charSys;
        m_state = LoginUIState::Login;
        std::memset(m_loginUsername, 0, sizeof(m_loginUsername));
        std::memset(m_loginPassword, 0, sizeof(m_loginPassword));
        std::memset(m_registerEmail, 0, sizeof(m_registerEmail));
        std::memset(m_createName, 0, sizeof(m_createName));
        SPARK_LOG_INFO(Spark::LogCategory::Game, "MMO Login UI initialized");
        Spark::SimpleConsole::GetInstance().LogInfo("[MMO] Login UI initialized");
        return true;
    }

    void MMOLoginUI::Shutdown()
    {
        m_accountSys = nullptr;
        m_charSys = nullptr;
    }

    void MMOLoginUI::ReturnToCharacterSelect()
    {
        m_state = LoginUIState::CharacterSelect;
        m_selectedCharIndex = -1;
        m_confirmDelete = false;
    }

    void MMOLoginUI::ReturnToLogin()
    {
        SPARK_LOG_DEBUG(Spark::LogCategory::Game, "Login UI: returning to login screen");
        if (!m_sessionToken.empty() && m_accountSys)
            m_accountSys->Logout(m_sessionToken);
        m_sessionToken.clear();
        m_loggedInAccountId = 0;
        m_state = LoginUIState::Login;
        m_loginError.clear();
        m_loginSuccess.clear();
    }

    void MMOLoginUI::RenderUI()
    {
#ifdef ENABLE_EDITOR
        switch (m_state)
        {
        case LoginUIState::Login:
            RenderLoginScreen();
            break;
        case LoginUIState::CharacterSelect:
            RenderCharacterSelectScreen();
            break;
        case LoginUIState::CharacterCreate:
            RenderCharacterCreateScreen();
            break;
        case LoginUIState::EnteringWorld:
            RenderEnteringWorldScreen();
            break;
        case LoginUIState::InGame:
            break;
        }
#endif
    }

    void MMOLoginUI::RenderLoginScreen()
    {
#ifdef ENABLE_EDITOR
        ImGui::SetNextWindowSize(ImVec2(400, 320), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));

        ImGui::Begin("Spark MMO - Login", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize);

        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Welcome to Spark MMO");
        ImGui::Separator();
        ImGui::Spacing();

        if (m_isRegistering)
        {
            ImGui::Text("Create a New Account");
            ImGui::Spacing();
        }
        else
        {
            ImGui::Text("Sign In");
            ImGui::Spacing();
        }

        ImGui::SetNextItemWidth(250);
        ImGui::InputText("Username", m_loginUsername, sizeof(m_loginUsername));

        ImGui::SetNextItemWidth(250);
        ImGui::InputText("Password", m_loginPassword, sizeof(m_loginPassword), ImGuiInputTextFlags_Password);

        if (m_isRegistering)
        {
            ImGui::SetNextItemWidth(250);
            ImGui::InputText("Email (optional)", m_registerEmail, sizeof(m_registerEmail));
        }

        ImGui::Spacing();

        if (!m_loginError.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_loginError.c_str());
        if (!m_loginSuccess.empty())
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "%s", m_loginSuccess.c_str());

        ImGui::Spacing();

        if (m_isRegistering)
        {
            if (ImGui::Button("Register", ImVec2(120, 30)))
            {
                if (m_accountSys)
                {
                    auto result = m_accountSys->Register(m_loginUsername, m_loginPassword, m_registerEmail);
                    if (result.success)
                    {
                        m_loginSuccess = "Account created! You can now log in.";
                        m_loginError.clear();
                        m_isRegistering = false;
                    }
                    else
                    {
                        m_loginError = result.errorMessage;
                        m_loginSuccess.clear();
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Back to Login", ImVec2(120, 30)))
            {
                m_isRegistering = false;
                m_loginError.clear();
            }
        }
        else
        {
            if (ImGui::Button("Login", ImVec2(120, 30)))
            {
                if (m_accountSys)
                {
                    auto result = m_accountSys->Login(m_loginUsername, m_loginPassword);
                    if (result.success)
                    {
                        SPARK_LOG_INFO(Spark::LogCategory::Game, "Login UI: login successful for %s", m_loginUsername);
                        m_sessionToken = result.sessionToken;
                        m_loggedInAccountId = result.accountId;
                        m_loginError.clear();
                        m_loginSuccess.clear();
                        m_state = LoginUIState::CharacterSelect;
                        m_selectedCharIndex = -1;
                    }
                    else
                    {
                        m_loginError = result.errorMessage;
                        m_loginSuccess.clear();
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Create Account", ImVec2(120, 30)))
            {
                m_isRegistering = true;
                m_loginError.clear();
                m_loginSuccess.clear();
            }
        }

        ImGui::End();
#endif
    }

    void MMOLoginUI::RenderCharacterSelectScreen()
    {
#ifdef ENABLE_EDITOR
        if (!m_charSys || !m_accountSys)
            return;

        ImGui::SetNextWindowSize(ImVec2(500, 450), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));

        ImGui::Begin("Spark MMO - Character Select", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);

        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Select Your Character");
        ImGui::Text("Account: %s", m_loginUsername);
        ImGui::Separator();
        ImGui::Spacing();

        auto characters = m_charSys->GetCharacters(m_loggedInAccountId);

        if (characters.empty())
        {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No characters found. Create one to begin!");
        }
        else
        {
            for (int i = 0; i < static_cast<int>(characters.size()); ++i)
            {
                const auto& ch = characters[i];
                const auto* raceDef = m_charSys->GetRace(ch.race);
                const auto* classDef = m_charSys->GetClass(ch.classId);

                bool selected = (m_selectedCharIndex == i);
                std::string label = ch.name + " - Lv" + std::to_string(ch.level) + " " +
                                    (raceDef ? raceDef->name : "?") + " " + (classDef ? classDef->name : "?");

                if (ImGui::Selectable(label.c_str(), selected, 0, ImVec2(0, 40)))
                {
                    m_selectedCharIndex = i;
                    m_confirmDelete = false;
                }

                if (selected)
                {
                    ImGui::SameLine(ImGui::GetWindowWidth() - 160);
                    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "@ %s", ch.areaName.c_str());
                }
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Action buttons
        bool hasSelection = m_selectedCharIndex >= 0 && m_selectedCharIndex < static_cast<int>(characters.size());

        if (hasSelection)
        {
            if (ImGui::Button("Enter World", ImVec2(130, 35)))
            {
                const auto& ch = characters[m_selectedCharIndex];
                m_accountSys->SetActiveCharacter(m_sessionToken, ch.characterId);
                m_state = LoginUIState::EnteringWorld;
                m_enterWorldTimer = 2.0f;

                if (m_enterWorldCallback)
                    m_enterWorldCallback(m_loggedInAccountId, ch.characterId);

                SPARK_LOG_INFO(Spark::LogCategory::Game, "Login UI: entering world as %s", ch.name.c_str());
                Spark::SimpleConsole::GetInstance().LogInfo("[MMO] Entering world as " + ch.name);
            }
            ImGui::SameLine();
        }

        if (ImGui::Button("Create New", ImVec2(110, 35)))
        {
            m_state = LoginUIState::CharacterCreate;
            std::memset(m_createName, 0, sizeof(m_createName));
            m_selectedRace = 0;
            m_selectedClass = 0;
            m_faceIndex = 0;
            m_hairIndex = 0;
            m_hairColorIndex = 0;
            m_skinColorIndex = 0;
            m_bodyTypeIndex = 0;
            m_markingIndex = 0;
            m_createError.clear();
        }

        if (hasSelection)
        {
            ImGui::SameLine();
            if (!m_confirmDelete)
            {
                if (ImGui::Button("Delete", ImVec2(80, 35)))
                    m_confirmDelete = true;
            }
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
                if (ImGui::Button("Confirm?", ImVec2(80, 35)))
                {
                    const auto& ch = characters[m_selectedCharIndex];
                    m_charSys->DeleteCharacter(m_loggedInAccountId, ch.characterId);
                    m_selectedCharIndex = -1;
                    m_confirmDelete = false;
                }
                ImGui::PopStyleColor();
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(60, 35)))
                    m_confirmDelete = false;
            }
        }

        ImGui::SameLine(ImGui::GetWindowWidth() - 100);
        if (ImGui::Button("Logout", ImVec2(80, 35)))
            ReturnToLogin();

        ImGui::End();
#endif
    }

    void MMOLoginUI::RenderCharacterCreateScreen()
    {
#ifdef ENABLE_EDITOR
        if (!m_charSys)
            return;

        ImGui::SetNextWindowSize(ImVec2(550, 550), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));

        ImGui::Begin("Spark MMO - Character Creation", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);

        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Create Your Character");
        ImGui::Separator();
        ImGui::Spacing();

        // === Name ===
        ImGui::Text("Character Name:");
        ImGui::SetNextItemWidth(250);
        ImGui::InputText("##name", m_createName, sizeof(m_createName));

        ImGui::Spacing();

        // === Race Selection ===
        ImGui::Text("Race:");
        const auto& races = m_charSys->GetAllRaces();
        for (int i = 0; i < static_cast<int>(races.size()); ++i)
        {
            if (i > 0)
                ImGui::SameLine();
            bool selected = (m_selectedRace == i);
            if (selected)
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.6f, 0.9f, 1.0f));
            if (ImGui::Button(races[i].name.c_str(), ImVec2(80, 30)))
            {
                m_selectedRace = i;
                // Reset class if not valid for new race
                const auto& classes = m_charSys->GetAllClasses();
                if (m_selectedClass < static_cast<int>(classes.size()))
                {
                    if (!m_charSys->IsValidCombination(races[i].id, classes[m_selectedClass].id))
                        m_selectedClass = 0;
                }
            }
            if (selected)
                ImGui::PopStyleColor();
        }

        if (m_selectedRace < static_cast<int>(races.size()))
        {
            ImGui::TextWrapped("  %s", races[m_selectedRace].description.c_str());
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "  %s", races[m_selectedRace].lore.c_str());
        }

        ImGui::Spacing();

        // === Class Selection ===
        ImGui::Text("Class:");
        const auto& classes = m_charSys->GetAllClasses();
        int btnCount = 0;
        for (int i = 0; i < static_cast<int>(classes.size()); ++i)
        {
            // Only show allowed classes for selected race
            if (m_selectedRace < static_cast<int>(races.size()))
            {
                if (!m_charSys->IsValidCombination(races[m_selectedRace].id, classes[i].id))
                    continue;
            }
            if (btnCount > 0)
                ImGui::SameLine();
            bool selected = (m_selectedClass == i);
            if (selected)
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.6f, 0.9f, 1.0f));
            if (ImGui::Button(classes[i].name.c_str(), ImVec2(80, 30)))
                m_selectedClass = i;
            if (selected)
                ImGui::PopStyleColor();
            btnCount++;
        }

        if (m_selectedClass < static_cast<int>(classes.size()))
        {
            ImGui::Text("  Role: %s", classes[m_selectedClass].role.c_str());
            ImGui::TextWrapped("  %s", classes[m_selectedClass].description.c_str());
        }

        ImGui::Spacing();

        // === Stat Preview ===
        if (m_selectedRace < static_cast<int>(races.size()) && m_selectedClass < static_cast<int>(classes.size()))
        {
            auto stats = m_charSys->ComputeStats(races[m_selectedRace].id, classes[m_selectedClass].id, 1);
            ImGui::Separator();
            ImGui::Text("Starting Stats (Level 1):");
            ImGui::Columns(4, "stats", false);
            ImGui::Text("HP: %.0f", stats.health);
            ImGui::NextColumn();
            ImGui::Text("MP: %.0f", stats.mana);
            ImGui::NextColumn();
            ImGui::Text("STA: %.0f", stats.stamina);
            ImGui::NextColumn();
            ImGui::Text("SPD: %.1f", stats.moveSpeed);
            ImGui::NextColumn();
            ImGui::Text("STR: %.0f", stats.strength);
            ImGui::NextColumn();
            ImGui::Text("AGI: %.0f", stats.agility);
            ImGui::NextColumn();
            ImGui::Text("INT: %.0f", stats.intellect);
            ImGui::NextColumn();
            ImGui::Text("ARM: %.0f", stats.armor);
            ImGui::Columns(1);
            ImGui::Separator();
        }

        ImGui::Spacing();

        // === Appearance ===
        if (ImGui::TreeNode("Appearance"))
        {
            ImGui::SliderInt("Face", &m_faceIndex, 0, AppearanceOptions::MAX_FACES - 1);
            ImGui::SliderInt("Hair Style", &m_hairIndex, 0, AppearanceOptions::MAX_HAIRS - 1);
            ImGui::SliderInt("Hair Color", &m_hairColorIndex, 0, AppearanceOptions::MAX_HAIR_COLORS - 1);
            ImGui::SliderInt("Skin Color", &m_skinColorIndex, 0, AppearanceOptions::MAX_SKIN_COLORS - 1);
            ImGui::SliderInt("Body Type", &m_bodyTypeIndex, 0, AppearanceOptions::MAX_BODY_TYPES - 1);
            ImGui::SliderInt("Markings", &m_markingIndex, 0, AppearanceOptions::MAX_MARKINGS - 1);
            ImGui::TreePop();
        }

        ImGui::Spacing();

        // === Error ===
        if (!m_createError.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_createError.c_str());

        // === Buttons ===
        if (ImGui::Button("Create Character", ImVec2(140, 35)))
        {
            CharacterCreateRequest req;
            req.name = m_createName;
            if (m_selectedRace < static_cast<int>(races.size()))
                req.race = races[m_selectedRace].id;
            if (m_selectedClass < static_cast<int>(classes.size()))
                req.classId = classes[m_selectedClass].id;
            req.appearance.faceIndex = m_faceIndex;
            req.appearance.hairIndex = m_hairIndex;
            req.appearance.hairColorIndex = m_hairColorIndex;
            req.appearance.skinColorIndex = m_skinColorIndex;
            req.appearance.bodyTypeIndex = m_bodyTypeIndex;
            req.appearance.markingIndex = m_markingIndex;

            auto result = m_charSys->CreateCharacter(m_loggedInAccountId, req);
            if (result.success)
            {
                m_createError.clear();
                ReturnToCharacterSelect();
            }
            else
            {
                m_createError = result.errorMessage;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Back", ImVec2(80, 35)))
            ReturnToCharacterSelect();

        ImGui::End();
#endif
    }

    void MMOLoginUI::RenderEnteringWorldScreen()
    {
#ifdef ENABLE_EDITOR
        ImGui::SetNextWindowSize(ImVec2(300, 100), ImGuiCond_Always);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));

        ImGui::Begin("##entering", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoScrollbar);

        ImGui::Spacing();
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize("Entering World...").x) * 0.5f);
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Entering World...");
        ImGui::Spacing();

        float progress = 1.0f - (m_enterWorldTimer / 2.0f);
        ImGui::ProgressBar(progress, ImVec2(-1, 20));

        ImGui::End();

        m_enterWorldTimer -= ImGui::GetIO().DeltaTime;
        if (m_enterWorldTimer <= 0.0f)
            m_state = LoginUIState::InGame;
#endif
    }

} // namespace MMO
