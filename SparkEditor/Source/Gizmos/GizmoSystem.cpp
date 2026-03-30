/**
 * @file GizmoSystem.cpp
 * @brief 3D object manipulation gizmo system implementation
 * @author Spark Engine Team
 * @date 2025
 *
 * Implements translation, rotation, and scale gizmos rendered via ImGui
 * overlay draw lists. DirectX geometry buffers and shaders are not used;
 * all visualisation is performed through ImGui so the system works
 * regardless of the underlying render backend.
 */

#include "GizmoSystem.h"
#include "Utils/LogMacros.h"
#include "Utils/MathUtils.h"
#include "Utils/Validate.h"
#include <imgui.h>
#include <cmath>
#include <algorithm>

using namespace DirectX;

namespace SparkEditor
{

    // ========================================================================
    // Constants
    // ========================================================================

    static constexpr float GIZMO_AXIS_LENGTH = 100.0f;  // pixels
    static constexpr float GIZMO_HIT_THRESHOLD = 10.0f; // pixels
    static constexpr float GIZMO_RING_RADIUS = 80.0f;   // pixels
    static constexpr int GIZMO_RING_SEGMENTS = 64;
    static constexpr float GIZMO_ARROW_SIZE = 10.0f; // pixels
    static constexpr float GIZMO_CUBE_SIZE = 6.0f;   // pixels

    // ========================================================================
    // Helpers
    // ========================================================================

    static XMFLOAT2 WorldToScreen(const XMFLOAT3& worldPos, const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix,
                                  const XMFLOAT4& viewport)
    {
        XMVECTOR pos = XMLoadFloat3(&worldPos);
        XMVECTOR screen = XMVector3Project(pos, viewport.x, viewport.y, viewport.z, viewport.w, 0.0f, 1.0f, projMatrix,
                                           viewMatrix, XMMatrixIdentity());
        XMFLOAT3 s;
        XMStoreFloat3(&s, screen);
        return {s.x, s.y};
    }

    static ImU32 XMFloat4ToImU32(const XMFLOAT4& c)
    {
        return IM_COL32(static_cast<int>(c.x * 255.0f), static_cast<int>(c.y * 255.0f), static_cast<int>(c.z * 255.0f),
                        static_cast<int>(c.w * 255.0f));
    }

    static float PointToLineDistance2D(const ImVec2& point, const ImVec2& lineStart, const ImVec2& lineEnd)
    {
        float dx = lineEnd.x - lineStart.x;
        float dy = lineEnd.y - lineStart.y;
        float lenSq = dx * dx + dy * dy;
        if (lenSq < 1e-6f)
        {
            float ex = point.x - lineStart.x;
            float ey = point.y - lineStart.y;
            return std::sqrt(ex * ex + ey * ey);
        }
        float t = std::clamp(((point.x - lineStart.x) * dx + (point.y - lineStart.y) * dy) / lenSq, 0.0f, 1.0f);
        float projX = lineStart.x + t * dx;
        float projY = lineStart.y + t * dy;
        float ex = point.x - projX;
        float ey = point.y - projY;
        return std::sqrt(ex * ex + ey * ey);
    }

    static float PointToCircleDistance2D(const ImVec2& point, const ImVec2& center, float radius)
    {
        float dx = point.x - center.x;
        float dy = point.y - center.y;
        float dist = std::sqrt(dx * dx + dy * dy);
        return std::abs(dist - radius);
    }

    // ========================================================================
    // Ray
    // ========================================================================

    Ray Ray::ScreenToWorldRay(float screenX, float screenY, const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix,
                              const XMFLOAT4& viewport)
    {
        // Unproject near and far points to build the ray
        XMVECTOR nearPoint =
            XMVector3Unproject(XMVectorSet(screenX, screenY, 0.0f, 1.0f), viewport.x, viewport.y, viewport.z,
                               viewport.w, 0.0f, 1.0f, projMatrix, viewMatrix, XMMatrixIdentity());

        XMVECTOR farPoint =
            XMVector3Unproject(XMVectorSet(screenX, screenY, 1.0f, 1.0f), viewport.x, viewport.y, viewport.z,
                               viewport.w, 0.0f, 1.0f, projMatrix, viewMatrix, XMMatrixIdentity());

        XMVECTOR direction = XMVector3Normalize(XMVectorSubtract(farPoint, nearPoint));

        Ray ray;
        XMStoreFloat3(&ray.origin, nearPoint);
        XMStoreFloat3(&ray.direction, direction);
        return ray;
    }

