/**
 * @file RegionMapEditorCanvas.cpp
 * @brief 2D map canvas rendering and mouse interaction for RegionMapEditorPanel (W11)
 * @author Spark Engine Team
 * @date 2026
 *
 * Regions draw as tier-colored pointy-top hexes at their world centers with
 * owner-colored outlines; conduits as lines underneath; capture points, spawns
 * and vehicle terminals as draggable markers. Wheel zooms around the cursor,
 * right/middle drag pans, link mode toggles conduits by clicking two regions.
 *
 * See RegionMapEditorIO.cpp for the schema contract of the file being edited.
 */

#include "RegionMapEditorPanel.h"

#include "RegionMapEditorInternal.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace SparkEditor
{
    using namespace RegionMapInternal;

    namespace
    {
        ImU32 TierFillColor(const std::string& tier)
        {
            if (tier == "skyanchor")
                return IM_COL32(255, 215, 90, 110);
            if (tier == "fort")
                return IM_COL32(230, 140, 80, 100);
            if (tier == "facility")
                return IM_COL32(170, 110, 240, 110);
            if (tier == "outpost")
                return IM_COL32(120, 200, 120, 90);
            return IM_COL32(220, 70, 70, 120); // unknown tier: alarming red
        }

        ImU32 OwnerColor(const std::string& owner)
        {
            if (owner == "MRA")
                return IM_COL32(80, 140, 255, 255);
            if (owner == "AUC")
                return IM_COL32(255, 170, 60, 255);
            if (owner == "HLX")
                return IM_COL32(200, 90, 255, 255);
            return IM_COL32(150, 150, 150, 255); // neutral / unassigned
        }

        float TierWorldRadius(const std::string& tier)
        {
            if (tier == "skyanchor")
                return 190.0f;
            if (tier == "facility")
                return 165.0f;
            if (tier == "fort")
                return 140.0f;
            return 110.0f;
        }
    } // namespace

    // ========================================================================
    // Canvas rendering
    // ========================================================================

    void RegionMapEditorPanel::RenderCanvas()
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImVec2 size = ImGui::GetContentRegionAvail();
        size.x = std::max(size.x, 64.0f);
        size.y = std::max(size.y, 64.0f);
        const ImVec2 p1(p0.x + size.x, p0.y + size.y);

        ImGui::InvisibleButton("region_map_canvas", size,
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                   ImGuiButtonFlags_MouseButtonMiddle);
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();
        ImGuiIO& io = ImGui::GetIO();

        const float sizeM = std::max(1.0f, m_continent.sizeM);
        const float baseScale = std::min(size.x, size.y) / sizeM;
        if (m_fitRequested)
        {
            m_zoom = 1.0f;
            m_panX = (size.x - sizeM * baseScale) * 0.5f;
            m_panY = (size.y - sizeM * baseScale) * 0.5f;
            m_fitRequested = false;
        }

        // Zoom around the mouse cursor.
        if (hovered && io.MouseWheel != 0.0f)
        {
            const float sOld = baseScale * m_zoom;
            const float newZoom = std::clamp(m_zoom * std::pow(1.15f, io.MouseWheel), 0.2f, 25.0f);
            const float sNew = baseScale * newZoom;
            const float mx = io.MousePos.x - p0.x;
            const float my = io.MousePos.y - p0.y;
            m_panX = mx - (mx - m_panX) * (sNew / sOld);
            m_panY = my - (my - m_panY) * (sNew / sOld);
            m_zoom = newZoom;
        }
        const float s = baseScale * m_zoom;

        auto toScreen = [&](float wx, float wz)
        { return ImVec2(p0.x + m_panX + wx * s, p0.y + m_panY + (sizeM - wz) * s); };

        // Pan with middle or right drag.
        if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f) ||
                       ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f)))
        {
            m_panX += io.MouseDelta.x;
            m_panY += io.MouseDelta.y;
        }

        // ---- Left-mouse interaction: hit test on press, drag while held ----
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            m_dragKind = DragKind::None;
            m_dragRegion = -1;
            m_dragPoint = -1;
            const float hitR2 = 8.0f * 8.0f;
            int hitRegion = -1;

            // Markers first (they are small and sit on top of the hexes).
            for (size_t i = 0; i < m_regions.size() && m_dragKind == DragKind::None; ++i)
            {
                const Region& r = m_regions[i];
                for (size_t j = 0; j < r.capturePoints.size(); ++j)
                {
                    const ImVec2 sp = toScreen(r.capturePoints[j][0], r.capturePoints[j][1]);
                    if (Dist2(sp.x, sp.y, io.MousePos.x, io.MousePos.y) < hitR2)
                    {
                        m_dragKind = DragKind::CapturePoint;
                        m_dragRegion = static_cast<int>(i);
                        m_dragPoint = static_cast<int>(j);
                        break;
                    }
                }
                for (size_t j = 0; m_dragKind == DragKind::None && j < r.spawns.size(); ++j)
                {
                    const ImVec2 sp = toScreen(r.spawns[j][0], r.spawns[j][1]);
                    if (Dist2(sp.x, sp.y, io.MousePos.x, io.MousePos.y) < hitR2)
                    {
                        m_dragKind = DragKind::Spawn;
                        m_dragRegion = static_cast<int>(i);
                        m_dragPoint = static_cast<int>(j);
                    }
                }
                if (m_dragKind == DragKind::None && r.hasVehicleTerminal)
                {
                    const ImVec2 sp = toScreen(r.vehicleTerminal[0], r.vehicleTerminal[1]);
                    if (Dist2(sp.x, sp.y, io.MousePos.x, io.MousePos.y) < hitR2)
                    {
                        m_dragKind = DragKind::Terminal;
                        m_dragRegion = static_cast<int>(i);
                    }
                }
            }
            // Region hexes second.
            if (m_dragKind == DragKind::None)
            {
                for (size_t i = 0; i < m_regions.size(); ++i)
                {
                    const Region& r = m_regions[i];
                    const ImVec2 c = toScreen(r.centerX, r.centerZ);
                    const float rpx = std::max(6.0f, TierWorldRadius(r.tier) * s);
                    if (Dist2(c.x, c.y, io.MousePos.x, io.MousePos.y) < rpx * rpx)
                    {
                        hitRegion = static_cast<int>(i);
                        break;
                    }
                }
            }

            if (m_linkMode)
            {
                // Link mode: pure click semantics, no dragging.
                m_dragKind = DragKind::None;
                if (m_dragRegion >= 0)
                    hitRegion = m_dragRegion; // clicking a marker counts as its region
                m_dragRegion = -1;
                m_dragPoint = -1;
                if (hitRegion >= 0)
                {
                    if (m_linkFirst < 0)
                    {
                        m_linkFirst = hitRegion;
                    }
                    else if (m_linkFirst != hitRegion)
                    {
                        ToggleConduit(m_regions[static_cast<size_t>(m_linkFirst)].id,
                                      m_regions[static_cast<size_t>(hitRegion)].id);
                        m_linkFirst = -1;
                    }
                    SelectRegion(hitRegion);
                }
                else
                {
                    m_linkFirst = -1;
                    SelectRegion(-1);
                }
            }
            else
            {
                if (m_dragKind != DragKind::None)
                {
                    SelectRegion(m_dragRegion);
                }
                else if (hitRegion >= 0)
                {
                    m_dragKind = DragKind::RegionCenter;
                    m_dragRegion = hitRegion;
                    SelectRegion(hitRegion);
                }
                else
                {
                    SelectRegion(-1);
                }
            }
        }

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            m_dragKind = DragKind::None;
        }
        else if (active && m_dragKind != DragKind::None && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f) &&
                 (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) && m_dragRegion >= 0 &&
                 m_dragRegion < static_cast<int>(m_regions.size()))
        {
            const float dwx = io.MouseDelta.x / s;
            const float dwz = -io.MouseDelta.y / s;
            Region& r = m_regions[static_cast<size_t>(m_dragRegion)];
            auto movePt = [&](std::array<float, 2>& pt)
            {
                pt[0] = std::clamp(pt[0] + dwx, 0.0f, sizeM);
                pt[1] = std::clamp(pt[1] + dwz, 0.0f, sizeM);
            };
            switch (m_dragKind)
            {
            case DragKind::RegionCenter:
                r.centerX = std::clamp(r.centerX + dwx, 0.0f, sizeM);
                r.centerZ = std::clamp(r.centerZ + dwz, 0.0f, sizeM);
                if (m_movePointsWithRegion)
                {
                    for (auto& pt : r.capturePoints)
                        movePt(pt);
                    for (auto& pt : r.spawns)
                        movePt(pt);
                    if (r.hasVehicleTerminal)
                        movePt(r.vehicleTerminal);
                }
                break;
            case DragKind::CapturePoint:
                if (m_dragPoint >= 0 && m_dragPoint < static_cast<int>(r.capturePoints.size()))
                    movePt(r.capturePoints[static_cast<size_t>(m_dragPoint)]);
                break;
            case DragKind::Spawn:
                if (m_dragPoint >= 0 && m_dragPoint < static_cast<int>(r.spawns.size()))
                    movePt(r.spawns[static_cast<size_t>(m_dragPoint)]);
                break;
            case DragKind::Terminal:
                if (r.hasVehicleTerminal)
                    movePt(r.vehicleTerminal);
                break;
            case DragKind::None:
                break;
            }
            m_dirty = true;
            m_validationRan = false;
        }

        // ---- Drawing ----
        dl->PushClipRect(p0, p1, true);
        dl->AddRectFilled(p0, p1, IM_COL32(24, 26, 30, 255));

        // World bounds + coarse grid.
        const ImVec2 wMin = toScreen(0.0f, sizeM);
        const ImVec2 wMax = toScreen(sizeM, 0.0f);
        dl->AddRect(wMin, wMax, IM_COL32(90, 95, 105, 200));
        const float gridStep = 512.0f;
        for (float g = gridStep; g < sizeM; g += gridStep)
        {
            dl->AddLine(toScreen(g, 0.0f), toScreen(g, sizeM), IM_COL32(60, 63, 70, 120));
            dl->AddLine(toScreen(0.0f, g), toScreen(sizeM, g), IM_COL32(60, 63, 70, 120));
        }

        // Conduits underneath the regions.
        for (const auto& c : m_conduits)
        {
            if (c.first < 0 || c.second < 0 || c.first >= static_cast<int>(m_regions.size()) ||
                c.second >= static_cast<int>(m_regions.size()))
                continue;
            const Region& a = m_regions[static_cast<size_t>(c.first)];
            const Region& b = m_regions[static_cast<size_t>(c.second)];
            dl->AddLine(toScreen(a.centerX, a.centerZ), toScreen(b.centerX, b.centerZ), IM_COL32(120, 180, 220, 160),
                        2.0f);
        }
        // Link-mode rubber band.
        if (m_linkMode && m_linkFirst >= 0 && m_linkFirst < static_cast<int>(m_regions.size()))
        {
            const Region& a = m_regions[static_cast<size_t>(m_linkFirst)];
            dl->AddLine(toScreen(a.centerX, a.centerZ), io.MousePos, IM_COL32(255, 255, 120, 200), 1.5f);
        }

        // Regions as pointy-top hexes.
        for (size_t i = 0; i < m_regions.size(); ++i)
        {
            const Region& r = m_regions[i];
            const ImVec2 c = toScreen(r.centerX, r.centerZ);
            const float rpx = std::max(6.0f, TierWorldRadius(r.tier) * s);
            ImVec2 pts[6];
            for (int k = 0; k < 6; ++k)
            {
                const float ang = (static_cast<float>(k) * 60.0f - 30.0f) * 3.14159265f / 180.0f;
                pts[k] = ImVec2(c.x + rpx * std::cos(ang), c.y + rpx * std::sin(ang));
            }
            dl->AddConvexPolyFilled(pts, 6, TierFillColor(r.tier));
            const bool isSel = (static_cast<int>(i) == m_selected);
            const bool isLinkFirst = (m_linkMode && static_cast<int>(i) == m_linkFirst);
            ImU32 outline = OwnerColor(r.owner);
            float thick = 2.0f;
            if (isLinkFirst)
            {
                outline = IM_COL32(255, 255, 120, 255);
                thick = 3.0f;
            }
            else if (isSel)
            {
                outline = IM_COL32(255, 255, 255, 255);
                thick = 3.0f;
            }
            dl->AddPolyline(pts, 6, outline, thick, ImDrawFlags_Closed);

            const std::string& label = r.name.empty() ? r.key : r.name;
            const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
            dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y + rpx + 2.0f), IM_COL32(225, 228, 235, 255), label.c_str());

            // Markers: capture points (yellow circles), spawns (green dots),
            // vehicle terminal (cyan square).
            for (const auto& cp : r.capturePoints)
            {
                const ImVec2 sp = toScreen(cp[0], cp[1]);
                dl->AddCircleFilled(sp, 5.0f, IM_COL32(250, 210, 70, 255));
                dl->AddCircle(sp, 5.0f, IM_COL32(30, 30, 30, 255));
            }
            for (const auto& sw : r.spawns)
            {
                const ImVec2 sp = toScreen(sw[0], sw[1]);
                dl->AddCircleFilled(sp, 3.5f, IM_COL32(110, 230, 110, 255));
            }
            if (r.hasVehicleTerminal)
            {
                const ImVec2 sp = toScreen(r.vehicleTerminal[0], r.vehicleTerminal[1]);
                dl->AddRectFilled(ImVec2(sp.x - 4.5f, sp.y - 4.5f), ImVec2(sp.x + 4.5f, sp.y + 4.5f),
                                  IM_COL32(90, 220, 235, 255));
                dl->AddRect(ImVec2(sp.x - 4.5f, sp.y - 4.5f), ImVec2(sp.x + 4.5f, sp.y + 4.5f),
                            IM_COL32(30, 30, 30, 255));
            }
        }

        // Legend / hint line.
        const char* hint = m_linkMode ? (m_linkFirst >= 0 ? "Link mode: click the second region (click empty to cancel)"
                                                          : "Link mode: click the first region")
                                      : "Drag hexes/markers to move. Wheel = zoom, right/middle drag = pan.";
        dl->AddText(ImVec2(p0.x + 6.0f, p0.y + 4.0f), IM_COL32(170, 175, 185, 255), hint);
        dl->PopClipRect();
    }

} // namespace SparkEditor
