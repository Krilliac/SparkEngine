/**
 * @file InspectorComponentRenderers_Gameplay.cpp
 * @brief Inspector renderers for gameplay components with complex conditional UI
 *
 * Contains: Spline, Projectile, Interaction.
 * Simpler components migrated to InspectorComponentRenderers_Reflected.cpp:
 * AnimationController, Script, Health, AIAgent, SplineFollower, Weather,
 * NetworkIdentity, Decal, ParticleEmitter.
 */

#include "InspectorPanel.h"
#include "../Core/EditorIcons.h"
#include "../Core/EditorFonts.h"
#include "../CommandHistory.h"
#include "Utils/LogMacros.h"
#include "Utils/MathUtils.h"
#include <imgui.h>
#include <algorithm>
#include <cinttypes>
#include <cstring>

namespace SparkEditor
{

    // ParticleEmitter, AnimationController, Script, Health, AIAgent, Decal — migrated
    // to InspectorComponentRenderers_Reflected.cpp (reflection-driven rendering)

    // ============================================================================
    // Spline Component
    // ============================================================================

    void InspectorPanel::RenderSplineComponent()
    {
        bool headerOpen = ImGui::CollapsingHeader(ICON_FA_BEZIER_CURVE " Spline");
        if (ImGui::BeginPopupContextItem("##SplineCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
                RemoveComponent(ComponentType::SPLINE);
            ImGui::EndPopup();
        }
        if (headerOpen)
        {
            ImGui::Indent(4);
            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::SPLINE);
            SplineData* spline = comp ? comp->GetData<SplineData>() : nullptr;
            if (spline)
            {
                ImGui::Checkbox("Debug Visible", &spline->debugVisible);
                ImGui::Checkbox("Closed Loop", &spline->closed);
                ImGui::Text("Control Points: %d", spline->pointCount);
                ImGui::TextDisabled("Edit points in the Spline Editor panel");
            }
            else
            {
                ImGui::TextDisabled("(Component data unavailable)");
            }
            ImGui::Unindent(4);
        }
    }

    // Spline Follower — migrated to InspectorComponentRenderers_Reflected.cpp

    // Projectile, Interaction — migrated to InspectorComponentRenderers_Reflected.cpp
    // (uses conditional field visibility for impact-behavior and interaction-type dependent fields)

} // namespace SparkEditor
