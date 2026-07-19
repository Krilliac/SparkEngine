/**
 * @file InspectorComponentRenderers_ReflectedGameplay.cpp
 * @brief Reflection-driven inspector renderers for gameplay components
 *
 * Split from InspectorComponentRenderers_Reflected.cpp. Contains the
 * renderers migrated from InspectorComponentRenderers_Gameplay.cpp:
 * AnimationController, Script, Health, SplineFollower, NetworkIdentity,
 * Weather, AIAgent, Decal, ParticleEmitter.
 */

#include "InspectorComponentRenderers_ReflectedInternal.h"

namespace SparkEditor
{

    // ============================================================================
    // Migrated from InspectorComponentRenderers_Gameplay.cpp
    // ============================================================================

    // ============================================================================
    // Animation Controller (migrated to reflection)
    // ============================================================================

    void InspectorPanel::RenderAnimationControllerComponent()
    {
        RENDER_REFLECTED_COMPONENT(ComponentType::ANIMATION, ICON_FA_RUNNING, "Animation", AnimationControllerData,
                                   FIELD_STRING(AnimationControllerData, defaultAnimation, "Default Clip"),
                                   FIELD_FLOAT(AnimationControllerData, playbackSpeed, "Speed"),
                                   FIELD_BOOL(AnimationControllerData, playing, "Playing"),
                                   FIELD_BOOL(AnimationControllerData, loop, "Loop"));
    }

    // ============================================================================
    // Script (migrated to reflection)
    // ============================================================================

    void InspectorPanel::RenderScriptComponent()
    {
        RENDER_REFLECTED_COMPONENT(ComponentType::SCRIPT, ICON_FA_CODE, "Script", ScriptData,
                                   FIELD_STRING(ScriptData, scriptPath, "Script Path"),
                                   FIELD_STRING(ScriptData, className, "Class Name"),
                                   FIELD_BOOL(ScriptData, autoStart, "Auto Start"));
    }

