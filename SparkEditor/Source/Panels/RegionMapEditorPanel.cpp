/**
 * @file RegionMapEditorPanel.cpp
 * @brief Visual editor for Terrafront continent region maps — lifecycle, validation, layout
 * @author Spark Engine Team
 * @date 2026
 *
 * Implementation split (shared helpers live in RegionMapEditorInternal.h):
 *  - RegionMapEditorPanel.cpp    — lifecycle, validation, editing helpers, toolbar + panel layout
 *  - RegionMapEditorIO.cpp       — load / save / hand-rolled writer (schema contract documented there)
 *  - RegionMapEditorCanvas.cpp   — 2D map canvas rendering + mouse interaction
 *  - RegionMapEditorSidePane.cpp — side-pane editors and the validation/save UI
 *
 * Validation (BFS skyanchor reachability, capture-point radius heuristic,
 * orphan links, plus every hard error ParseRegions raises) blocks the save
 * until the 'I know what I am doing' checkbox is ticked.
 *
 * This panel edits the DATA FILE only — no live world interaction.
 */

#include "RegionMapEditorPanel.h"

#include "RegionMapEditorInternal.h"
#include "Utils/LogMacros.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

namespace SparkEditor
{
    using namespace RegionMapInternal;

    // ========================================================================
    // Construction / lifecycle
    // ========================================================================

    RegionMapEditorPanel::RegionMapEditorPanel() : EditorPanel("Region Map Editor", "region_map_editor_panel")
    {
        m_category = PanelCategory::Tool;
    }

