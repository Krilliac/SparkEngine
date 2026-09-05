/**
 * @file SaveSystemPanel.cpp
 * @brief Implementation of the save system management panel
 */

#include "SaveSystemPanel.h"
#include "../Core/EditorIcons.h"
#include "Core/EngineContext.h"
#include "Engine/SaveSystem/SaveSystem.h"
#include "Utils/LogMacros.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <imgui.h>

namespace SparkEditor
{

    namespace
    {
        /// Format a unix timestamp as local "YYYY-MM-DD HH:MM"; empty for an unset stamp.
        std::string FormatTimestamp(uint64_t unixSeconds)
        {
            if (unixSeconds == 0)
            {
                return "-";
            }

            const std::time_t raw = static_cast<std::time_t>(unixSeconds);
            std::tm parts{};
#ifdef _WIN32
            if (localtime_s(&parts, &raw) != 0)
                return "-";
#else
            if (localtime_r(&raw, &parts) == nullptr)
                return "-";
#endif
            char buffer[32] = {};
            if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &parts) == 0)
                return "-";
            return buffer;
        }
    } // namespace

    SaveSystemPanel::SaveSystemPanel() : EditorPanel("Save System", "save_system_panel") {}

    Spark::SaveSystem& SaveSystemPanel::System() const
    {
        if (auto* context = ::EngineContext::Get())
        {
            if (auto* saveSystem = context->GetSaveSystem())
            {
                return *saveSystem;
            }
        }
        return Spark::SaveSystem::GetInstance();
    }

    bool SaveSystemPanel::Initialize()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "SaveSystemPanel initialized");
        System().SetMaxAutoSaves(m_autosaveRotation);
        RefreshSlots();
        return true;
    }

    void SaveSystemPanel::Update(float /*deltaTime*/) {}

    std::string SaveSystemPanel::SlotFilePath(const std::string& slotName) const
    {
        return m_saveDirectory + "/" + slotName + ".spark_save";
    }

    size_t SaveSystemPanel::RefreshSlots()
    {
        m_slots.clear();
        m_selectedSlot = -1;
        m_renaming = false;

        // Keep enumeration and GetSaveMetadata() reading the same directory. The panel
        // adopts the engine's directory (SaveSystem::GetSaveDirectory) instead of
        // forcing its own on every refresh, which would silently retarget a live game
        // session's saves. It only writes the directory back when the user has picked
        // a different one through SetSaveDirectory().
        if (m_saveDirectoryOverridden)
            System().SetSaveDirectory(m_saveDirectory);
        else
            m_saveDirectory = System().GetSaveDirectory();

        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::exists(m_saveDirectory, ec) || ec)
        {
            return 0;
        }

        for (const auto& entry : fs::directory_iterator(m_saveDirectory, ec))
        {
            if (ec)
                break;
            if (!entry.is_regular_file() || entry.path().extension() != ".spark_save")
                continue;

            SaveSlotInfo info;
            info.slotName = entry.path().stem().string();
            if (!System().GetSaveMetadata(info.slotName, info.metadata))
            {
                // The file exists but its header is unreadable; show it as unreadable
                // rather than inventing metadata for it.
                info.metadata = Spark::SaveMetadata{};
                info.metadata.saveName = "(unreadable)";
            }
            std::error_code sizeEc;
            info.fileSizeBytes = fs::file_size(entry.path(), sizeEc);
            if (sizeEc)
                info.fileSizeBytes = 0;
            m_slots.push_back(std::move(info));
        }

        std::sort(m_slots.begin(), m_slots.end(),
                  [](const SaveSlotInfo& a, const SaveSlotInfo& b) { return a.slotName < b.slotName; });
        return m_slots.size();
    }

    void SaveSystemPanel::SetSaveDirectory(const std::string& directory)
    {
        m_saveDirectory = directory;
        m_saveDirectoryOverridden = true;
        RefreshSlots();
    }

    bool SaveSystemPanel::DeleteSlot(const std::string& slotName)
    {
        const bool deleted = System().DeleteSave(slotName);
        if (!deleted)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "SaveSystemPanel: failed to delete save '%s'", slotName.c_str());
        }
        RefreshSlots();
        return deleted;
    }

    bool SaveSystemPanel::RenameSlot(const std::string& slotName, const std::string& newSlotName)
    {
        if (newSlotName.empty() || newSlotName == slotName || newSlotName.find_first_of("/\\") != std::string::npos)
        {
            return false;
        }

        namespace fs = std::filesystem;
        const fs::path from(SlotFilePath(slotName));
        const fs::path to(SlotFilePath(newSlotName));

        std::error_code ec;
        if (!fs::exists(from, ec) || ec || fs::exists(to, ec))
        {
            return false;
        }

        fs::rename(from, to, ec);
        if (ec)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "SaveSystemPanel: rename of '%s' failed: %s", slotName.c_str(),
                           ec.message().c_str());
            return false;
        }

        RefreshSlots();
        return true;
    }

    bool SaveSystemPanel::CanLoad() const
    {
        // Loading needs both ends of the transfer: a save this panel actually
        // enumerated, and a live game World to deserialize it into. An empty slot
        // list (a fresh directory, or the one left behind after the last save was
        // deleted) means there is nothing to load, whatever the engine side says.
        if (m_slots.empty())
        {
            return false;
        }

        auto* context = ::EngineContext::Get();
        return context != nullptr && context->GetWorld() != nullptr;
    }

    bool SaveSystemPanel::LoadSlot(const std::string& slotName)
    {
        auto* context = ::EngineContext::Get();
        ::World* world = context ? context->GetWorld() : nullptr;
        if (!world)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "SaveSystemPanel: no running game World to load save '%s' into",
                           slotName.c_str());
            return false;
        }

        const bool loaded = System().Load(slotName, *world);
        if (!loaded)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "SaveSystemPanel: load of save '%s' failed", slotName.c_str());
        }
        return loaded;
    }

    void SaveSystemPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            if (ImGui::BeginTabBar("SaveTabs"))
            {
                if (ImGui::BeginTabItem(ICON_FA_SAVE " Save Slots"))
                {
                    RenderSaveSlots();
                    if (m_selectedSlot >= 0 && m_selectedSlot < static_cast<int>(m_slots.size()))
                    {
                        ImGui::Separator();
                        RenderSlotActions();
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_COG " Autosave Settings"))
                {
                    RenderAutosaveSettings();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        EndPanel();
    }

    void SaveSystemPanel::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "SaveSystemPanel shutting down");
    }

    void SaveSystemPanel::RenderSaveSlots()
    {
        ImGui::Text("Directory: %s", m_saveDirectory.c_str());
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_REFRESH " Rescan"))
            RefreshSlots();

        if (m_slots.empty())
        {
            ImGui::TextDisabled("No save files in this directory.");
            return;
        }

        if (ImGui::BeginTable("SaveSlotTable", 5,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("Slot");
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Scene");
            ImGui::TableSetupColumn("Play Time", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Saved", ImGuiTableColumnFlags_WidthFixed, 130);
            ImGui::TableHeadersRow();

            for (int i = 0; i < static_cast<int>(m_slots.size()); ++i)
            {
                const SaveSlotInfo& slot = m_slots[i];
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                const bool selected = (m_selectedSlot == i);
                if (ImGui::Selectable(slot.slotName.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns))
                {
                    m_selectedSlot = i;
                    m_renaming = false;
                }

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(slot.metadata.saveName.c_str());

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(slot.metadata.sceneName.empty() ? "-" : slot.metadata.sceneName.c_str());

                ImGui::TableNextColumn();
                const int minutes = static_cast<int>(slot.metadata.playTime) / 60;
                const int seconds = static_cast<int>(slot.metadata.playTime) % 60;
                ImGui::Text("%d:%02d", minutes, seconds);

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(FormatTimestamp(slot.metadata.timestamp).c_str());
            }

            ImGui::EndTable();
        }
    }

    void SaveSystemPanel::RenderSlotActions()
    {
        const SaveSlotInfo slot = m_slots[static_cast<size_t>(m_selectedSlot)];
        ImGui::Text("Selected: %s (%.1f KB)", slot.slotName.c_str(), static_cast<double>(slot.fileSizeBytes) / 1024.0);

        if (m_renaming)
        {
            ImGui::SetNextItemWidth(200);
            const bool commit = ImGui::InputText("##rename", m_renameBuffer, sizeof(m_renameBuffer),
                                                 ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if (commit || ImGui::Button("OK"))
            {
                if (!RenameSlot(slot.slotName, m_renameBuffer))
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Editor, "SaveSystemPanel: could not rename '%s' to '%s'",
                                   slot.slotName.c_str(), m_renameBuffer);
                }
                m_renaming = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                m_renaming = false;
            return;
        }

        ImGui::BeginDisabled(!CanLoad());
        if (ImGui::Button(ICON_FA_DOWNLOAD " Load Save"))
            LoadSlot(slot.slotName);
        ImGui::EndDisabled();
        if (!CanLoad())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(needs a running game session)");
        }

        if (ImGui::Button(ICON_FA_PEN " Rename"))
        {
            m_renaming = true;
            std::snprintf(m_renameBuffer, sizeof(m_renameBuffer), "%s", slot.slotName.c_str());
        }

        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_TRASH " Delete"))
            DeleteSlot(slot.slotName);
    }

    void SaveSystemPanel::RenderAutosaveSettings()
    {
        if (ImGui::DragInt("Autosave Rotation", &m_autosaveRotation, 1.0f, 1, 10))
        {
            m_autosaveRotation = std::clamp(m_autosaveRotation, 1, 10);
            System().SetMaxAutoSaves(m_autosaveRotation);
        }
        ImGui::TextDisabled("SaveSystem keeps the last %d autosave slots.", m_autosaveRotation);

        ImGui::Separator();
        ImGui::TextDisabled("When autosaves and quicksaves are written is decided by the");
        ImGui::TextDisabled("running game module, not by the editor.");
    }

} // namespace SparkEditor
