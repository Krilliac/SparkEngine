/**
 * @file RegionMapEditorSidePane.cpp
 * @brief Side-pane editors (continent / region) and the validation + save UI for RegionMapEditorPanel (W11)
 * @author Spark Engine Team
 * @date 2026
 *
 * The side pane shows the continent editor when nothing is selected and the
 * per-region editor otherwise, followed by the Validate / Save controls.
 * Violations block the save until the 'I know what I am doing' checkbox is
 * ticked. See RegionMapEditorIO.cpp for the schema contract of the file.
 */

#include "RegionMapEditorPanel.h"

#include "RegionMapEditorInternal.h"
#include "Utils/LogMacros.h"

#include <imgui.h>

#include <cstddef>
#include <string>

namespace SparkEditor
{
    using namespace RegionMapInternal;

    // ========================================================================
    // Side pane
    // ========================================================================

    void RegionMapEditorPanel::RenderSidePane()
    {
        if (!m_statusMsg.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  m_statusIsError ? IM_COL32(255, 110, 110, 255) : IM_COL32(150, 210, 150, 255));
            ImGui::TextWrapped("%s", m_statusMsg.c_str());
            ImGui::PopStyleColor();
            ImGui::Separator();
        }
        if (!m_loaded)
        {
            ImGui::TextWrapped("Could not load '%s'. Fix the file (or its location) and press Reload.",
                               m_dataPath.c_str());
            return;
        }

        if (m_selected >= 0 && m_selected < static_cast<int>(m_regions.size()))
            RenderRegionEditor();
        else
            RenderContinentEditor();

