/**
 * @file DecorLayoutEditorPanel.cpp
 * @brief Visual editor for Assets/MMOFPS/Data/decor.json (W12) — core lifecycle, validation, footprints
 * @author Spark Engine Team
 * @date 2026
 *
 * Contains: construction/lifecycle, Render (top-level layout), editing
 * helpers (SelectPiece/SyncEditBuffers/MarkDirty), BoundsForModel,
 * RunValidation. The implementation is split across sibling files:
 *  - DecorLayoutEditorIO.cpp       — load/save/serialize (schema contract lives there)
 *  - DecorLayoutEditorCanvas.cpp   — toolbar + 2D canvas (geometry conventions live there)
 *  - DecorLayoutEditorSidePane.cpp — tier/piece editors, validation UI, save UI
 *  - DecorLayoutEditorInternal.h   — shared constants/helpers for the split
 *
 * This panel edits the DATA FILE only — no live world interaction.
 */

#include "DecorLayoutEditorPanel.h"

#include "DecorLayoutEditorInternal.h"
#include "Utils/LogMacros.h"

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace SparkEditor
{

    // ========================================================================
    // Construction / lifecycle
    // ========================================================================

    DecorLayoutEditorPanel::DecorLayoutEditorPanel() : EditorPanel("Decor Layout Editor", "decor_layout_editor_panel")
    {
        m_category = PanelCategory::Tool;
    }

    bool DecorLayoutEditorPanel::Initialize()
    {
        ResolveDataPath();
        std::string err;
        if (LoadFromDisk(err))
        {
            int pieces = 0;
            for (const TierTemplate& t : m_tiers)
                pieces += static_cast<int>(t.pieces.size());
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "DecorLayoutEditorPanel: loaded %d piece(s) from '%s'", pieces,
                           m_dataPath.c_str());
        }
        else
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "DecorLayoutEditorPanel: failed to load '%s': %s",
                           m_dataPath.c_str(), err.c_str());
        }
        return true; // the panel is still useful (shows the error + Reload button)
    }

    void DecorLayoutEditorPanel::Update(float /*deltaTime*/) {}

    void DecorLayoutEditorPanel::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Shutting down Decor Layout Editor panel");
    }

    // ========================================================================
    // OBJ footprints
    // ========================================================================

    const DecorLayoutEditorPanel::ObjBounds& DecorLayoutEditorPanel::BoundsForModel(const std::string& model)
    {
        auto it = m_boundsCache.find(model);
        if (it != m_boundsCache.end())
            return it->second;

        // Stream the OBJ once — vertex lines only, min/max accumulate. Same
        // contract as TFWorldCollision::ObjLocalAabb (which is module code the
        // editor must not link). Misses are cached too, so a missing file does
        // not re-hit the disk every frame; Reload clears the cache.
        ObjBounds b;
        std::ifstream f(m_assetsPrefix + model);
        if (f.is_open())
        {
            bool any = false;
            std::string line;
            while (std::getline(f, line))
            {
                if (line.size() < 3 || line[0] != 'v' || line[1] != ' ')
                    continue;
                float v[3] = {0.0f, 0.0f, 0.0f};
                const char* c = line.c_str() + 2;
                for (float& axis : v)
                {
                    char* end = nullptr;
                    axis = std::strtof(c, &end);
                    if (end == c)
                        break;
                    c = end;
                }
                if (!any)
                {
                    for (size_t i = 0; i < 3; ++i)
                        b.mn[i] = b.mx[i] = v[i];
                    any = true;
                }
                else
                {
                    for (size_t i = 0; i < 3; ++i)
                    {
                        b.mn[i] = std::min(b.mn[i], v[i]);
                        b.mx[i] = std::max(b.mx[i], v[i]);
                    }
                }
            }
            b.valid = any;
        }
        if (!b.valid)
        {
            b.mn = {-0.5f, -0.5f, -0.5f}; // unit-cube fallback, drawn dashed
            b.mx = {0.5f, 0.5f, 0.5f};
        }
        return m_boundsCache.emplace(model, b).first->second;
    }

    // ========================================================================
    // Validation
    // ========================================================================

    void DecorLayoutEditorPanel::RunValidation()
    {
        m_violations.clear();
        m_validationRan = true;
        if (!m_loaded)
        {
            m_violations.push_back("no document loaded");
            return;
        }
        if (m_clearanceM <= 0.0f)
            m_violations.push_back("clearanceM must be positive");

        bool anyPresent = false;
        for (int ti = 0; ti < kTierCount; ++ti)
        {
            const TierTemplate& t = m_tiers[static_cast<size_t>(ti)];
            if (!t.present)
                continue;
            anyPresent = true;
            const std::string tier = kTierNames[ti];

            for (size_t i = 0; i < t.pieces.size(); ++i)
            {
                const Piece& p = t.pieces[i];
                const std::string tag = tier + " piece " + std::to_string(i) + " ('" + ModelLabel(p.model) + "')";
                if (p.model.empty())
                {
                    m_violations.push_back(tag + ": empty model path (the game skips it)");
                    continue;
                }
                if (!BoundsForModel(p.model).valid)
                    m_violations.push_back(tag + ": OBJ missing/unreadable ('" + p.model +
                                           "') - placeholder visual, collidable pieces stay walk-through");
                if (p.collideParts.size() > kMaxCollideParts)
                    m_violations.push_back(tag + ": " + std::to_string(p.collideParts.size()) +
                                           " collideParts exceeds the game cap of " + std::to_string(kMaxCollideParts) +
                                           " (extras dropped)");
                for (size_t k = 0; k < p.collideParts.size(); ++k)
                {
                    const CollidePart& part = p.collideParts[k];
                    if (part.size[0] <= 0.0f || part.size[1] <= 0.0f || part.size[2] <= 0.0f)
                        m_violations.push_back(tag + ": collidePart " + std::to_string(k) +
                                               " has a non-positive size axis");
                }
            }

            const Scatter& sc = t.scatter;
            if (sc.countMin < 0)
                m_violations.push_back(tier + ": scatter count min is negative");
            if (sc.countMax < sc.countMin)
                m_violations.push_back(tier + ": scatter count max < min (the game clamps, fix the data)");
            if (sc.radiusMax < sc.radiusMin)
                m_violations.push_back(tier + ": scatter radius max < min (the game clamps, fix the data)");
            if (sc.countMax > 0 && sc.models.empty())
                m_violations.push_back(tier + ": scatter count > 0 but no scatter models");
            for (size_t i = 0; i < sc.models.size(); ++i)
            {
                if (sc.models[i].empty())
                    m_violations.push_back(tier + ": scatter model " + std::to_string(i) + " is empty");
                else if (!BoundsForModel(sc.models[i]).valid)
                    m_violations.push_back(tier + ": scatter model OBJ missing ('" + sc.models[i] + "')");
            }

            const size_t worstCase = t.pieces.size() + static_cast<size_t>(std::max(sc.countMax, 0));
            if (worstCase > kMaxDecorPerRegion)
                m_violations.push_back(tier + ": pieces + max scatter = " + std::to_string(worstCase) +
                                       " exceeds the per-region cap of " + std::to_string(kMaxDecorPerRegion) +
                                       " (the game drops overflow)");
        }
        if (!anyPresent)
            m_violations.push_back("no tier templates - every region stays bare");
    }

    // ========================================================================
    // Editing helpers
    // ========================================================================

    void DecorLayoutEditorPanel::SelectPiece(int piece, int part)
    {
        m_selPiece = piece;
        m_selPart = part;
        SyncEditBuffers();
    }

    void DecorLayoutEditorPanel::SyncEditBuffers()
    {
        if (m_selPiece == m_bufPiece && m_tier == m_bufTier)
            return;
        m_bufTier = m_tier;
        m_bufPiece = m_selPiece;
        const TierTemplate& t = m_tiers[static_cast<size_t>(m_tier)];
        if (m_selPiece >= 0 && m_selPiece < static_cast<int>(t.pieces.size()))
        {
            const Piece& p = t.pieces[static_cast<size_t>(m_selPiece)];
            std::snprintf(m_modelBuf, sizeof(m_modelBuf), "%s", p.model.c_str());
            std::snprintf(m_materialBuf, sizeof(m_materialBuf), "%s", p.material.c_str());
        }
    }

    void DecorLayoutEditorPanel::MarkDirty()
    {
        m_dirty = true;
        m_validationRan = false;
    }

    // ========================================================================
    // Rendering
    // ========================================================================

    void DecorLayoutEditorPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            RenderToolbar();
            ImGui::Separator();

            if (ImGui::BeginTabBar("decor_tier_tabs"))
            {
                for (int ti = 0; ti < kTierCount; ++ti)
                {
                    if (ImGui::BeginTabItem(kTierNames[ti]))
                    {
                        if (m_tier != ti)
                        {
                            m_tier = ti;
                            SelectPiece(-1, -1);
                            m_dragKind = DragKind::None;
                            m_fitRequested = true;
                        }
                        ImGui::EndTabItem();
                    }
                }
                ImGui::EndTabBar();
            }

            TierTemplate& t = m_tiers[static_cast<size_t>(m_tier)];

            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const float sideW = 380.0f;
            const float canvasW = std::max(140.0f, avail.x - sideW - 8.0f);

            ImGui::BeginChild("decor_canvas_child", ImVec2(canvasW, 0.0f), true,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            RenderCanvas(t);
            ImGui::EndChild();

            ImGui::SameLine();
            ImGui::BeginChild("decor_side_pane", ImVec2(0.0f, 0.0f), true);
            RenderSidePane(t);
            ImGui::EndChild();
        }
        EndPanel();
    }

} // namespace SparkEditor