    bool RegionMapEditorPanel::Initialize()
    {
        ResolveDataPath();
        std::string err;
        if (LoadFromDisk(err))
        {
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "RegionMapEditorPanel: loaded %d region(s) from '%s'",
                           static_cast<int>(m_regions.size()), m_dataPath.c_str());
        }
        else
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "RegionMapEditorPanel: failed to load '%s': %s",
                           m_dataPath.c_str(), err.c_str());
        }
        return true; // the panel is still useful (shows the error + Reload button)
    }

    void RegionMapEditorPanel::Update(float /*deltaTime*/) {}

    void RegionMapEditorPanel::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Shutting down Region Map Editor panel");
    }

    // ========================================================================
    // Validation
    // ========================================================================

    float RegionMapEditorPanel::RegionRadiusHeuristic(size_t regionIndex) const
    {
        // Heuristic footprint: half the distance to the nearest other region
        // center, clamped to a sane minimum so tightly-packed lattices don't
        // produce zero-size regions.
        float best = m_continent.sizeM * m_continent.sizeM;
        const Region& r = m_regions[regionIndex];
        for (size_t i = 0; i < m_regions.size(); ++i)
        {
            if (i == regionIndex)
                continue;
            best = std::min(best, Dist2(r.centerX, r.centerZ, m_regions[i].centerX, m_regions[i].centerZ));
        }
        const float radius = 0.5f * std::sqrt(best);
        return std::max(radius, 150.0f);
    }

    void RegionMapEditorPanel::RunValidation()
    {
        m_violations.clear();
        m_validationRan = true;
        if (!m_loaded)
        {
            m_violations.push_back("no document loaded");
            return;
        }

        const size_t count = m_regions.size();
        if (count == 0)
            m_violations.push_back("regions array is empty (ParseRegions rejects this)");
        if (count > static_cast<size_t>(kMaxRegions))
            m_violations.push_back("too many regions (" + std::to_string(count) + " > " + std::to_string(kMaxRegions) +
                                   ")");

        // Ids contiguous [0..N) — ParseRegions hard-fails otherwise.
        for (size_t i = 0; i < count; ++i)
        {
            if (m_regions[i].id != static_cast<int>(i))
            {
                m_violations.push_back("region ids must be contiguous starting at 0 (index " + std::to_string(i) +
                                       " has id " + std::to_string(m_regions[i].id) + ")");
                break;
            }
        }

        std::unordered_set<std::string> seenKeys;
        for (size_t i = 0; i < count; ++i)
        {
            const Region& r = m_regions[i];
            const std::string tag = "'" + (r.key.empty() ? ("#" + std::to_string(r.id)) : r.key) + "'";
            if (r.key.empty())
                m_violations.push_back(tag + ": empty key");
            else if (!seenKeys.insert(r.key).second)
                m_violations.push_back(tag + ": duplicate key");
            if (!IsValidTier(r.tier))
                m_violations.push_back(tag + ": unknown tier '" + r.tier + "'");
            if (r.tier == "skyanchor" && !IsFactionTag(r.homeFaction))
                m_violations.push_back(tag + ": skyanchor needs a homeFaction (MRA/AUC/HLX)");
            if (!r.homeFaction.empty() && !IsFactionTag(r.homeFaction))
                m_violations.push_back(tag + ": bad homeFaction '" + r.homeFaction + "'");
            if (r.spawns.empty())
                m_violations.push_back(tag + ": no spawns (every region needs at least one)");
            if (r.tier != "skyanchor" && r.capturePoints.empty())
                m_violations.push_back(tag + ": capturable region has no capturePoints");
            if (r.capturePoints.size() > static_cast<size_t>(kMaxCapturePoints))
                m_violations.push_back(tag + ": exceeds kMaxCapturePoints (" + std::to_string(kMaxCapturePoints) + ")");
            if (r.owner.empty())
                m_violations.push_back(tag + ": not in initialOwnership (will be written as neutral)");
            if (r.centerX < 0.0f || r.centerX > m_continent.sizeM || r.centerZ < 0.0f || r.centerZ > m_continent.sizeM)
                m_violations.push_back(tag + ": center outside world bounds [0.." + FormatNum(m_continent.sizeM) + "]");

            // Radius heuristic: capture points / spawns / terminal should sit
            // within the region's estimated footprint.
            const float radius = count > 1 ? RegionRadiusHeuristic(i) : m_continent.sizeM;
            const float r2 = radius * radius;
            auto checkPts = [&](const std::vector<std::array<float, 2>>& pts, const char* what)
            {
                for (size_t p = 0; p < pts.size(); ++p)
                {
                    if (Dist2(pts[p][0], pts[p][1], r.centerX, r.centerZ) > r2)
                        m_violations.push_back(tag + ": " + what + " " + std::to_string(p) +
                                               " lies outside the region radius heuristic (~" + FormatNum(radius) +
                                               " m)");
                }
            };
            checkPts(r.capturePoints, "capture point");
            checkPts(r.spawns, "spawn");
            if (r.hasVehicleTerminal && Dist2(r.vehicleTerminal[0], r.vehicleTerminal[1], r.centerX, r.centerZ) > r2)
                m_violations.push_back(tag + ": vehicle terminal lies outside the region radius heuristic (~" +
                                       FormatNum(radius) + " m)");
        }

        // Conduits: no orphan links, no self-links, no duplicates; non-empty.
        if (m_conduits.empty())
            m_violations.push_back("conduits array is empty (ParseRegions rejects this)");
        std::unordered_set<uint64_t> seenLinks;
        std::vector<std::vector<int>> adj(count);
        for (size_t i = 0; i < m_conduits.size(); ++i)
        {
            const int a = m_conduits[i].first;
            const int b = m_conduits[i].second;
            const std::string ctag = "conduit [" + std::to_string(a) + "," + std::to_string(b) + "]";
            if (a < 0 || b < 0 || a >= static_cast<int>(count) || b >= static_cast<int>(count))
            {
                m_violations.push_back(ctag + ": orphan link (endpoint is not a region id)");
                continue;
            }
            if (a == b)
            {
                m_violations.push_back(ctag + ": self-link");
                continue;
            }
            const uint64_t lo = static_cast<uint64_t>(std::min(a, b));
            const uint64_t hi = static_cast<uint64_t>(std::max(a, b));
            if (!seenLinks.insert((hi << 32) | lo).second)
                m_violations.push_back(ctag + ": duplicate link");
            adj[static_cast<size_t>(a)].push_back(b);
            adj[static_cast<size_t>(b)].push_back(a);
        }

        // BFS: every non-skyanchor region reachable from at least one skyanchor.
        if (count > 0)
        {
            std::vector<bool> reached(count, false);
            std::vector<int> queue;
            for (size_t i = 0; i < count; ++i)
            {
                if (m_regions[i].tier == "skyanchor")
                {
                    reached[i] = true;
                    queue.push_back(static_cast<int>(i));
                }
            }
            if (queue.empty())
                m_violations.push_back("no skyanchor regions - nothing is reachable");
            for (size_t head = 0; head < queue.size(); ++head)
            {
                for (int n : adj[static_cast<size_t>(queue[head])])
                {
                    if (n >= 0 && n < static_cast<int>(count) && !reached[static_cast<size_t>(n)])
                    {
                        reached[static_cast<size_t>(n)] = true;
                        queue.push_back(n);
                    }
                }
            }
            for (size_t i = 0; i < count; ++i)
            {
                if (!reached[i] && m_regions[i].tier != "skyanchor")
                    m_violations.push_back("'" + m_regions[i].key + "': not reachable from any skyanchor via conduits");
            }
        }
    }

    // ========================================================================
    // Editing helpers
    // ========================================================================

    void RegionMapEditorPanel::ToggleConduit(int a, int b)
    {
        if (a == b || a < 0 || b < 0)
            return;
        for (size_t i = 0; i < m_conduits.size(); ++i)
        {
            const auto& c = m_conduits[i];
            if ((c.first == a && c.second == b) || (c.first == b && c.second == a))
            {
                m_conduits.erase(m_conduits.begin() + static_cast<std::ptrdiff_t>(i));
                m_dirty = true;
                m_validationRan = false;
                return;
            }
        }
        m_conduits.emplace_back(a, b);
        m_dirty = true;
        m_validationRan = false;
    }

    void RegionMapEditorPanel::SelectRegion(int index)
    {
        m_selected = index;
        SyncEditBuffers();
    }

    void RegionMapEditorPanel::SyncEditBuffers()
    {
        if (m_selected == m_bufForRegion)
            return;
        m_bufForRegion = m_selected;
        if (m_selected >= 0 && m_selected < static_cast<int>(m_regions.size()))
        {
            const Region& r = m_regions[static_cast<size_t>(m_selected)];
            std::snprintf(m_keyBuf, sizeof(m_keyBuf), "%s", r.key.c_str());
            std::snprintf(m_nameBuf, sizeof(m_nameBuf), "%s", r.name.c_str());
        }
    }

    // ========================================================================
    // Rendering
    // ========================================================================

    void RegionMapEditorPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            RenderToolbar();
            ImGui::Separator();

            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const float sideW = 360.0f;
            const float canvasW = std::max(140.0f, avail.x - sideW - 8.0f);

            ImGui::BeginChild("region_canvas_child", ImVec2(canvasW, 0.0f), true,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            RenderCanvas();
            ImGui::EndChild();

            ImGui::SameLine();
            ImGui::BeginChild("region_side_pane", ImVec2(0.0f, 0.0f), true);
            RenderSidePane();
            ImGui::EndChild();
        }
        EndPanel();
    }

    void RegionMapEditorPanel::RenderToolbar()
    {
        const char* sourceLabel = "Unavailable";
        if (m_dataSourceIndex >= 0 && m_dataSourceIndex < static_cast<int>(m_dataSources.size()))
            sourceLabel = m_dataSources[static_cast<size_t>(m_dataSourceIndex)].name.c_str();

        ImGui::SetNextItemWidth(190.0f);
        ImGui::BeginDisabled(m_dirty || m_dataSources.empty());
        if (ImGui::BeginCombo("##region_map_continent", sourceLabel))
        {
            for (size_t i = 0; i < m_dataSources.size(); ++i)
            {
                const bool selected = static_cast<int>(i) == m_dataSourceIndex;
                const std::string label = m_dataSources[i].name + "##" + m_dataSources[i].key;
                if (ImGui::Selectable(label.c_str(), selected) && !selected)
                {
                    std::string err;
                    SelectDataSource(i, err);
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        if (m_dirty && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Save or reload the current map before switching continents.");
        else if (!m_dataSourceError.empty() && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", m_dataSourceError.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Reload"))
        {
            std::string err;
            LoadFromDisk(err);
        }
        ImGui::SameLine();
        if (ImGui::Button("Fit View"))
        {
            m_fitRequested = true;
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Link mode", &m_linkMode))
        {
            m_linkFirst = -1;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Click region A then region B to toggle a conduit between them.");
        ImGui::SameLine();
        ImGui::Checkbox("Move points with region", &m_movePointsWithRegion);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Dragging a region center also drags its capture points, spawns and terminal.");
        ImGui::SameLine();
        ImGui::TextDisabled("| %s%s", m_dataPath.c_str(), m_dirty ? " *" : "");
    }

} // namespace SparkEditor
