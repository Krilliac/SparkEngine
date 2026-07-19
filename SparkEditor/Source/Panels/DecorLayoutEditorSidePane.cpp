/**
 * @file DecorLayoutEditorSidePane.cpp
 * @brief Side pane (tier/piece editors, validation, save) for the decor layout editor
 * @author Spark Engine Team
 * @date 2026
 *
 * Contains: RenderSidePane, RenderTierEditor, RenderPieceEditor,
 * RenderValidationAndSave.
 *
 * Validation (missing OBJs, part budget kMaxCollideParts, the 20-per-region
 * piece cap, degenerate part sizes, scatter range sanity) blocks the save
 * until the 'I know what I am doing' checkbox is ticked.
 */

#include "DecorLayoutEditorPanel.h"

#include "DecorLayoutEditorInternal.h"
#include "Utils/LogMacros.h"

#include <imgui.h>

#include <cstddef>
#include <cstdio>
#include <string>

namespace SparkEditor
{

    // ========================================================================
    // Rendering — side pane
    // ========================================================================

    void DecorLayoutEditorPanel::RenderSidePane(TierTemplate& t)
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

        if (m_selPiece >= 0 && m_selPiece < static_cast<int>(t.pieces.size()))
            RenderPieceEditor(t);
        else
            RenderTierEditor(t);

