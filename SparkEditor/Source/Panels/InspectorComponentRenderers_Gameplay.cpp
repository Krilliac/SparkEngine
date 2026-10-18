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
#include "SparkUI/Runtime.h"
#include "SparkUI/Widgets.h"
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
        bool headerOpen = Spark::Ui::SectionHeader("Spline", ICON_FA_BEZIER_CURVE, false);
        if (ImGui::BeginPopupContextItem("##SplineCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
                RemoveComponent(ComponentType::SPLINE);
            ImGui::EndPopup();
        }
        if (headerOpen)
        {
            ImGui::Indent(Spark::Ui::Px(4.0f));
            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::SPLINE);
            SplineData* spline = comp ? comp->GetData<SplineData>() : nullptr;
            if (spline)
            {
                if (Spark::Ui::BeginPropertyGrid("##SplineProps"))
                {
                    Spark::Ui::PropertyLabel("Debug Visible");
                    ImGui::Checkbox("##DebugVisible", &spline->debugVisible);
                    Spark::Ui::PropertyLabel("Closed Loop");
                    ImGui::Checkbox("##ClosedLoop", &spline->closed);
                    Spark::Ui::PropertyLabel("Control Points");
                    Spark::Ui::MonoText("%d", spline->pointCount);
                    Spark::Ui::SecondaryText("Edit points in the Spline Editor panel");
                    Spark::Ui::EndPropertyGrid();
                }
            }
            else
            {
                Spark::Ui::StatusText(Spark::Ui::Status::Warning, "Component data unavailable");
            }
            ImGui::Unindent(Spark::Ui::Px(4.0f));
        }
    }

    // Spline Follower — migrated to InspectorComponentRenderers_Reflected.cpp

    // Projectile, Interaction — migrated to InspectorComponentRenderers_Reflected.cpp
    // (uses conditional field visibility for impact-behavior and interaction-type dependent fields)

} // namespace SparkEditor
