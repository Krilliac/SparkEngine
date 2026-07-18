/**
 * @file DecorLayoutEditorCanvas.cpp
 * @brief Toolbar and 2D top-down canvas for the decor layout editor
 * @author Spark Engine Team
 * @date 2026
 *
 * Contains: RenderToolbar, RenderCanvas.
 *
 * Geometry conventions mirror the game exactly:
 *  - offset = [x, z] meters from region center (+x east, +z north); yaw in
 *    DEGREES (the RADIANS rule is PhysicsBody-only).
 *  - Footprints come from streaming the OBJ vertex min/max, the same contract
 *    as TFWorldCollision::ObjLocalAabb (local reader — no module linkage).
 *  - The yaw mapping is the play-test-validated Build() convention:
 *    x' = x*c + z*s, z' = -x*s + z*c; part world center = piece origin +
 *    Ry(pieceYaw) * part offset, part box rotation = pieceYaw + partYaw.
 */

#include "DecorLayoutEditorPanel.h"

#include "DecorLayoutEditorInternal.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>

namespace SparkEditor
{

    namespace
    {
        constexpr float kDegToRad = 0.01745329252f; // same constant as TFWorldCollision

        float Dist2(float ax, float ay, float bx, float by)
        {
            const float dx = ax - bx;
            const float dy = ay - by;
            return dx * dx + dy * dy;
        }
    } // namespace

    // ========================================================================
    // Rendering — toolbar + canvas
    // ========================================================================