        ImGui::Separator();
        RenderValidationAndSave();
    }

    void RegionMapEditorPanel::RenderContinentEditor()
    {
        ImGui::TextUnformatted("Continent");
        ImGui::Separator();
        auto markDirty = [this]
        {
            m_dirty = true;
            m_validationRan = false;
        };
        if (ImGui::InputText("Name", m_continentNameBuf, sizeof(m_continentNameBuf)))
        {
            m_continent.name = m_continentNameBuf;
            markDirty();
        }
        if (ImGui::InputText("Scene", m_sceneBuf, sizeof(m_sceneBuf)))
        {
            m_continent.scene = m_sceneBuf;
            markDirty();
        }
        if (ImGui::DragFloat("Size (m)", &m_continent.sizeM, 32.0f, 256.0f, 65536.0f, "%.0f"))
            markDirty();
        if (ImGui::DragFloat("Flux tick (s)", &m_continent.fluxTickSec, 1.0f, 1.0f, 3600.0f, "%.0f"))
            markDirty();
        ImGui::Spacing();
        ImGui::TextWrapped("Select a region on the map to edit it. Enable Link mode to toggle conduits.");
    }

    void RegionMapEditorPanel::RenderRegionEditor()
    {
        Region& r = m_regions[static_cast<size_t>(m_selected)];
        SyncEditBuffers();
        auto markDirty = [this]
        {
            m_dirty = true;
            m_validationRan = false;
        };

        ImGui::Text("Region #%d", r.id);
        ImGui::Separator();

        if (ImGui::InputText("Key", m_keyBuf, sizeof(m_keyBuf)))
        {
            r.key = m_keyBuf;
            markDirty();
        }
        if (ImGui::InputText("Name", m_nameBuf, sizeof(m_nameBuf)))
        {
            r.name = m_nameBuf;
            markDirty();
        }

        // Tier dropdown.
        int tierIdx = -1;
        for (int i = 0; i < 4; ++i)
            if (r.tier == kTierNames[i])
                tierIdx = i;
        const char* tierPreview = tierIdx >= 0 ? kTierNames[tierIdx] : r.tier.c_str();
        if (ImGui::BeginCombo("Tier", tierPreview))
        {
            for (int i = 0; i < 4; ++i)
            {
                if (ImGui::Selectable(kTierNames[i], i == tierIdx))
                {
                    r.tier = kTierNames[i];
                    markDirty();
                }
            }
            ImGui::EndCombo();
        }

        // Home faction (skyanchors require one; others usually omit it).
        const char* facPreview = r.homeFaction.empty() ? "(none)" : r.homeFaction.c_str();
        if (ImGui::BeginCombo("Home faction", facPreview))
        {
            if (ImGui::Selectable("(none)", r.homeFaction.empty()))
            {
                r.homeFaction.clear();
                markDirty();
            }
            for (const char* f : kFactionTags)
            {
                if (ImGui::Selectable(f, r.homeFaction == f))
                {
                    r.homeFaction = f;
                    markDirty();
                }
            }
            ImGui::EndCombo();
        }

        // Initial owner bucket.
        const char* ownPreview = r.owner.empty() ? "(unassigned)" : r.owner.c_str();
        if (ImGui::BeginCombo("Initial owner", ownPreview))
        {
            const char* const owners[] = {"neutral", "MRA", "AUC", "HLX"};
            for (const char* o : owners)
            {
                if (ImGui::Selectable(o, r.owner == o))
                {
                    r.owner = o;
                    markDirty();
                }
            }
            ImGui::EndCombo();
        }

        float center[2] = {r.centerX, r.centerZ};
        if (ImGui::DragFloat2("Center X/Z", center, 4.0f, 0.0f, m_continent.sizeM, "%.0f"))
        {
            r.centerX = center[0];
            r.centerZ = center[1];
            markDirty();
        }
        if (ImGui::Checkbox("Has hex coords", &r.hasHex))
            markDirty();
        if (r.hasHex)
        {
            int hex[2] = {r.hexQ, r.hexR};
            if (ImGui::DragInt2("Hex q/r", hex, 0.1f, -16, 16))
            {
                r.hexQ = hex[0];
                r.hexR = hex[1];
                markDirty();
            }
        }
        if (ImGui::DragFloat("Capture (s)", &r.captureSec, 1.0f, 0.0f, 3600.0f, "%.0f"))
            markDirty();
        if (ImGui::DragInt("Flux per tick", &r.fluxPerTick, 0.1f, 0, 100))
            markDirty();

        // Capture points.
        ImGui::Spacing();
        ImGui::Text("Capture points (%d/%d)", static_cast<int>(r.capturePoints.size()), kMaxCapturePoints);
        for (size_t j = 0; j < r.capturePoints.size(); ++j)
        {
            ImGui::PushID(static_cast<int>(j));
            if (ImGui::DragFloat2("##cp", r.capturePoints[j].data(), 2.0f, 0.0f, m_continent.sizeM, "%.0f"))
                markDirty();
            ImGui::SameLine();
            if (ImGui::SmallButton("X##cp"))
            {
                r.capturePoints.erase(r.capturePoints.begin() + static_cast<std::ptrdiff_t>(j));
                markDirty();
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        if (static_cast<int>(r.capturePoints.size()) < kMaxCapturePoints && ImGui::SmallButton("Add capture point"))
        {
            r.capturePoints.push_back({r.centerX + 40.0f, r.centerZ});
            markDirty();
        }

        // Spawns.
        ImGui::Spacing();
        ImGui::Text("Spawns (%d)", static_cast<int>(r.spawns.size()));
        for (size_t j = 0; j < r.spawns.size(); ++j)
        {
            ImGui::PushID(1000 + static_cast<int>(j));
            if (ImGui::DragFloat2("##sp", r.spawns[j].data(), 2.0f, 0.0f, m_continent.sizeM, "%.0f"))
                markDirty();
            ImGui::SameLine();
            if (ImGui::SmallButton("X##sp"))
            {
                r.spawns.erase(r.spawns.begin() + static_cast<std::ptrdiff_t>(j));
                markDirty();
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        if (ImGui::SmallButton("Add spawn"))
        {
            r.spawns.push_back({r.centerX, r.centerZ - 40.0f});
            markDirty();
        }

        // Vehicle terminal.
        ImGui::Spacing();
        if (ImGui::Checkbox("Vehicle terminal", &r.hasVehicleTerminal))
        {
            if (r.hasVehicleTerminal && r.vehicleTerminal[0] == 0.0f && r.vehicleTerminal[1] == 0.0f)
                r.vehicleTerminal = {r.centerX, r.centerZ + 60.0f};
            markDirty();
        }
        if (r.hasVehicleTerminal)
        {
            if (ImGui::DragFloat2("Terminal X/Z", r.vehicleTerminal.data(), 2.0f, 0.0f, m_continent.sizeM, "%.0f"))
                markDirty();
        }

        // Conduits of this region.
        ImGui::Spacing();
        ImGui::TextUnformatted("Conduits");
        for (size_t i = 0; i < m_conduits.size(); ++i)
        {
            const auto& c = m_conduits[i];
            if (c.first != r.id && c.second != r.id)
                continue;
            const int otherId = (c.first == r.id) ? c.second : c.first;
            const char* otherKey = (otherId >= 0 && otherId < static_cast<int>(m_regions.size()))
                                       ? m_regions[static_cast<size_t>(otherId)].key.c_str()
                                       : "<orphan>";
            ImGui::PushID(2000 + static_cast<int>(i));
            ImGui::BulletText("-> %d (%s)", otherId, otherKey);
            ImGui::SameLine();
            if (ImGui::SmallButton("X##cd"))
            {
                m_conduits.erase(m_conduits.begin() + static_cast<std::ptrdiff_t>(i));
                markDirty();
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
    }

    void RegionMapEditorPanel::RenderValidationAndSave()
    {
        if (ImGui::Button("Validate"))
            RunValidation();
        ImGui::SameLine();
        const bool blocked = !m_validationRan || !m_violations.empty();
        if (ImGui::Button("Save regions.json"))
        {
            RunValidation();
            if (m_violations.empty() || m_overrideSave)
            {
                std::string err;
                if (SaveToDisk(err))
                {
                    m_statusMsg = "Saved. Backup written to regions.json.bak; ParseStrict re-read OK.";
                    m_statusIsError = false;
                    SPARK_LOG_INFO(Spark::LogCategory::Editor, "RegionMapEditorPanel: saved '%s'", m_dataPath.c_str());
                }
                else
                {
                    m_statusMsg = "Save FAILED: " + err;
                    m_statusIsError = true;
                    SPARK_LOG_ERROR(Spark::LogCategory::Editor, "RegionMapEditorPanel: %s", m_statusMsg.c_str());
                }
            }
            else
            {
                m_statusMsg = "Save blocked: " + std::to_string(m_violations.size()) +
                              " violation(s). Fix them or tick the override checkbox.";
                m_statusIsError = true;
            }
        }
        if (blocked && ImGui::IsItemHovered())
            ImGui::SetTooltip("Validation runs automatically on save; violations block it unless overridden.");

        if (m_validationRan)
        {
            if (m_violations.empty())
            {
                ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f),
                                   "Validation OK - the game will accept this file.");
            }
            else
            {
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f),
                                   "%d violation(s):", static_cast<int>(m_violations.size()));
                ImGui::BeginChild("violation_list", ImVec2(0.0f, 140.0f), true);
                for (const std::string& v : m_violations)
                    ImGui::TextWrapped("- %s", v.c_str());
                ImGui::EndChild();
                ImGui::Checkbox("I know what I am doing (save despite violations)", &m_overrideSave);
            }
        }
    }

} // namespace SparkEditor
