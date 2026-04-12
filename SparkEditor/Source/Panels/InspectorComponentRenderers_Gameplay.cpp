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

    // ============================================================================
    // Projectile Component
    // ============================================================================

    void InspectorPanel::RenderProjectileComponent()
    {
        bool headerOpen = ImGui::CollapsingHeader(ICON_FA_CROSSHAIRS " Projectile");
        if (ImGui::BeginPopupContextItem("##ProjectileCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
                RemoveComponent(ComponentType::PROJECTILE);
            ImGui::EndPopup();
        }
        if (headerOpen)
        {
            ImGui::Indent(4);
            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::PROJECTILE);
            ProjectileData* proj = comp ? comp->GetData<ProjectileData>() : nullptr;
            if (proj)
            {
                const char* moveTypes[] = {"Hitscan", "Ballistic"};
                ImGui::Combo("Movement", &proj->movementType, moveTypes, IM_ARRAYSIZE(moveTypes));

                const char* impactTypes[] = {"Destroy", "Bounce", "Pierce", "Stick"};
                ImGui::Combo("On Impact", &proj->impactBehavior, impactTypes, IM_ARRAYSIZE(impactTypes));

                ImGui::DragFloat("Speed", &proj->speed, 1.0f, 0.0f, 5000.0f);
                ImGui::DragFloat("Damage", &proj->damage, 1.0f, 0.0f, 1000.0f);
                ImGui::DragFloat("Gravity Scale", &proj->gravityScale, 0.1f, 0.0f, 5.0f);
                ImGui::DragFloat("Explosion Radius", &proj->explosionRadius, 0.5f, 0.0f, 100.0f);
                ImGui::DragFloat("Max Range", &proj->maxRange, 10.0f, 0.0f, 10000.0f);
                ImGui::DragFloat("Max Lifetime", &proj->maxLifetime, 0.5f, 0.0f, 60.0f, "%.1f s");

                if (proj->impactBehavior == 1) // Bounce
                    ImGui::DragInt("Bounces", &proj->bouncesRemaining, 1.0f, 0, 20);
                if (proj->impactBehavior == 2) // Pierce
                    ImGui::DragInt("Pierces", &proj->piercesRemaining, 1.0f, 0, 20);
            }
            else
            {
                ImGui::TextDisabled("(Component data unavailable)");
            }
            ImGui::Unindent(4);
        }
    }

    // ============================================================================
    // Interaction Component
    // ============================================================================

    void InspectorPanel::RenderInteractionComponent()
    {
        bool headerOpen = ImGui::CollapsingHeader(ICON_FA_HAND_POINTER " Interaction");
        if (ImGui::BeginPopupContextItem("##InteractionCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
                RemoveComponent(ComponentType::INTERACTION);
            ImGui::EndPopup();
        }
        if (headerOpen)
        {
            ImGui::Indent(4);
            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::INTERACTION);
            InteractionData* inter = comp ? comp->GetData<InteractionData>() : nullptr;
            if (inter)
            {
                const char* types[] = {"Use", "Pickup", "Hold", "Toggle"};
                ImGui::Combo("Type", &inter->interactionType, types, IM_ARRAYSIZE(types));

                ImGui::InputText("Display Name", inter->displayName, sizeof(inter->displayName));
                ImGui::InputText("Action Verb", inter->actionVerb, sizeof(inter->actionVerb));
                ImGui::DragFloat("Radius", &inter->interactionRadius, 0.1f, 0.0f, 50.0f);

                if (inter->interactionType == 2) // Hold
                    ImGui::DragFloat("Hold Duration", &inter->holdDuration, 0.1f, 0.0f, 30.0f, "%.1f s");

                ImGui::DragFloat("Cooldown", &inter->cooldownDuration, 0.1f, 0.0f, 30.0f, "%.1f s");
                ImGui::DragInt("Uses (-1=unlimited)", &inter->usesRemaining, 1.0f, -1, 100);
                ImGui::Checkbox("Show Highlight", &inter->showHighlight);
            }
            else
            {
                ImGui::TextDisabled("(Component data unavailable)");
            }
            ImGui::Unindent(4);
        }
    }

    // Weather, Network Identity — migrated to InspectorComponentRenderers_Reflected.cpp

} // namespace SparkEditor