    void DecorLayoutEditorPanel::RenderToolbar()
    {
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
        ImGui::Checkbox("Clearance overlay", &m_showClearance);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Draggable probe with the %.0fm capture-point clearance circle and the 0.8m pawn "
                              "capsule diameter, so walkable gaps are visible at a glance.",
                              static_cast<double>(m_clearanceM));
        ImGui::SameLine();
        ImGui::TextDisabled("| %s%s", m_dataPath.c_str(), m_dirty ? " *" : "");
    }

    void DecorLayoutEditorPanel::RenderCanvas(TierTemplate& t)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImVec2 size = ImGui::GetContentRegionAvail();
        size.x = std::max(size.x, 64.0f);
        size.y = std::max(size.y, 64.0f);
        const ImVec2 p1(p0.x + size.x, p0.y + size.y);

        ImGui::InvisibleButton("decor_layout_canvas", size,
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                   ImGuiButtonFlags_MouseButtonMiddle);
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();
        ImGuiIO& io = ImGui::GetIO();

        if (m_fitRequested)
        {
            // Half-extent covering every footprint corner + scatter ring + the
            // clearance circle, with padding; minimum keeps tiny templates sane.
            float extent = std::max(m_clearanceM + 4.0f, 24.0f);
            for (const Piece& p : t.pieces)
            {
                const ObjBounds& b = BoundsForModel(p.model);
                const float reach =
                    std::sqrt(Dist2(p.offX, p.offZ, 0.0f, 0.0f)) +
                    std::max({std::fabs(b.mn[0]), std::fabs(b.mx[0]), std::fabs(b.mn[2]), std::fabs(b.mx[2])});
                extent = std::max(extent, reach + 6.0f);
            }
            extent = std::max(extent, t.scatter.radiusMax + 6.0f);
            m_viewExtent = extent;
            m_zoom = 1.0f;
            m_panX = 0.0f;
            m_panY = 0.0f;
            m_fitRequested = false;
        }

        const float baseScale = 0.5f * std::min(size.x, size.y) / std::max(1.0f, m_viewExtent);
        const ImVec2 ctrRaw(p0.x + size.x * 0.5f, p0.y + size.y * 0.5f);

        // Zoom around the mouse cursor.
        if (hovered && io.MouseWheel != 0.0f)
        {
            const float sOld = baseScale * m_zoom;
            const float newZoom = std::clamp(m_zoom * std::pow(1.15f, io.MouseWheel), 0.1f, 40.0f);
            const float sNew = baseScale * newZoom;
            const float relX = io.MousePos.x - ctrRaw.x;
            const float relY = io.MousePos.y - ctrRaw.y;
            m_panX = relX - (relX - m_panX) * (sNew / sOld);
            m_panY = relY - (relY - m_panY) * (sNew / sOld);
            m_zoom = newZoom;
        }
        const float s = baseScale * m_zoom;

        auto toScreen = [&](float wx, float wz)
        { return ImVec2(ctrRaw.x + m_panX + wx * s, ctrRaw.y + m_panY - wz * s); };
        auto toWorldX = [&](float sx) { return (sx - ctrRaw.x - m_panX) / s; };
        auto toWorldZ = [&](float sy) { return -(sy - ctrRaw.y - m_panY) / s; };

        // Pan with middle or right drag.
        if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f) ||
                       ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f)))
        {
            m_panX += io.MouseDelta.x;
            m_panY += io.MouseDelta.y;
        }

        // Footprint helpers — the game's yaw mapping (x' = x*c + z*s, z' = -x*s + z*c).
        auto pieceCorners = [&](const Piece& p, const ObjBounds& b, ImVec2 out[4])
        {
            const float c = std::cos(p.yawDeg * kDegToRad);
            const float sn = std::sin(p.yawDeg * kDegToRad);
            const float xs[4] = {b.mn[0], b.mx[0], b.mx[0], b.mn[0]};
            const float zs[4] = {b.mn[2], b.mn[2], b.mx[2], b.mx[2]};
            for (int i = 0; i < 4; ++i)
            {
                const float wx = xs[i] * c + zs[i] * sn + p.offX;
                const float wz = -xs[i] * sn + zs[i] * c + p.offZ;
                out[i] = toScreen(wx, wz);
            }
        };
        auto pointInPiece = [&](const Piece& p, const ObjBounds& b, float wx, float wz)
        {
            const float c = std::cos(p.yawDeg * kDegToRad);
            const float sn = std::sin(p.yawDeg * kDegToRad);
            const float dx = wx - p.offX;
            const float dz = wz - p.offZ;
            const float lx = c * dx - sn * dz; // inverse of the forward mapping
            const float lz = sn * dx + c * dz;
            const float pad = 4.0f / s; // few pixels of grab slack
            return lx >= b.mn[0] - pad && lx <= b.mx[0] + pad && lz >= b.mn[2] - pad && lz <= b.mx[2] + pad;
        };
        auto partCenterWorld = [&](const Piece& p, const CollidePart& part, float& wx, float& wz)
        {
            const float c = std::cos(p.yawDeg * kDegToRad);
            const float sn = std::sin(p.yawDeg * kDegToRad);
            wx = part.off[0] * c + part.off[2] * sn + p.offX;
            wz = -part.off[0] * sn + part.off[2] * c + p.offZ;
        };
        auto partCorners = [&](const Piece& p, const CollidePart& part, ImVec2 out[4])
        {
            float cx = 0.0f, cz = 0.0f;
            partCenterWorld(p, part, cx, cz);
            const float total = (p.yawDeg + part.yawDeg) * kDegToRad;
            const float c = std::cos(total);
            const float sn = std::sin(total);
            const float hx = part.size[0] * 0.5f;
            const float hz = part.size[2] * 0.5f;
            const float xs[4] = {-hx, hx, hx, -hx};
            const float zs[4] = {-hz, -hz, hz, hz};
            for (int i = 0; i < 4; ++i)
            {
                const float wx = xs[i] * c + zs[i] * sn + cx;
                const float wz = -xs[i] * sn + zs[i] * c + cz;
                out[i] = toScreen(wx, wz);
            }
        };
        auto partResizeHandle = [&](const Piece& p, const CollidePart& part)
        {
            float cx = 0.0f, cz = 0.0f;
            partCenterWorld(p, part, cx, cz);
            const float total = (p.yawDeg + part.yawDeg) * kDegToRad;
            const float c = std::cos(total);
            const float sn = std::sin(total);
            const float hx = part.size[0] * 0.5f;
            const float hz = part.size[2] * 0.5f;
            return toScreen(hx * c + hz * sn + cx, -hx * sn + hz * c + cz); // local (+hx, +hz) corner
        };
        auto rotateHandle = [&](const Piece& p, const ObjBounds& b)
        {
            const float reach = std::max(std::fabs(b.mn[2]), std::fabs(b.mx[2])) + 16.0f / s;
            const float c = std::cos(p.yawDeg * kDegToRad);
            const float sn = std::sin(p.yawDeg * kDegToRad);
            // Local +z rotated: R(yaw) * (0, reach) = (reach*sn, reach*c).
            return toScreen(p.offX + reach * sn, p.offZ + reach * c);
        };

        Piece* sel = (m_selPiece >= 0 && m_selPiece < static_cast<int>(t.pieces.size()))
                         ? &t.pieces[static_cast<size_t>(m_selPiece)]
                         : nullptr;

        // ---- Left-mouse interaction: hit test on press, drag while held ----
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            m_dragKind = DragKind::None;
            const float hitR2 = 8.0f * 8.0f;
            const float mwx = toWorldX(io.MousePos.x);
            const float mwz = toWorldZ(io.MousePos.y);

            // 1) Clearance probe (small, sits on top).
            if (m_showClearance)
            {
                const ImVec2 pp = toScreen(m_probeX, m_probeZ);
                if (Dist2(pp.x, pp.y, io.MousePos.x, io.MousePos.y) < hitR2)
                    m_dragKind = DragKind::Probe;
            }
            // 2) Selected piece's handles + parts.
            if (m_dragKind == DragKind::None && sel != nullptr)
            {
                const ObjBounds& b = BoundsForModel(sel->model);
                const ImVec2 rh = rotateHandle(*sel, b);
                if (Dist2(rh.x, rh.y, io.MousePos.x, io.MousePos.y) < hitR2)
                {
                    m_dragKind = DragKind::PieceRotate;
                }
                else
                {
                    for (size_t k = 0; k < sel->collideParts.size() && m_dragKind == DragKind::None; ++k)
                    {
                        const CollidePart& part = sel->collideParts[k];
                        const ImVec2 rz = partResizeHandle(*sel, part);
                        if (Dist2(rz.x, rz.y, io.MousePos.x, io.MousePos.y) < hitR2)
                        {
                            m_selPart = static_cast<int>(k);
                            m_dragKind = DragKind::PartResize;
                            break;
                        }
                        float cx = 0.0f, cz = 0.0f;
                        partCenterWorld(*sel, part, cx, cz);
                        const float total = (sel->yawDeg + part.yawDeg) * kDegToRad;
                        const float c = std::cos(total);
                        const float sn = std::sin(total);
                        const float dx = mwx - cx;
                        const float dz = mwz - cz;
                        const float lx = c * dx - sn * dz;
                        const float lz = sn * dx + c * dz;
                        if (std::fabs(lx) <= part.size[0] * 0.5f && std::fabs(lz) <= part.size[2] * 0.5f)
                        {
                            m_selPart = static_cast<int>(k);
                            m_dragKind = DragKind::PartMove;
                        }
                    }
                }
            }
            // 3) Piece bodies (topmost drawn = last in the list, so hit-test back to front).
            if (m_dragKind == DragKind::None)
            {
                int hit = -1;
                for (int i = static_cast<int>(t.pieces.size()) - 1; i >= 0; --i)
                {
                    const Piece& p = t.pieces[static_cast<size_t>(i)];
                    if (pointInPiece(p, BoundsForModel(p.model), mwx, mwz))
                    {
                        hit = i;
                        break;
                    }
                }
                if (hit >= 0)
                {
                    SelectPiece(hit, -1);
                    m_dragKind = DragKind::PieceMove;
                }
                else
                {
                    SelectPiece(-1, -1);
                }
            }
            // The click may have changed the selection — refresh the pointer so
            // a same-frame drag delta moves the piece that was just grabbed.
            sel = (m_selPiece >= 0 && m_selPiece < static_cast<int>(t.pieces.size()))
                      ? &t.pieces[static_cast<size_t>(m_selPiece)]
                      : nullptr;
        }

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            m_dragKind = DragKind::None;
        }
        else if (active && m_dragKind != DragKind::None && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f))
        {
            const float dwx = io.MouseDelta.x / s;
            const float dwz = -io.MouseDelta.y / s;
            switch (m_dragKind)
            {
            case DragKind::Probe:
                m_probeX += dwx;
                m_probeZ += dwz;
                break;
            case DragKind::PieceMove:
                if (sel != nullptr)
                {
                    sel->offX += dwx;
                    sel->offZ += dwz;
                    MarkDirty();
                }
                break;
            case DragKind::PieceRotate:
                if (sel != nullptr)
                {
                    const float dx = toWorldX(io.MousePos.x) - sel->offX;
                    const float dz = toWorldZ(io.MousePos.y) - sel->offZ;
                    if (dx != 0.0f || dz != 0.0f)
                    {
                        // Local +z points along (sin yaw, cos yaw) => yaw = atan2(x, z).
                        const float snapped = SnapYaw15(std::atan2(dx, dz) / kDegToRad);
                        if (snapped != sel->yawDeg)
                        {
                            sel->yawDeg = snapped;
                            MarkDirty();
                        }
                    }
                }
                break;
            case DragKind::PartMove:
                if (sel != nullptr && m_selPart >= 0 && m_selPart < static_cast<int>(sel->collideParts.size()))
                {
                    // World delta -> piece-local delta (inverse rotation by piece yaw).
                    CollidePart& part = sel->collideParts[static_cast<size_t>(m_selPart)];
                    const float c = std::cos(sel->yawDeg * kDegToRad);
                    const float sn = std::sin(sel->yawDeg * kDegToRad);
                    part.off[0] += c * dwx - sn * dwz;
                    part.off[2] += sn * dwx + c * dwz;
                    MarkDirty();
                }
                break;
            case DragKind::PartResize:
                if (sel != nullptr && m_selPart >= 0 && m_selPart < static_cast<int>(sel->collideParts.size()))
                {
                    // World delta -> part-local delta; the (+hx,+hz) corner follows
                    // the mouse, so each unit of local delta adds two to full size.
                    CollidePart& part = sel->collideParts[static_cast<size_t>(m_selPart)];
                    const float total = (sel->yawDeg + part.yawDeg) * kDegToRad;
                    const float c = std::cos(total);
                    const float sn = std::sin(total);
                    part.size[0] = std::max(0.1f, part.size[0] + 2.0f * (c * dwx - sn * dwz));
                    part.size[2] = std::max(0.1f, part.size[2] + 2.0f * (sn * dwx + c * dwz));
                    MarkDirty();
                }
                break;
            case DragKind::None:
                break;
            }
        }

        // ---- Drawing ----
        dl->PushClipRect(p0, p1, true);
        dl->AddRectFilled(p0, p1, IM_COL32(24, 26, 30, 255));

        // Meter grid (10 m) + origin axes (the region center).
        {
            const float wx0 = toWorldX(p0.x);
            const float wx1 = toWorldX(p1.x);
            const float wz0 = toWorldZ(p1.y); // bottom of the canvas = lowest z
            const float wz1 = toWorldZ(p0.y);
            const float step = 10.0f;
            for (float g = std::floor(wx0 / step) * step; g <= wx1; g += step)
                dl->AddLine(toScreen(g, wz0), toScreen(g, wz1), IM_COL32(60, 63, 70, 110));
            for (float g = std::floor(wz0 / step) * step; g <= wz1; g += step)
                dl->AddLine(toScreen(wx0, g), toScreen(wx1, g), IM_COL32(60, 63, 70, 110));
            dl->AddLine(toScreen(0.0f, wz0), toScreen(0.0f, wz1), IM_COL32(110, 115, 125, 170));
            dl->AddLine(toScreen(wx0, 0.0f), toScreen(wx1, 0.0f), IM_COL32(110, 115, 125, 170));
        }

        // Scatter ring (radius min/max) — context for where props may land.
        if (!t.scatter.models.empty() && t.scatter.radiusMax > 0.0f)
        {
            const ImVec2 o = toScreen(0.0f, 0.0f);
            dl->AddCircle(o, t.scatter.radiusMin * s, IM_COL32(120, 130, 145, 90), 64, 1.0f);
            dl->AddCircle(o, t.scatter.radiusMax * s, IM_COL32(120, 130, 145, 90), 64, 1.0f);
        }

        // Clearance preview UNDER the pieces: the capture-point clearance circle
        // (clearanceM) + the 0.8 m pawn capsule at the draggable probe.
        if (m_showClearance)
        {
            const ImVec2 pp = toScreen(m_probeX, m_probeZ);
            dl->AddCircleFilled(pp, m_clearanceM * s, IM_COL32(250, 210, 70, 26), 64);
            dl->AddCircle(pp, m_clearanceM * s, IM_COL32(250, 210, 70, 170), 64, 1.5f);
            dl->AddCircle(pp, kPawnRadiusM * s, IM_COL32(90, 220, 235, 220), 32, 1.5f);
            dl->AddCircleFilled(pp, 3.0f, IM_COL32(250, 210, 70, 255));
            char label[96];
            std::snprintf(label, sizeof(label), "clearance %.0fm / capsule 0.8m (drag me)",
                          static_cast<double>(m_clearanceM));
            dl->AddText(ImVec2(pp.x + 8.0f, pp.y - 16.0f), IM_COL32(250, 210, 70, 200), label);
        }

        // Pieces as rotated footprint rectangles. Color code:
        //   red    = collides via the whole-model OBB
        //   orange = collides via authored collideParts (arch/underside walkable)
        //   green  = walk-through (visual only)
        //   dashed-ish gray fill = OBJ missing (unit-cube fallback footprint)
        for (size_t i = 0; i < t.pieces.size(); ++i)
        {
            const Piece& p = t.pieces[i];
            const ObjBounds& b = BoundsForModel(p.model);
            ImVec2 quad[4];
            pieceCorners(p, b, quad);

            const bool hasParts = !p.collideParts.empty();
            ImU32 outline = p.collide ? (hasParts ? IM_COL32(255, 170, 60, 255) : IM_COL32(235, 100, 90, 255))
                                      : IM_COL32(120, 200, 140, 255);
            ImU32 fill = p.collide ? (hasParts ? IM_COL32(255, 170, 60, 34) : IM_COL32(235, 100, 90, 40))
                                   : IM_COL32(120, 200, 140, 28);
            if (!b.valid)
            {
                outline = IM_COL32(220, 70, 70, 255); // alarming: footprint is a guess
                fill = IM_COL32(220, 70, 70, 20);
            }
            const bool isSel = (static_cast<int>(i) == m_selPiece);
            dl->AddConvexPolyFilled(quad, 4, fill);
            dl->AddPolyline(quad, 4, isSel ? IM_COL32(255, 255, 255, 255) : outline, isSel ? 3.0f : 1.5f,
                            ImDrawFlags_Closed);

            // Facing tick: piece origin toward local +z.
            const float c = std::cos(p.yawDeg * kDegToRad);
            const float sn = std::sin(p.yawDeg * kDegToRad);
            const ImVec2 org = toScreen(p.offX, p.offZ);
            const float tickM = std::max(1.5f, (b.mx[2] - b.mn[2]) * 0.25f);
            dl->AddLine(org, toScreen(p.offX + tickM * sn, p.offZ + tickM * c), outline, 1.5f);
            dl->AddCircleFilled(org, 3.0f, outline);

            // Nested collidePart boxes (drawn for every piece; edited on the selected one).
            for (size_t k = 0; k < p.collideParts.size(); ++k)
            {
                ImVec2 pq[4];
                partCorners(p, p.collideParts[k], pq);
                const bool partSel = isSel && static_cast<int>(k) == m_selPart;
                dl->AddConvexPolyFilled(pq, 4, IM_COL32(235, 100, 90, 45));
                dl->AddPolyline(pq, 4, partSel ? IM_COL32(255, 255, 255, 255) : IM_COL32(235, 100, 90, 230),
                                partSel ? 2.5f : 1.5f, ImDrawFlags_Closed);
            }

            const std::string label = ModelLabel(p.model);
            const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
            dl->AddText(ImVec2(org.x - ts.x * 0.5f, org.y + 6.0f), IM_COL32(225, 228, 235, 255), label.c_str());
        }

        // Handles for the selected piece: rotate knob + part resize corners.
        if (sel != nullptr)
        {
            const ObjBounds& b = BoundsForModel(sel->model);
            const ImVec2 rh = rotateHandle(*sel, b);
            const ImVec2 org = toScreen(sel->offX, sel->offZ);
            dl->AddLine(org, rh, IM_COL32(255, 255, 120, 160), 1.0f);
            dl->AddCircleFilled(rh, 5.0f, IM_COL32(255, 255, 120, 255));
            dl->AddCircle(rh, 5.0f, IM_COL32(30, 30, 30, 255));
            for (const CollidePart& part : sel->collideParts)
            {
                const ImVec2 rz = partResizeHandle(*sel, part);
                dl->AddRectFilled(ImVec2(rz.x - 4.0f, rz.y - 4.0f), ImVec2(rz.x + 4.0f, rz.y + 4.0f),
                                  IM_COL32(255, 255, 255, 230));
                dl->AddRect(ImVec2(rz.x - 4.0f, rz.y - 4.0f), ImVec2(rz.x + 4.0f, rz.y + 4.0f),
                            IM_COL32(30, 30, 30, 255));
            }
        }

        // Legend / hint line.
        const char* hint =
            sel != nullptr
                ? "Drag body = move, yellow knob = rotate (15-deg steps). Parts: drag = move, white corner = "
                  "resize. Wheel = zoom, right/middle drag = pan."
                : "Click a footprint to select. Red = solid OBB, orange = collideParts, green = walk-through.";
        dl->AddText(ImVec2(p0.x + 6.0f, p0.y + 4.0f), IM_COL32(170, 175, 185, 255), hint);
        dl->PopClipRect();
    }

} // namespace SparkEditor
