/**
 * @file InspectorComponentRenderers_Gameplay.cpp
 * @brief Inspector renderers for gameplay, animation, scripting, and networking components
 *
 * Contains: ParticleEmitter, AnimationController, Script, Health, AIAgent,
 * Spline, SplineFollower, Decal, Projectile, Interaction, Weather, NetworkIdentity.
 * Split from InspectorComponentRenderers.cpp for maintainability.
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

    // ============================================================================
    // Particle Emitter Component
    // ============================================================================

    void InspectorPanel::RenderParticleEmitterComponent()
    {
        bool headerOpen = ImGui::CollapsingHeader(ICON_FA_FIRE " Particle Emitter");
        if (ImGui::BeginPopupContextItem("##ParticleEmitterCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
                RemoveComponent(ComponentType::PARTICLE_SYSTEM);
            ImGui::EndPopup();
        }
        if (headerOpen)
        {
            ImGui::Indent(4);
            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::PARTICLE_SYSTEM);
            ParticleEmitterData* pe = comp ? comp->GetData<ParticleEmitterData>() : nullptr;
            if (pe)
            {
                ImGui::InputText("Effect Name", pe->effectName, sizeof(pe->effectName));
                ImGui::Checkbox("Auto Play", &pe->autoPlay);
                ImGui::SameLine();
                ImGui::Checkbox("Loop", &pe->loop);

                ImGui::Separator();
                ImGui::TextDisabled("Emission");
                ImGui::DragFloat("Rate", &pe->emissionRate, 1.0f, 0.0f, 10000.0f, "%.0f/s");
                ImGui::DragInt("Max Particles", &pe->maxParticles, 10.0f, 1, 100000);
                ImGui::DragFloat("Lifetime", &pe->lifetime, 0.1f, 0.01f, 60.0f, "%.2f s");

                ImGui::Separator();
                ImGui::TextDisabled("Initial Values");
                float color[4] = {pe->startColor.x, pe->startColor.y, pe->startColor.z, pe->startColor.w};
                if (ImGui::ColorEdit4("Start Color", color))
                    pe->startColor = {color[0], color[1], color[2], color[3]};

                ImGui::DragFloat("Start Size", &pe->startSize, 0.01f, 0.001f, 10.0f);
                ImGui::DragFloat("Start Speed", &pe->startSpeed, 0.1f, 0.0f, 100.0f);
                ImGui::DragFloat("Gravity", &pe->gravityMultiplier, 0.01f, -2.0f, 2.0f);
            }
            else
            {
                ImGui::TextDisabled("(Component data unavailable)");
            }
            ImGui::Unindent(4);
        }
    }

    // ============================================================================
    // Animation Controller Component
    // ============================================================================

    void InspectorPanel::RenderAnimationControllerComponent()
    {
        bool headerOpen = ImGui::CollapsingHeader(ICON_FA_RUNNING " Animation");
        if (ImGui::BeginPopupContextItem("##AnimationCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
                RemoveComponent(ComponentType::ANIMATION);
            ImGui::EndPopup();
        }
        if (headerOpen)
        {
            ImGui::Indent(4);
            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::ANIMATION);
            AnimationControllerData* anim = comp ? comp->GetData<AnimationControllerData>() : nullptr;
            if (anim)
            {
                ImGui::InputText("Default Clip", anim->defaultAnimation, sizeof(anim->defaultAnimation));
                ImGui::DragFloat("Speed", &anim->playbackSpeed, 0.01f, 0.0f, 10.0f);
                ImGui::Checkbox("Playing", &anim->playing);
                ImGui::SameLine();
                ImGui::Checkbox("Loop", &anim->loop);
            }
            else
            {
                ImGui::TextDisabled("(Component data unavailable)");
            }
            ImGui::Unindent(4);
        }
    }

    // ============================================================================
    // Script Component
    // ============================================================================

    void InspectorPanel::RenderScriptComponent()
    {
        bool headerOpen = ImGui::CollapsingHeader(ICON_FA_CODE " Script");
        if (ImGui::BeginPopupContextItem("##ScriptCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
            {
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "Inspector: removing Script from object %" PRIu64,
                               m_inspectedObjectID);
                RemoveComponent(ComponentType::SCRIPT);
            }
            ImGui::EndPopup();
        }
        if (headerOpen)
        {
            ImGui::Indent(4);
            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::SCRIPT);
            ScriptData* script = comp ? comp->GetData<ScriptData>() : nullptr;
            if (script)
            {
                ImGui::InputText("Script Path", script->scriptPath, sizeof(script->scriptPath));
                ImGui::InputText("Class Name", script->className, sizeof(script->className));
                ImGui::Checkbox("Auto Start", &script->autoStart);
            }
            else
            {
                ImGui::TextDisabled("(Component data unavailable)");
            }
            ImGui::Unindent(4);
        }
    }

    // ============================================================================
    // Health Component
    // ============================================================================

    void InspectorPanel::RenderHealthComponent()
    {
        bool headerOpen = ImGui::CollapsingHeader(ICON_FA_HEART " Health");
        if (ImGui::BeginPopupContextItem("##HealthCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
                RemoveComponent(ComponentType::HEALTH);
            ImGui::EndPopup();
        }
        if (headerOpen)
        {
            ImGui::Indent(4);
            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::HEALTH);
            HealthData* hp = comp ? comp->GetData<HealthData>() : nullptr;
            if (hp)
            {
                ImGui::DragFloat("Health", &hp->health, 1.0f, 0.0f, hp->maxHealth);
                ImGui::DragFloat("Max Health", &hp->maxHealth, 1.0f, 1.0f, 10000.0f);

                // Health bar preview
                float fraction = hp->maxHealth > 0.0f ? hp->health / hp->maxHealth : 0.0f;
                ImGui::ProgressBar(fraction, ImVec2(-1, 0), "");
            }
            else
            {
                ImGui::TextDisabled("(Component data unavailable)");
            }
            ImGui::Unindent(4);
        }
    }

    // ============================================================================
    // AI Agent Component
    // ============================================================================

    void InspectorPanel::RenderAIAgentComponent()
    {
        bool headerOpen = ImGui::CollapsingHeader(ICON_FA_BRAIN " AI Agent");
        if (ImGui::BeginPopupContextItem("##AIAgentCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
                RemoveComponent(ComponentType::AI_AGENT);
            ImGui::EndPopup();
        }
        if (headerOpen)
        {
            ImGui::Indent(4);
            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::AI_AGENT);
            AIAgentData* ai = comp ? comp->GetData<AIAgentData>() : nullptr;
            if (ai)
            {
                const char* states[] = {"Idle", "Patrolling", "Alert", "Combat", "Fleeing", "Dead"};
                ImGui::Combo("Initial State", &ai->aiState, states, IM_ARRAYSIZE(states));
                ImGui::InputText("Behavior Tree", ai->behaviorTreeName, sizeof(ai->behaviorTreeName));

                ImGui::Separator();
                ImGui::TextDisabled("Perception");
                ImGui::DragFloat("Detection Range", &ai->detectionRange, 0.5f, 0.0f, 200.0f);
                ImGui::DragFloat("Attack Range", &ai->attackRange, 0.5f, 0.0f, 100.0f);
                ImGui::DragFloat("Reaction Time", &ai->reactionTime, 0.05f, 0.0f, 5.0f, "%.2f s");

                ImGui::Separator();
                ImGui::TextDisabled("Movement");
                ImGui::DragFloat("Move Speed", &ai->moveSpeed, 0.1f, 0.0f, 50.0f);
                ImGui::SliderFloat("Accuracy", &ai->accuracy, 0.0f, 1.0f);
            }
            else
            {
                ImGui::TextDisabled("(Component data unavailable)");
            }
            ImGui::Unindent(4);
        }
    }

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

    // ============================================================================
    // Spline Follower Component
    // ============================================================================

    void InspectorPanel::RenderSplineFollowerComponent()
    {
        bool headerOpen = ImGui::CollapsingHeader(ICON_FA_ROUTE " Spline Follower");
        if (ImGui::BeginPopupContextItem("##SplineFollowerCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
                RemoveComponent(ComponentType::SPLINE_FOLLOWER);
            ImGui::EndPopup();
        }
        if (headerOpen)
        {
            ImGui::Indent(4);
            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::SPLINE_FOLLOWER);
            SplineFollowerData* sf = comp ? comp->GetData<SplineFollowerData>() : nullptr;
            if (sf)
            {
                ImGui::DragFloat("Speed", &sf->speed, 0.1f, 0.0f, 100.0f);

                const char* loopModes[] = {"Once", "Loop", "Ping-Pong"};
                ImGui::Combo("Loop Mode", &sf->loopMode, loopModes, IM_ARRAYSIZE(loopModes));

                ImGui::Checkbox("Playing", &sf->playing);
                ImGui::Checkbox("Orient to Path", &sf->orientToPath);
            }
            else
            {
                ImGui::TextDisabled("(Component data unavailable)");
            }
            ImGui::Unindent(4);
        }
    }

    // ============================================================================
    // Decal Component
    // ============================================================================

    void InspectorPanel::RenderDecalComponent()
    {
        bool headerOpen = ImGui::CollapsingHeader(ICON_FA_STAMP " Decal");
        if (ImGui::BeginPopupContextItem("##DecalCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
                RemoveComponent(ComponentType::DECAL);
            ImGui::EndPopup();
        }
        if (headerOpen)
        {
            ImGui::Indent(4);
            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::DECAL);
            DecalData* decal = comp ? comp->GetData<DecalData>() : nullptr;
            if (decal)
            {
                ImGui::InputText("Texture", decal->texturePath, sizeof(decal->texturePath));
                ImGui::InputText("Category", decal->category, sizeof(decal->category));

                float sz[3] = {decal->size.x, decal->size.y, decal->size.z};
                if (ImGui::DragFloat3("Size", sz, 0.01f, 0.001f, 10.0f))
                    decal->size = {sz[0], sz[1], sz[2]};

                float color[4] = {decal->color.x, decal->color.y, decal->color.z, decal->color.w};
                if (ImGui::ColorEdit4("Color", color))
                    decal->color = {color[0], color[1], color[2], color[3]};

                ImGui::DragFloat("Lifetime", &decal->lifetime, 1.0f, 0.0f, 300.0f, "%.0f s (0=permanent)");
                ImGui::DragFloat("Fade Duration", &decal->fadeOutDuration, 0.1f, 0.0f, 30.0f, "%.1f s");
                ImGui::Checkbox("Receive Lighting", &decal->receiveLighting);
                ImGui::DragInt("Sort Order", &decal->sortOrder);
            }
            else
            {
                ImGui::TextDisabled("(Component data unavailable)");
            }
            ImGui::Unindent(4);
        }
    }

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

    // ============================================================================
    // Weather Component
    // ============================================================================

    void InspectorPanel::RenderWeatherComponent()
    {
        bool headerOpen = ImGui::CollapsingHeader(ICON_FA_CLOUD_SUN " Weather Zone");
        if (ImGui::BeginPopupContextItem("##WeatherCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
                RemoveComponent(ComponentType::WEATHER);
            ImGui::EndPopup();
        }
        if (headerOpen)
        {
            ImGui::Indent(4);
            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::WEATHER);
            WeatherData* weather = comp ? comp->GetData<WeatherData>() : nullptr;
            if (weather)
            {
                const char* weatherTypes[] = {"Clear", "Cloudy", "Rain", "Snow", "Fog", "Storm"};
                ImGui::Combo("Type", &weather->weatherType, weatherTypes, IM_ARRAYSIZE(weatherTypes));

                ImGui::SliderFloat("Intensity", &weather->intensity, 0.0f, 1.0f);
                ImGui::DragFloat("Wind Speed", &weather->windSpeed, 0.1f, 0.0f, 50.0f);

                float windDir[3] = {weather->windDirection.x, weather->windDirection.y, weather->windDirection.z};
                if (ImGui::DragFloat3("Wind Dir", windDir, 0.1f, -1.0f, 1.0f))
                    weather->windDirection = {windDir[0], windDir[1], windDir[2]};

                ImGui::DragFloat("Transition Time", &weather->transitionTime, 0.1f, 0.0f, 30.0f, "%.1f s");
                ImGui::Checkbox("Enabled", &weather->enabled);
            }
            else
            {
                ImGui::TextDisabled("(Component data unavailable)");
            }
            ImGui::Unindent(4);
        }
    }

    // ============================================================================
    // Network Identity Component
    // ============================================================================

    void InspectorPanel::RenderNetworkIdentityComponent()
    {
        bool headerOpen = ImGui::CollapsingHeader(ICON_FA_NETWORK_WIRED " Network Identity");
        if (ImGui::BeginPopupContextItem("##NetworkIdentityCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
                RemoveComponent(ComponentType::NETWORK_IDENTITY);
            ImGui::EndPopup();
        }
        if (headerOpen)
        {
            ImGui::Indent(4);
            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::NETWORK_IDENTITY);
            NetworkIdentityData* net = comp ? comp->GetData<NetworkIdentityData>() : nullptr;
            if (net)
            {
                ImGui::Checkbox("Replicate Transform", &net->replicateTransform);
                ImGui::Checkbox("Replicate Health", &net->replicateHealth);
                ImGui::Checkbox("Local Authority", &net->isLocalAuthority);
            }
            else
            {
                ImGui::TextDisabled("(Component data unavailable)");
            }
            ImGui::Unindent(4);
        }
    }

} // namespace SparkEditor