    // ========================================================================
    // GizmoSystem lifetime
    // ========================================================================

    GizmoSystem::GizmoSystem() = default;

    GizmoSystem::~GizmoSystem() = default;

    bool GizmoSystem::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        m_device = device;
        m_context = context;

        if (!CreateGizmoGeometry())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Failed to create gizmo geometry");
            return false;
        }
        if (!CreateGizmoShaders())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Failed to create gizmo shaders");
            return false;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Editor, "GizmoSystem initialized");
        return true;
    }

    void GizmoSystem::Shutdown()
    {
        m_device = nullptr;
        m_context = nullptr;
    }

    // ========================================================================
    // Update
    // ========================================================================

    void GizmoSystem::Update(float /*deltaTime*/)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        // Animation timers could be updated here for gizmo pulse effects.
        // Currently no animated state is required.
    }

    // ========================================================================
    // Render
    // ========================================================================

    void GizmoSystem::Render(const std::vector<Transform*>& selectedObjects, const XMMATRIX& viewMatrix,
                             const XMMATRIX& projMatrix, const XMFLOAT4& viewport)
    {
        if (!m_isVisible || selectedObjects.empty())
        {
            return;
        }

        // Build a combined transform at the gizmo centre
        XMFLOAT3 center = CalculateGizmoCenter(selectedObjects);
        Transform gizmoTransform;
        gizmoTransform.position = center;
        gizmoTransform.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
        gizmoTransform.scale = {1.0f, 1.0f, 1.0f};

        (void)viewport; // viewport used indirectly via member render helpers

        switch (m_currentMode)
        {
        case GizmoMode::TRANSLATE:
            RenderTranslationGizmo(gizmoTransform, viewMatrix, projMatrix);
            break;
        case GizmoMode::ROTATE:
            RenderRotationGizmo(gizmoTransform, viewMatrix, projMatrix);
            break;
        case GizmoMode::SCALE:
            RenderScaleGizmo(gizmoTransform, viewMatrix, projMatrix);
            break;
        case GizmoMode::UNIVERSAL:
            RenderTranslationGizmo(gizmoTransform, viewMatrix, projMatrix);
            RenderRotationGizmo(gizmoTransform, viewMatrix, projMatrix);
            RenderScaleGizmo(gizmoTransform, viewMatrix, projMatrix);
            break;
        }
    }

    // ========================================================================
    // HandleMouseInput
    // ========================================================================

    bool GizmoSystem::HandleMouseInput(float mouseX, float mouseY, bool isMouseDown, const XMMATRIX& viewMatrix,
                                       const XMMATRIX& projMatrix, const XMFLOAT4& viewport,
                                       std::vector<Transform*>& selectedObjects)
    {
        if (!m_isVisible || selectedObjects.empty())
        {
            return false;
        }

        Ray ray = Ray::ScreenToWorldRay(mouseX, mouseY, viewMatrix, projMatrix, viewport);

        // Cache matrices for hit testing
        m_cachedView = viewMatrix;
        m_cachedProj = projMatrix;

        XMFLOAT3 center = CalculateGizmoCenter(selectedObjects);
        Transform gizmoTransform;
        gizmoTransform.position = center;
        gizmoTransform.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
        gizmoTransform.scale = {1.0f, 1.0f, 1.0f};

        if (!isMouseDown)
        {
            // Hover test
            GizmoAxis hitAxis = GizmoAxis::NONE;
            switch (m_currentMode)
            {
            case GizmoMode::TRANSLATE:
                hitAxis = TestTranslationGizmoHit(ray, gizmoTransform);
                break;
            case GizmoMode::ROTATE:
                hitAxis = TestRotationGizmoHit(ray, gizmoTransform);
                break;
            case GizmoMode::SCALE:
                hitAxis = TestScaleGizmoHit(ray, gizmoTransform);
                break;
            case GizmoMode::UNIVERSAL:
                hitAxis = TestTranslationGizmoHit(ray, gizmoTransform);
                break;
            }
            m_hoveredAxis = hitAxis;

            if (m_isDragging)
            {
                // Mouse released -- end drag
                m_isDragging = false;
                m_interaction.isActive = false;
                m_interaction.isDragging = false;
            }
            return hitAxis != GizmoAxis::NONE;
        }

        // Mouse is down
        if (!m_isDragging)
        {
            // Begin drag if hovering over an axis
            if (m_hoveredAxis != GizmoAxis::NONE)
            {
                SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Gizmo drag started on axis %d in mode %d",
                                static_cast<int>(m_hoveredAxis), static_cast<int>(m_currentMode));
                m_isDragging = true;
                m_interaction.isActive = true;
                m_interaction.isDragging = true;
                m_interaction.activeAxis = m_hoveredAxis;
                m_interaction.startPosition = center;
                m_interaction.currentDelta = {0.0f, 0.0f, 0.0f};
                m_interaction.totalDelta = 0.0f;
                m_lastMouseWorldPos = ray.origin;
                m_interactionStartPos = center;
            }
        }

        if (m_isDragging)
        {
            // Compute world-space delta along the ray direction
            XMVECTOR curOrigin = XMLoadFloat3(&ray.origin);
            XMVECTOR lastOrigin = XMLoadFloat3(&m_lastMouseWorldPos);
            XMVECTOR deltaVec = XMVectorSubtract(curOrigin, lastOrigin);
            XMFLOAT3 delta;
            XMStoreFloat3(&delta, deltaVec);

            switch (m_currentMode)
            {
            case GizmoMode::TRANSLATE:
            case GizmoMode::UNIVERSAL:
            {
                // Constrain delta to the active axis
                XMFLOAT3 constrainedDelta = {0.0f, 0.0f, 0.0f};
                if (m_interaction.activeAxis == GizmoAxis::X)
                {
                    constrainedDelta.x = delta.x;
                }
                else if (m_interaction.activeAxis == GizmoAxis::Y)
                {
                    constrainedDelta.y = delta.y;
                }
                else if (m_interaction.activeAxis == GizmoAxis::Z)
                {
                    constrainedDelta.z = delta.z;
                }
                else
                {
                    constrainedDelta = delta;
                }

                if (m_snapToGrid)
                {
                    constrainedDelta.x = SnapToGrid(constrainedDelta.x);
                    constrainedDelta.y = SnapToGrid(constrainedDelta.y);
                    constrainedDelta.z = SnapToGrid(constrainedDelta.z);
                }

                ApplyTranslation(constrainedDelta, selectedObjects);
                m_interaction.currentDelta = constrainedDelta;
                break;
            }
            case GizmoMode::ROTATE:
            {
                float angleDelta = (delta.x + delta.y) * 0.01f;
                if (m_snapToGrid)
                {
                    angleDelta = SnapToRotation(angleDelta);
                }
                ApplyRotation(m_interaction.activeAxis, angleDelta, selectedObjects);
                m_interaction.totalDelta += angleDelta;
                break;
            }
            case GizmoMode::SCALE:
            {
                float scaleFactor = 1.0f + (delta.x + delta.y) * 0.01f;
                XMFLOAT3 scaleVec = {scaleFactor, scaleFactor, scaleFactor};
                ApplyScale(scaleVec, m_interaction.activeAxis, selectedObjects);
                break;
            }
            }

            m_lastMouseWorldPos = ray.origin;
            return true;
        }

        return false;
    }

    // ========================================================================
    // Render helpers (ImGui draw list based)
    // ========================================================================

    void GizmoSystem::RenderTranslationGizmo(const Transform& transform, const XMMATRIX& viewMatrix,
                                             const XMMATRIX& projMatrix)
    {
        ImDrawList* drawList = ImGui::GetForegroundDrawList();
        if (!drawList)
        {
            return;
        }

        XMFLOAT4 vp = {0.0f, 0.0f, ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y};

        XMFLOAT2 center = WorldToScreen(transform.position, viewMatrix, projMatrix, vp);
        float size = CalculateAdaptiveSize(transform.position, viewMatrix) * GIZMO_AXIS_LENGTH;

        // X axis (red arrow)
        {
            XMFLOAT3 axisEnd = {transform.position.x + m_gizmoSize, transform.position.y, transform.position.z};
            XMFLOAT2 end = WorldToScreen(axisEnd, viewMatrix, projMatrix, vp);
            // Extend to fixed pixel length from centre
            float dx = end.x - center.x;
            float dy = end.y - center.y;
            float len = std::sqrt(dx * dx + dy * dy);
            if (len > 1e-4f)
            {
                end.x = center.x + (dx / len) * size;
                end.y = center.y + (dy / len) * size;
            }
            XMFLOAT4 color = GetAxisColor(GizmoAxis::X, m_hoveredAxis == GizmoAxis::X,
                                          m_interaction.activeAxis == GizmoAxis::X && m_isDragging);
            ImU32 col = XMFloat4ToImU32(color);
            drawList->AddLine(ImVec2(center.x, center.y), ImVec2(end.x, end.y), col, 2.0f);
            // Arrowhead
            float ndx = (end.x - center.x) / size;
            float ndy = (end.y - center.y) / size;
            ImVec2 arrowA = {end.x - ndx * GIZMO_ARROW_SIZE + ndy * GIZMO_ARROW_SIZE * 0.5f,
                             end.y - ndy * GIZMO_ARROW_SIZE - ndx * GIZMO_ARROW_SIZE * 0.5f};
            ImVec2 arrowB = {end.x - ndx * GIZMO_ARROW_SIZE - ndy * GIZMO_ARROW_SIZE * 0.5f,
                             end.y - ndy * GIZMO_ARROW_SIZE + ndx * GIZMO_ARROW_SIZE * 0.5f};
            drawList->AddTriangleFilled(ImVec2(end.x, end.y), arrowA, arrowB, col);
        }

        // Y axis (green arrow)
        {
            XMFLOAT3 axisEnd = {transform.position.x, transform.position.y + m_gizmoSize, transform.position.z};
            XMFLOAT2 end = WorldToScreen(axisEnd, viewMatrix, projMatrix, vp);
            float dx = end.x - center.x;
            float dy = end.y - center.y;
            float len = std::sqrt(dx * dx + dy * dy);
            if (len > 1e-4f)
            {
                end.x = center.x + (dx / len) * size;
                end.y = center.y + (dy / len) * size;
            }
            XMFLOAT4 color = GetAxisColor(GizmoAxis::Y, m_hoveredAxis == GizmoAxis::Y,
                                          m_interaction.activeAxis == GizmoAxis::Y && m_isDragging);
            ImU32 col = XMFloat4ToImU32(color);
            drawList->AddLine(ImVec2(center.x, center.y), ImVec2(end.x, end.y), col, 2.0f);
            float ndx = (end.x - center.x) / size;
            float ndy = (end.y - center.y) / size;
            ImVec2 arrowA = {end.x - ndx * GIZMO_ARROW_SIZE + ndy * GIZMO_ARROW_SIZE * 0.5f,
                             end.y - ndy * GIZMO_ARROW_SIZE - ndx * GIZMO_ARROW_SIZE * 0.5f};
            ImVec2 arrowB = {end.x - ndx * GIZMO_ARROW_SIZE - ndy * GIZMO_ARROW_SIZE * 0.5f,
                             end.y - ndy * GIZMO_ARROW_SIZE + ndx * GIZMO_ARROW_SIZE * 0.5f};
            drawList->AddTriangleFilled(ImVec2(end.x, end.y), arrowA, arrowB, col);
        }

        // Z axis (blue arrow)
        {
            XMFLOAT3 axisEnd = {transform.position.x, transform.position.y, transform.position.z + m_gizmoSize};
            XMFLOAT2 end = WorldToScreen(axisEnd, viewMatrix, projMatrix, vp);
            float dx = end.x - center.x;
            float dy = end.y - center.y;
            float len = std::sqrt(dx * dx + dy * dy);
            if (len > 1e-4f)
            {
                end.x = center.x + (dx / len) * size;
                end.y = center.y + (dy / len) * size;
            }
            XMFLOAT4 color = GetAxisColor(GizmoAxis::Z, m_hoveredAxis == GizmoAxis::Z,
                                          m_interaction.activeAxis == GizmoAxis::Z && m_isDragging);
            ImU32 col = XMFloat4ToImU32(color);
            drawList->AddLine(ImVec2(center.x, center.y), ImVec2(end.x, end.y), col, 2.0f);
            float ndx = (end.x - center.x) / size;
            float ndy = (end.y - center.y) / size;
            ImVec2 arrowA = {end.x - ndx * GIZMO_ARROW_SIZE + ndy * GIZMO_ARROW_SIZE * 0.5f,
                             end.y - ndy * GIZMO_ARROW_SIZE - ndx * GIZMO_ARROW_SIZE * 0.5f};
            ImVec2 arrowB = {end.x - ndx * GIZMO_ARROW_SIZE - ndy * GIZMO_ARROW_SIZE * 0.5f,
                             end.y - ndy * GIZMO_ARROW_SIZE + ndx * GIZMO_ARROW_SIZE * 0.5f};
            drawList->AddTriangleFilled(ImVec2(end.x, end.y), arrowA, arrowB, col);
        }
    }

    void GizmoSystem::RenderRotationGizmo(const Transform& transform, const XMMATRIX& viewMatrix,
                                          const XMMATRIX& projMatrix)
    {
        ImDrawList* drawList = ImGui::GetForegroundDrawList();
        if (!drawList)
        {
            return;
        }

        XMFLOAT4 vp = {0.0f, 0.0f, ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y};

        XMFLOAT2 center = WorldToScreen(transform.position, viewMatrix, projMatrix, vp);
        float adaptiveSize = CalculateAdaptiveSize(transform.position, viewMatrix);
        float radius = adaptiveSize * GIZMO_RING_RADIUS;

        // Draw three circles representing rotation around each axis.
        // For simplicity we draw screen-space circles (ellipses would require
        // projecting many points).

        // X-axis ring (rotate in YZ plane) -- draw as slightly squished circle
        {
            XMFLOAT4 color = GetAxisColor(GizmoAxis::X, m_hoveredAxis == GizmoAxis::X,
                                          m_interaction.activeAxis == GizmoAxis::X && m_isDragging);
            ImU32 col = XMFloat4ToImU32(color);
            for (int i = 0; i < GIZMO_RING_SEGMENTS; ++i)
            {
                float a0 = (static_cast<float>(i) / GIZMO_RING_SEGMENTS) * MathUtils::TWO_PI;
                float a1 = (static_cast<float>(i + 1) / GIZMO_RING_SEGMENTS) * MathUtils::TWO_PI;
                ImVec2 p0 = {center.x + std::cos(a0) * radius * 0.3f, center.y + std::sin(a0) * radius};
                ImVec2 p1 = {center.x + std::cos(a1) * radius * 0.3f, center.y + std::sin(a1) * radius};
                drawList->AddLine(p0, p1, col, 2.0f);
            }
        }

        // Y-axis ring (rotate in XZ plane)
        {
            XMFLOAT4 color = GetAxisColor(GizmoAxis::Y, m_hoveredAxis == GizmoAxis::Y,
                                          m_interaction.activeAxis == GizmoAxis::Y && m_isDragging);
            ImU32 col = XMFloat4ToImU32(color);
            for (int i = 0; i < GIZMO_RING_SEGMENTS; ++i)
            {
                float a0 = (static_cast<float>(i) / GIZMO_RING_SEGMENTS) * MathUtils::TWO_PI;
                float a1 = (static_cast<float>(i + 1) / GIZMO_RING_SEGMENTS) * MathUtils::TWO_PI;
                ImVec2 p0 = {center.x + std::cos(a0) * radius, center.y + std::sin(a0) * radius * 0.3f};
                ImVec2 p1 = {center.x + std::cos(a1) * radius, center.y + std::sin(a1) * radius * 0.3f};
                drawList->AddLine(p0, p1, col, 2.0f);
            }
        }

        // Z-axis ring (rotate in XY plane) -- full circle
        {
            XMFLOAT4 color = GetAxisColor(GizmoAxis::Z, m_hoveredAxis == GizmoAxis::Z,
                                          m_interaction.activeAxis == GizmoAxis::Z && m_isDragging);
            ImU32 col = XMFloat4ToImU32(color);
            drawList->AddCircle(ImVec2(center.x, center.y), radius, col, GIZMO_RING_SEGMENTS, 2.0f);
        }
    }

    void GizmoSystem::RenderScaleGizmo(const Transform& transform, const XMMATRIX& viewMatrix,
                                       const XMMATRIX& projMatrix)
    {
        ImDrawList* drawList = ImGui::GetForegroundDrawList();
        if (!drawList)
        {
            return;
        }

        XMFLOAT4 vp = {0.0f, 0.0f, ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y};

        XMFLOAT2 center = WorldToScreen(transform.position, viewMatrix, projMatrix, vp);
        float size = CalculateAdaptiveSize(transform.position, viewMatrix) * GIZMO_AXIS_LENGTH;

        auto drawAxisWithCube = [&](GizmoAxis axis, const XMFLOAT3& axisEndWorld)
        {
            XMFLOAT2 end = WorldToScreen(axisEndWorld, viewMatrix, projMatrix, vp);
            float dx = end.x - center.x;
            float dy = end.y - center.y;
            float len = std::sqrt(dx * dx + dy * dy);
            if (len > 1e-4f)
            {
                end.x = center.x + (dx / len) * size;
                end.y = center.y + (dy / len) * size;
            }
            XMFLOAT4 color =
                GetAxisColor(axis, m_hoveredAxis == axis, m_interaction.activeAxis == axis && m_isDragging);
            ImU32 col = XMFloat4ToImU32(color);
            drawList->AddLine(ImVec2(center.x, center.y), ImVec2(end.x, end.y), col, 2.0f);
            // Draw cube at end
            float half = GIZMO_CUBE_SIZE;
            drawList->AddRectFilled(ImVec2(end.x - half, end.y - half), ImVec2(end.x + half, end.y + half), col);
        };

        drawAxisWithCube(GizmoAxis::X,
                         {transform.position.x + m_gizmoSize, transform.position.y, transform.position.z});
        drawAxisWithCube(GizmoAxis::Y,
                         {transform.position.x, transform.position.y + m_gizmoSize, transform.position.z});
        drawAxisWithCube(GizmoAxis::Z,
                         {transform.position.x, transform.position.y, transform.position.z + m_gizmoSize});
    }

    // ========================================================================
    // Hit testing
    // ========================================================================

    GizmoAxis GizmoSystem::TestTranslationGizmoHit(const Ray& /*ray*/, const Transform& transform)
    {
        ImVec2 mouse = ImGui::GetIO().MousePos;
        XMFLOAT4 vp = {0.0f, 0.0f, ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y};

        XMFLOAT2 center = WorldToScreen(transform.position, m_cachedView, m_cachedProj, vp);
        float size = CalculateAdaptiveSize(transform.position, m_cachedView) * GIZMO_AXIS_LENGTH;

        // Project each axis endpoint and test distance from mouse to the line
        struct AxisTest
        {
            GizmoAxis axis;
            XMFLOAT3 endWorld;
        };
        AxisTest axes[] = {
            {GizmoAxis::X, {transform.position.x + m_gizmoSize, transform.position.y, transform.position.z}},
            {GizmoAxis::Y, {transform.position.x, transform.position.y + m_gizmoSize, transform.position.z}},
            {GizmoAxis::Z, {transform.position.x, transform.position.y, transform.position.z + m_gizmoSize}},
        };

        GizmoAxis closest = GizmoAxis::NONE;
        float closestDist = GIZMO_HIT_THRESHOLD;

        for (const auto& ax : axes)
        {
            XMFLOAT2 end = WorldToScreen(ax.endWorld, m_cachedView, m_cachedProj, vp);
            // Normalize to fixed pixel length
            float dx = end.x - center.x;
            float dy = end.y - center.y;
            float len = std::sqrt(dx * dx + dy * dy);
            if (len > 1e-4f)
            {
                end.x = center.x + (dx / len) * size;
                end.y = center.y + (dy / len) * size;
            }
            float dist =
                PointToLineDistance2D(ImVec2(mouse.x, mouse.y), ImVec2(center.x, center.y), ImVec2(end.x, end.y));
            if (dist < closestDist)
            {
                closestDist = dist;
                closest = ax.axis;
            }
        }
        return closest;
    }

    GizmoAxis GizmoSystem::TestRotationGizmoHit(const Ray& /*ray*/, const Transform& transform)
    {
        ImVec2 mouse = ImGui::GetIO().MousePos;
        XMFLOAT4 vp = {0.0f, 0.0f, ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y};

        XMFLOAT2 center = WorldToScreen(transform.position, m_cachedView, m_cachedProj, vp);
        float adaptiveSize = CalculateAdaptiveSize(transform.position, m_cachedView);
        float radius = adaptiveSize * GIZMO_RING_RADIUS;

        GizmoAxis closest = GizmoAxis::NONE;
        float closestDist = GIZMO_HIT_THRESHOLD;

        // X-axis ring (squished horizontally — 0.3x width, full height)
        {
            float dx = (mouse.x - center.x) / 0.3f;
            float dy = mouse.y - center.y;
            float dist = std::abs(std::sqrt(dx * dx + dy * dy) - radius);
            if (dist < closestDist)
            {
                closestDist = dist;
                closest = GizmoAxis::X;
            }
        }

        // Y-axis ring (full width, squished vertically — 0.3x height)
        {
            float dx = mouse.x - center.x;
            float dy = (mouse.y - center.y) / 0.3f;
            float dist = std::abs(std::sqrt(dx * dx + dy * dy) - radius);
            if (dist < closestDist)
            {
                closestDist = dist;
                closest = GizmoAxis::Y;
            }
        }

        // Z-axis ring (full circle)
        {
            float dist = PointToCircleDistance2D(ImVec2(mouse.x, mouse.y), ImVec2(center.x, center.y), radius);
            if (dist < closestDist)
            {
                closestDist = dist;
                closest = GizmoAxis::Z;
            }
        }
        return closest;
    }

    GizmoAxis GizmoSystem::TestScaleGizmoHit(const Ray& /*ray*/, const Transform& transform)
    {
        // Scale gizmo uses the same line-based axes as translation
        // Reuse the same screen-space line distance approach
        ImVec2 mouse = ImGui::GetIO().MousePos;
        XMFLOAT4 vp = {0.0f, 0.0f, ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y};

        XMFLOAT2 center = WorldToScreen(transform.position, m_cachedView, m_cachedProj, vp);
        float size = CalculateAdaptiveSize(transform.position, m_cachedView) * GIZMO_AXIS_LENGTH;

        struct AxisTest
        {
            GizmoAxis axis;
            XMFLOAT3 endWorld;
        };
        AxisTest axes[] = {
            {GizmoAxis::X, {transform.position.x + m_gizmoSize, transform.position.y, transform.position.z}},
            {GizmoAxis::Y, {transform.position.x, transform.position.y + m_gizmoSize, transform.position.z}},
            {GizmoAxis::Z, {transform.position.x, transform.position.y, transform.position.z + m_gizmoSize}},
        };

        GizmoAxis closest = GizmoAxis::NONE;
        float closestDist = GIZMO_HIT_THRESHOLD;

        for (const auto& ax : axes)
        {
            XMFLOAT2 end = WorldToScreen(ax.endWorld, m_cachedView, m_cachedProj, vp);
            float dx = end.x - center.x;
            float dy = end.y - center.y;
            float len = std::sqrt(dx * dx + dy * dy);
            if (len > 1e-4f)
            {
                end.x = center.x + (dx / len) * size;
                end.y = center.y + (dy / len) * size;
            }
            float dist =
                PointToLineDistance2D(ImVec2(mouse.x, mouse.y), ImVec2(center.x, center.y), ImVec2(end.x, end.y));
            if (dist < closestDist)
            {
                closestDist = dist;
                closest = ax.axis;
            }
        }
        return closest;
    }

    // ========================================================================
    // Transform application
    // ========================================================================

    void GizmoSystem::ApplyTranslation(const XMFLOAT3& delta, std::vector<Transform*>& transforms)
    {
        SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Applying translation delta (%.3f, %.3f, %.3f) to %zu objects",
                        delta.x, delta.y, delta.z, transforms.size());
        for (auto* t : transforms)
        {
            if (t)
            {
                t->position.x += delta.x;
                t->position.y += delta.y;
                t->position.z += delta.z;
            }
        }
    }

    void GizmoSystem::ApplyRotation(GizmoAxis axis, float angleDelta, std::vector<Transform*>& transforms)
    {
        SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Applying rotation delta %.3f rad on axis %d to %zu objects",
                        angleDelta, static_cast<int>(axis), transforms.size());
        XMVECTOR rotAxis = XMVectorSet(0.0f, 0.0f, 0.0f, 0.0f);
        switch (axis)
        {
        case GizmoAxis::X:
            rotAxis = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
            break;
        case GizmoAxis::Y:
            rotAxis = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
            break;
        case GizmoAxis::Z:
            rotAxis = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
            break;
        default:
            return;
        }

        XMVECTOR deltaQuat = XMQuaternionRotationAxis(rotAxis, angleDelta);

        for (auto* t : transforms)
        {
            if (t)
            {
                XMVECTOR currentRot = XMLoadFloat4(&t->rotation);
                XMVECTOR newRot = XMQuaternionMultiply(currentRot, deltaQuat);
                newRot = XMQuaternionNormalize(newRot);
                XMStoreFloat4(&t->rotation, newRot);
            }
        }
    }

    void GizmoSystem::ApplyScale(const XMFLOAT3& scale, GizmoAxis axis, std::vector<Transform*>& transforms)
    {
        SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Applying scale (%.3f, %.3f, %.3f) on axis %d to %zu objects",
                        scale.x, scale.y, scale.z, static_cast<int>(axis), transforms.size());
        for (auto* t : transforms)
        {
            if (t)
            {
                if (axis == GizmoAxis::X || axis == GizmoAxis::XYZ || axis == GizmoAxis::NONE)
                {
                    t->scale.x *= scale.x;
                }
                if (axis == GizmoAxis::Y || axis == GizmoAxis::XYZ || axis == GizmoAxis::NONE)
                {
                    t->scale.y *= scale.y;
                }
                if (axis == GizmoAxis::Z || axis == GizmoAxis::XYZ || axis == GizmoAxis::NONE)
                {
                    t->scale.z *= scale.z;
                }
            }
        }
    }

    // ========================================================================
    // Snapping
    // ========================================================================

    float GizmoSystem::SnapToGrid(float value) const
    {
        if (!m_snapToGrid || m_snapSize <= 0.0f)
        {
            return value;
        }
        return std::round(value / m_snapSize) * m_snapSize;
    }

    float GizmoSystem::SnapToRotation(float angle) const
    {
        if (m_rotationSnapAngle <= 0.0f)
        {
            return angle;
        }
        float snapRad = MathUtils::DegreesToRadians(m_rotationSnapAngle);
        return std::round(angle / snapRad) * snapRad;
    }

    // ========================================================================
    // Utility
    // ========================================================================

    XMFLOAT3 GizmoSystem::CalculateGizmoCenter(const std::vector<Transform*>& transforms) const
    {
        if (transforms.empty())
        {
            return {0.0f, 0.0f, 0.0f};
        }

        XMVECTOR sum = XMVectorZero();
        int count = 0;
        for (const auto* t : transforms)
        {
            if (t)
            {
                sum = XMVectorAdd(sum, XMLoadFloat3(&t->position));
                ++count;
            }
        }

        if (count == 0)
        {
            return {0.0f, 0.0f, 0.0f};
        }

        sum = XMVectorScale(sum, 1.0f / static_cast<float>(count));
        XMFLOAT3 result;
        XMStoreFloat3(&result, sum);
        return result;
    }

    float GizmoSystem::CalculateAdaptiveSize(const XMFLOAT3& gizmoPosition, const XMMATRIX& viewMatrix) const
    {
        // Transform the gizmo position into view space to get the distance from camera
        XMVECTOR pos = XMLoadFloat3(&gizmoPosition);
        XMVECTOR viewPos = XMVector3TransformCoord(pos, viewMatrix);
        XMFLOAT3 vp;
        XMStoreFloat3(&vp, viewPos);

        float distance = std::abs(vp.z);
        if (distance < 0.1f)
        {
            distance = 0.1f;
        }

        // Scale so gizmo appears roughly the same size regardless of camera distance
        return m_gizmoSize * (distance * 0.1f);
    }

    XMFLOAT4 GizmoSystem::GetAxisColor(GizmoAxis axis, bool isHighlighted, bool isSelected) const
    {
        if (isSelected)
        {
            return COLOR_SELECTED;
        }
        if (isHighlighted)
        {
            return COLOR_HIGHLIGHTED;
        }

        switch (axis)
        {
        case GizmoAxis::X:
            return COLOR_X_AXIS;
        case GizmoAxis::Y:
            return COLOR_Y_AXIS;
        case GizmoAxis::Z:
            return COLOR_Z_AXIS;
        default:
            return {1.0f, 1.0f, 1.0f, 1.0f};
        }
    }

    // ========================================================================
    // Resource creation (ImGui-based -- no GPU resources needed)
    // ========================================================================

    bool GizmoSystem::CreateGizmoGeometry()
    {
        // All rendering is done through ImGui draw lists, so no GPU geometry
        // buffers are required.
        return true;
    }

    bool GizmoSystem::CreateGizmoShaders()
    {
        // All rendering is done through ImGui draw lists, so no compiled
        // shaders are required.
        return true;
    }

} // namespace SparkEditor