        ImGui::Separator();
        RenderValidationAndSave();
    }

    void DecorLayoutEditorPanel::RenderTierEditor(TierTemplate& t)
    {
        ImGui::Text("Template: %s", kTierNames[m_tier]);
        ImGui::Separator();

        if (!t.present)
        {
            ImGui::TextWrapped("This tier has no template - regions of this tier stay bare.");
            if (ImGui::Button("Create template"))
            {
                t.present = true;
                MarkDirty();
            }
            return;
        }

        if (ImGui::DragFloat("Clearance (m)", &m_clearanceM, 0.25f, 0.5f, 64.0f, "%.1f"))
            MarkDirty();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Root-level clearanceM - the game demotes collidable pieces within this "
                              "distance of any capture point/spawn/terminal to visual-only.");

        ImGui::Spacing();
        ImGui::Text("Pieces (%d)", static_cast<int>(t.pieces.size()));
        for (size_t i = 0; i < t.pieces.size(); ++i)
        {
            const Piece& p = t.pieces[i];
            char row[320];
            std::snprintf(row, sizeof(row), "%d  %s  [%s]##piece%d", static_cast<int>(i), ModelLabel(p.model).c_str(),
                          p.collideParts.empty() ? (p.collide ? "solid" : "walk") : "parts", static_cast<int>(i));
            if (ImGui::Selectable(row, static_cast<int>(i) == m_selPiece))
                SelectPiece(static_cast<int>(i), -1);
        }
        ImGui::InputText("##newmodel", m_newModelBuf, sizeof(m_newModelBuf));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("OBJ path, e.g. Assets/Models/MMOFPS/buildings/barracks.obj");
        ImGui::SameLine();
        if (ImGui::SmallButton("Add piece") && m_newModelBuf[0] != '\0')
        {
            Piece p;
            p.model = m_newModelBuf;
            p.offX = 10.0f; // off-center default so new pieces never crowd the capture point
            p.offZ = 10.0f;
            t.pieces.push_back(std::move(p));
            SelectPiece(static_cast<int>(t.pieces.size()) - 1, -1);
            MarkDirty();
        }

        // Scatter spec.
        ImGui::Spacing();
        ImGui::TextUnformatted("Scatter (seeded prop ring)");
        for (size_t i = 0; i < t.scatter.models.size(); ++i)
        {
            ImGui::PushID(3000 + static_cast<int>(i));
            ImGui::BulletText("%s", ModelLabel(t.scatter.models[i]).c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", t.scatter.models[i].c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("X##sc"))
            {
                t.scatter.models.erase(t.scatter.models.begin() + static_cast<std::ptrdiff_t>(i));
                MarkDirty();
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        ImGui::InputText("##newscatter", m_newScatterBuf, sizeof(m_newScatterBuf));
        ImGui::SameLine();
        if (ImGui::SmallButton("Add scatter model") && m_newScatterBuf[0] != '\0')
        {
            t.scatter.models.emplace_back(m_newScatterBuf);
            MarkDirty();
        }
        int count[2] = {t.scatter.countMin, t.scatter.countMax};
        if (ImGui::DragInt2("Count min/max", count, 0.1f, 0, 20))
        {
            t.scatter.countMin = count[0];
            t.scatter.countMax = count[1];
            MarkDirty();
        }
        float radius[2] = {t.scatter.radiusMin, t.scatter.radiusMax};
        if (ImGui::DragFloat2("Radius min/max", radius, 0.5f, 0.0f, 200.0f, "%.0f"))
        {
            t.scatter.radiusMin = radius[0];
            t.scatter.radiusMax = radius[1];
            MarkDirty();
        }

        ImGui::Spacing();
        ImGui::TextWrapped("Select a piece on the canvas (or in the list) to edit it.");
    }

    void DecorLayoutEditorPanel::RenderPieceEditor(TierTemplate& t)
    {
        Piece& p = t.pieces[static_cast<size_t>(m_selPiece)];
        SyncEditBuffers();

        ImGui::Text("%s piece #%d", kTierNames[m_tier], m_selPiece);
        ImGui::SameLine();
        if (ImGui::SmallButton("Back"))
        {
            SelectPiece(-1, -1);
            return;
        }
        ImGui::Separator();

        if (ImGui::InputText("Model", m_modelBuf, sizeof(m_modelBuf)))
        {
            p.model = m_modelBuf;
            MarkDirty();
        }
        {
            const ObjBounds& b = BoundsForModel(p.model);
            if (b.valid)
                ImGui::TextDisabled("footprint %.1f x %.1f m, height %.1f m", static_cast<double>(b.mx[0] - b.mn[0]),
                                    static_cast<double>(b.mx[2] - b.mn[2]), static_cast<double>(b.mx[1] - b.mn[1]));
            else
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "OBJ missing - unit-cube footprint shown");
        }

        float off[2] = {p.offX, p.offZ};
        if (ImGui::DragFloat2("Offset X/Z (m)", off, 0.25f, -300.0f, 300.0f, "%.2f"))
        {
            p.offX = off[0];
            p.offZ = off[1];
            MarkDirty();
        }
        if (ImGui::DragFloat("Yaw (deg)", &p.yawDeg, 1.0f, -180.0f, 180.0f, "%.0f"))
            MarkDirty();
        ImGui::SameLine();
        if (ImGui::SmallButton("Snap 15"))
        {
            const float snapped = SnapYaw15(p.yawDeg);
            if (snapped != p.yawDeg)
            {
                p.yawDeg = snapped;
                MarkDirty();
            }
        }

        if (p.collideParts.empty())
        {
            if (ImGui::Checkbox("Collide (whole-model OBB)", &p.collide))
                MarkDirty();
        }
        else
        {
            ImGui::TextDisabled("Collide: implied by collideParts (whole-model OBB skipped)");
        }
        if (ImGui::Checkbox("Terrain align", &p.terrainAlign))
            MarkDirty();
        ImGui::SameLine();
        if (ImGui::Checkbox("Cast shadows", &p.castShadows))
            MarkDirty();
        if (ImGui::DragFloat("Emissive", &p.emissive, 0.05f, 0.0f, 4.0f, "%.2f"))
            MarkDirty();
        if (ImGui::InputText("Material", m_materialBuf, sizeof(m_materialBuf)))
        {
            p.material = m_materialBuf;
            MarkDirty();
        }

        // Collide parts (W11 gate-passages).
        ImGui::Spacing();
        ImGui::Text("Collide parts (%d/%d)", static_cast<int>(p.collideParts.size()),
                    static_cast<int>(kMaxCollideParts));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Model-local boxes that REPLACE the whole-model OBB - pillars/legs block "
                              "while the archway/underside stays walkable.");
        for (size_t k = 0; k < p.collideParts.size(); ++k)
        {
            CollidePart& part = p.collideParts[k];
            ImGui::PushID(4000 + static_cast<int>(k));
            const bool partSel = static_cast<int>(k) == m_selPart;
            char header[64];
            std::snprintf(header, sizeof(header), "part %d%s###parthdr", static_cast<int>(k),
                          partSel ? " (selected)" : "");
            if (ImGui::Selectable(header, partSel))
                m_selPart = static_cast<int>(k);
            ImGui::SameLine();
            if (ImGui::SmallButton("X##part"))
            {
                p.collideParts.erase(p.collideParts.begin() + static_cast<std::ptrdiff_t>(k));
                if (m_selPart >= static_cast<int>(p.collideParts.size()))
                    m_selPart = -1;
                MarkDirty();
                ImGui::PopID();
                break;
            }
            if (ImGui::DragFloat3("offset", part.off.data(), 0.1f, -60.0f, 60.0f, "%.2f"))
                MarkDirty();
            if (ImGui::DragFloat3("size", part.size.data(), 0.1f, 0.1f, 120.0f, "%.2f"))
                MarkDirty();
            if (ImGui::DragFloat("yawDeg", &part.yawDeg, 1.0f, -180.0f, 180.0f, "%.0f"))
                MarkDirty();
            ImGui::PopID();
        }
        if (p.collideParts.size() < kMaxCollideParts && ImGui::SmallButton("Add part"))
        {
            CollidePart part;
            part.size = {1.0f, 3.0f, 1.0f}; // pillar-ish default (full sizes)
            part.off = {0.0f, 1.5f, 0.0f};  // center sits half its height up
            p.collideParts.push_back(part);
            p.collide = true; // loader rule: parts imply collide
            m_selPart = static_cast<int>(p.collideParts.size()) - 1;
            MarkDirty();
        }

        ImGui::Spacing();
        if (ImGui::Button("Duplicate piece"))
        {
            Piece copy = p; // p may dangle after push_back - copy first
            copy.offX += 4.0f;
            copy.offZ += 4.0f;
            t.pieces.push_back(std::move(copy));
            SelectPiece(static_cast<int>(t.pieces.size()) - 1, -1);
            MarkDirty();
            return;
        }
        ImGui::SameLine();
        if (ImGui::Button("Remove piece"))
        {
            t.pieces.erase(t.pieces.begin() + static_cast<std::ptrdiff_t>(m_selPiece));
            SelectPiece(-1, -1);
            MarkDirty();
        }
    }

    void DecorLayoutEditorPanel::RenderValidationAndSave()
    {
        if (ImGui::Button("Validate"))
            RunValidation();
        ImGui::SameLine();
        const bool blocked = !m_validationRan || !m_violations.empty();
        if (ImGui::Button("Save decor.json"))
        {
            RunValidation();
            if (m_violations.empty() || m_overrideSave)
            {
                std::string err;
                if (SaveToDisk(err))
                {
                    m_statusMsg = "Saved. Backup written to decor.json.bak; ParseStrict re-read OK.";
                    m_statusIsError = false;
                    SPARK_LOG_INFO(Spark::LogCategory::Editor, "DecorLayoutEditorPanel: saved '%s'",
                                   m_dataPath.c_str());
                }
                else
                {
                    m_statusMsg = "Save FAILED: " + err;
                    m_statusIsError = true;
                    SPARK_LOG_ERROR(Spark::LogCategory::Editor, "DecorLayoutEditorPanel: %s", m_statusMsg.c_str());
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
                ImGui::BeginChild("decor_violation_list", ImVec2(0.0f, 140.0f), true);
                for (const std::string& v : m_violations)
                    ImGui::TextWrapped("- %s", v.c_str());
                ImGui::EndChild();
                ImGui::Checkbox("I know what I am doing (save despite violations)", &m_overrideSave);
            }
        }
    }

} // namespace SparkEditor