    // ============================================================================
    // Health (migrated to reflection)
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
            auto* d = comp ? comp->GetData<HealthData>() : nullptr;
            if (d)
            {
                static const std::vector<Spark::FieldInfo> fields = {
                    FIELD_FLOAT(HealthData, health, "Health"),
                    FIELD_FLOAT(HealthData, maxHealth, "Max Health"),
                };
                RenderReflectedFields(d, fields);

                // Health bar preview (custom visual not expressible via reflection)
                float fraction = d->maxHealth > 0.0f ? d->health / d->maxHealth : 0.0f;
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
    // Spline Follower (migrated to reflection with enum)
    // ============================================================================

    void InspectorPanel::RenderSplineFollowerComponent()
    {
        RENDER_REFLECTED_COMPONENT(ComponentType::SPLINE_FOLLOWER, ICON_FA_ROUTE, "Spline Follower", SplineFollowerData,
                                   FIELD_FLOAT(SplineFollowerData, speed, "Speed"),
                                   FIELD_ENUM(SplineFollowerData, loopMode, "Loop Mode", "Once", "Loop", "Ping-Pong"),
                                   FIELD_BOOL(SplineFollowerData, playing, "Playing"),
                                   FIELD_BOOL(SplineFollowerData, orientToPath, "Orient to Path"));
    }

    // ============================================================================
    // Network Identity (migrated to reflection)
    // ============================================================================

    void InspectorPanel::RenderNetworkIdentityComponent()
    {
        RENDER_REFLECTED_COMPONENT(ComponentType::NETWORK_IDENTITY, ICON_FA_NETWORK_WIRED, "Network Identity",
                                   NetworkIdentityData,
                                   FIELD_BOOL(NetworkIdentityData, replicateTransform, "Replicate Transform"),
                                   FIELD_BOOL(NetworkIdentityData, replicateHealth, "Replicate Health"),
                                   FIELD_BOOL(NetworkIdentityData, isLocalAuthority, "Local Authority"));
    }

    // ============================================================================
    // Weather (migrated to reflection with enum)
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
            auto* d = comp ? comp->GetData<WeatherData>() : nullptr;
            if (d)
            {
                static const std::vector<Spark::FieldInfo> fields = {
                    FIELD_ENUM(WeatherData, weatherType, "Type", "Clear", "Cloudy", "Rain", "Snow", "Fog", "Storm"),
                    FIELD_FLOAT_RANGE(WeatherData, intensity, "Intensity", 0.0f, 1.0f),
                    FIELD_FLOAT(WeatherData, windSpeed, "Wind Speed"),
                    FIELD_VEC3(WeatherData, windDirection, "Wind Direction"),
                    FIELD_FLOAT(WeatherData, transitionTime, "Transition Time"),
                    FIELD_BOOL(WeatherData, enabled, "Enabled"),
                };
                RenderReflectedFields(d, fields);
            }
            else
            {
                ImGui::TextDisabled("(Component data unavailable)");
            }
            ImGui::Unindent(4);
        }
    }

    // ============================================================================
    // AI Agent (migrated to reflection with enum)
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
            auto* d = comp ? comp->GetData<AIAgentData>() : nullptr;
            if (d)
            {
                Spark::FieldInfo stateField = FIELD_ENUM(AIAgentData, aiState, "Initial State", "Idle", "Patrolling",
                                                         "Alert", "Combat", "Fleeing", "Dead");
                Spark::FieldInfo btField = FIELD_STRING(AIAgentData, behaviorTreeName, "Behavior Tree");
                Spark::FieldInfo detField = FIELD_FLOAT(AIAgentData, detectionRange, "Detection Range");
                detField.category = "Perception";
                Spark::FieldInfo atkField = FIELD_FLOAT(AIAgentData, attackRange, "Attack Range");
                atkField.category = "Perception";
                Spark::FieldInfo rxnField = FIELD_FLOAT(AIAgentData, reactionTime, "Reaction Time");
                rxnField.category = "Perception";
                Spark::FieldInfo spdField = FIELD_FLOAT(AIAgentData, moveSpeed, "Move Speed");
                spdField.category = "Movement";
                Spark::FieldInfo accField = FIELD_FLOAT_RANGE(AIAgentData, accuracy, "Accuracy", 0.0f, 1.0f);
                accField.category = "Movement";

                const std::vector<Spark::FieldInfo> fields = {stateField, btField,  detField, atkField,
                                                              rxnField,   spdField, accField};
                RenderReflectedFields(d, fields);
            }
            else
            {
                ImGui::TextDisabled("(Component data unavailable)");
            }
            ImGui::Unindent(4);
        }
    }

    // ============================================================================
    // Batch 2 — Migrated from InspectorComponentRenderers_Gameplay.cpp
    // ============================================================================

    // ============================================================================
    // Decal (migrated to reflection)
    // ============================================================================

    void InspectorPanel::RenderDecalComponent()
    {
        RENDER_REFLECTED_COMPONENT(
            ComponentType::DECAL, ICON_FA_STAMP, "Decal", DecalData, FIELD_STRING(DecalData, texturePath, "Texture"),
            FIELD_STRING(DecalData, category, "Category"), FIELD_VEC3(DecalData, size, "Size"),
            FIELD_VEC4(DecalData, color, "Color"), FIELD_FLOAT(DecalData, lifetime, "Lifetime"),
            FIELD_FLOAT(DecalData, fadeOutDuration, "Fade Duration"),
            FIELD_BOOL(DecalData, receiveLighting, "Receive Lighting"), FIELD_INT(DecalData, sortOrder, "Sort Order"));
    }

    // ============================================================================
    // Particle Emitter (migrated to reflection with categories)
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
            auto* d = comp ? comp->GetData<ParticleEmitterData>() : nullptr;
            if (d)
            {
                Spark::FieldInfo nameField = FIELD_STRING(ParticleEmitterData, effectName, "Effect Name");
                Spark::FieldInfo autoField = FIELD_BOOL(ParticleEmitterData, autoPlay, "Auto Play");
                Spark::FieldInfo loopField = FIELD_BOOL(ParticleEmitterData, loop, "Loop");
                Spark::FieldInfo rateField = FIELD_FLOAT(ParticleEmitterData, emissionRate, "Rate");
                rateField.category = "Emission";
                Spark::FieldInfo maxField = FIELD_INT(ParticleEmitterData, maxParticles, "Max Particles");
                maxField.category = "Emission";
                Spark::FieldInfo lifeField = FIELD_FLOAT(ParticleEmitterData, lifetime, "Lifetime");
                lifeField.category = "Emission";
                Spark::FieldInfo colField = FIELD_VEC4(ParticleEmitterData, startColor, "Start Color");
                colField.category = "Initial Values";
                Spark::FieldInfo sizeField = FIELD_FLOAT(ParticleEmitterData, startSize, "Start Size");
                sizeField.category = "Initial Values";
                Spark::FieldInfo speedField = FIELD_FLOAT(ParticleEmitterData, startSpeed, "Start Speed");
                speedField.category = "Initial Values";
                Spark::FieldInfo gravField = FIELD_FLOAT(ParticleEmitterData, gravityMultiplier, "Gravity");
                gravField.category = "Initial Values";

                const std::vector<Spark::FieldInfo> fields = {nameField, autoField, loopField, rateField,  maxField,
                                                              lifeField, colField,  sizeField, speedField, gravField};
                RenderReflectedFields(d, fields);
            }
            else
            {
                ImGui::TextDisabled("(Component data unavailable)");
            }
            ImGui::Unindent(4);
        }
    }

} // namespace SparkEditor
