/**
 * @file InspectorComponentRenderers_Reflected.cpp
 * @brief Inspector renderers using reflection-driven field rendering
 *
 * Contains: RenderReflectedFields helper (with category, tooltip, and enum
 * dropdown support), RENDER_REFLECTED_COMPONENT macro, and all components
 * using reflection-driven rendering:
 *   Volumes: TriggerVolume, PostProcessVolume, FogVolume
 *   Probes:  ReflectionProbe, LightProbe
 *   Placement: NavObstacle, WaterPlane, LODGroup, SpawnPoint, WindZone, Billboard
 *   Audio:   AudioReverbZone, AudioListener
 *   Gameplay: Health, AIAgent, Script, AnimationController, SplineFollower,
 *             Weather, NetworkIdentity
 *   2D:      PixelPerfect, SpriteAnimator, Terrain
 *   Misc:    CharacterController, Skybox
 */

#include "InspectorPanel.h"
#include "../Core/EditorIcons.h"
#include "../Core/EditorFonts.h"
#include "../CommandHistory.h"
#include "Utils/LogMacros.h"
#include "Utils/MathUtils.h"
#include <imgui.h>
#include <algorithm>
#include <cstring>

namespace SparkEditor
{

    // ============================================================================
    // Reflection-driven field rendering
    // ============================================================================

    // Render a single field widget based on its FieldInfo metadata.
    static void RenderSingleField(const Spark::FieldInfo& field, char* dst)
    {
        // Enum fields with names → combo dropdown
        if (field.type == Spark::FieldType::Enum || (field.type == Spark::FieldType::Int && !field.enumNames.empty()))
        {
            int* val = reinterpret_cast<int*>(dst);
            if (!field.enumNames.empty())
            {
                // Build items string for ImGui::Combo (null-separated, double-null terminated)
                std::string items;
                for (const auto& name : field.enumNames)
                {
                    items += name;
                    items += '\0';
                }
                items += '\0';
                ImGui::Combo(field.name.c_str(), val, items.c_str());
            }
            else
            {
                ImGui::DragInt(field.name.c_str(), val);
            }
            return;
        }

        switch (field.type)
        {
        case Spark::FieldType::Bool:
            ImGui::Checkbox(field.name.c_str(), reinterpret_cast<bool*>(dst));
            break;

        case Spark::FieldType::Int:
            if (field.hasRange)
            {
                ImGui::SliderInt(field.name.c_str(), reinterpret_cast<int*>(dst), static_cast<int>(field.rangeMin),
                                 static_cast<int>(field.rangeMax));
            }
            else
            {
                ImGui::DragInt(field.name.c_str(), reinterpret_cast<int*>(dst));
            }
            break;

        case Spark::FieldType::Float:
            if (field.hasRange)
            {
                ImGui::SliderFloat(field.name.c_str(), reinterpret_cast<float*>(dst), field.rangeMin, field.rangeMax,
                                   "%.3f");
            }
            else
            {
                ImGui::DragFloat(field.name.c_str(), reinterpret_cast<float*>(dst), 0.1f);
            }
            break;

        case Spark::FieldType::Double:
        {
            auto val = static_cast<float>(*reinterpret_cast<double*>(dst));
            if (ImGui::DragFloat(field.name.c_str(), &val, 0.1f))
            {
                *reinterpret_cast<double*>(dst) = static_cast<double>(val);
            }
            break;
        }

        case Spark::FieldType::String:
            // Editor data types use char[N] arrays; handle as fixed buffer
            if (field.size > sizeof(std::string))
            {
                ImGui::InputText(field.name.c_str(), reinterpret_cast<char*>(dst), field.size);
            }
            else
            {
                auto* str = reinterpret_cast<std::string*>(dst);
                char buf[256];
                strncpy(buf, str->c_str(), sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = '\0';
                if (ImGui::InputText(field.name.c_str(), buf, sizeof(buf)))
                {
                    *str = buf;
                }
            }
            break;

        case Spark::FieldType::Vector2:
        {
            float* v = reinterpret_cast<float*>(dst);
            ImGui::DragFloat2(field.name.c_str(), v, 0.1f);
            break;
        }

        case Spark::FieldType::Vector3:
        {
            float* v = reinterpret_cast<float*>(dst);
            InspectorPanel::DrawVec3Control(field.name.c_str(), v, 0.0f, 0.1f);
            break;
        }

        case Spark::FieldType::Vector4:
        {
            float* v = reinterpret_cast<float*>(dst);
            ImGui::ColorEdit4(field.name.c_str(), v);
            break;
        }

        default:
            ImGui::TextDisabled("%s (unsupported type)", field.name.c_str());
            break;
        }
    }

    // Check if a field should be visible based on its visibleWhenField condition.
    static bool IsFieldVisible(const Spark::FieldInfo& field, const void* data,
                               const std::vector<Spark::FieldInfo>& allFields)
    {
        if (field.visibleWhenField.empty())
            return true; // No condition — always visible

        // Find the controlling field and read its int value
        for (const auto& ctrl : allFields)
        {
            if (ctrl.fieldName == field.visibleWhenField)
            {
                const auto* src = static_cast<const char*>(data) + ctrl.offset;
                int val = 0;
                if (ctrl.type == Spark::FieldType::Bool)
                {
                    bool b = false;
                    std::memcpy(&b, src, sizeof(bool));
                    val = b ? 1 : 0;
                }
                else
                {
                    std::memcpy(&val, src, sizeof(int));
                }
                return val == field.visibleWhenValue;
            }
        }
        return true; // Controlling field not found — show by default
    }

    void InspectorPanel::RenderReflectedFields(void* data, const std::vector<Spark::FieldInfo>& fields)
    {
        if (!data)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "Inspector: RenderReflectedFields called with null data");
            ImGui::TextDisabled("(Component data unavailable)");
            return;
        }

        // Check if any field has a category assigned
        bool hasCategories = false;
        for (const auto& field : fields)
        {
            if (!field.category.empty())
            {
                hasCategories = true;
                break;
            }
        }

        if (hasCategories)
        {
            // Collect unique categories in order of first appearance
            std::vector<std::string> categories;
            for (const auto& field : fields)
            {
                const auto& cat = field.category;
                bool found = false;
                for (const auto& c : categories)
                {
                    if (c == cat)
                    {
                        found = true;
                        break;
                    }
                }
                if (!found)
                    categories.push_back(cat);
            }

            for (const auto& cat : categories)
            {
                bool inSection = !cat.empty();
                if (inSection)
                {
                    ImGui::Separator();
                    ImGui::TextDisabled("%s", cat.c_str());
                }

                for (const auto& field : fields)
                {
                    if (field.category != cat)
                        continue;
                    if (!IsFieldVisible(field, data, fields))
                        continue;

                    auto* dst = static_cast<char*>(data) + field.offset;
                    RenderSingleField(field, dst);

                    if (!field.tooltip.empty() && ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", field.tooltip.c_str());

                    if (field.readOnly)
                    {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(read-only)");
                    }
                }
            }
        }
        else
        {
            // Flat rendering (no categories)
            for (const auto& field : fields)
            {
                if (!IsFieldVisible(field, data, fields))
                    continue;
                auto* dst = static_cast<char*>(data) + field.offset;
                RenderSingleField(field, dst);

                if (!field.tooltip.empty() && ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", field.tooltip.c_str());

                if (field.readOnly)
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(read-only)");
                }
            }
        }
    }

    // ============================================================================
    // Helper: render a component header + reflected fields from editor data
    // ============================================================================

#define RENDER_REFLECTED_COMPONENT(compType, icon, displayName, dataType, ...)                                         \
    {                                                                                                                  \
        bool headerOpen = ImGui::CollapsingHeader(icon " " displayName);                                               \
        if (ImGui::BeginPopupContextItem("##" displayName "Ctx"))                                                      \
        {                                                                                                              \
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))                                                    \
                RemoveComponent(compType);                                                                             \
            ImGui::EndPopup();                                                                                         \
        }                                                                                                              \
        if (headerOpen)                                                                                                \
        {                                                                                                              \
            ImGui::Indent(4);                                                                                          \
            Component* comp = FindComponent(m_scene, m_inspectedObjectID, compType);                                   \
            auto* d = comp ? comp->GetData<dataType>() : nullptr;                                                      \
            if (d)                                                                                                     \
            {                                                                                                          \
                static const std::vector<Spark::FieldInfo> fields = {__VA_ARGS__};                                     \
                dataType oldSnapshot = *d;                                                                             \
                RenderReflectedFields(d, fields);                                                                      \
                if (std::memcmp(&oldSnapshot, d, sizeof(dataType)) != 0)                                               \
                {                                                                                                      \
                    dataType newSnapshot = *d;                                                                         \
                    SceneFile* cs = m_scene;                                                                           \
                    ObjectID ci = m_inspectedObjectID;                                                                 \
                    ComponentType ct = compType;                                                                       \
                    auto& history = Spark::Editor::CommandHistory::GetInstance();                                      \
                    history.Execute(std::make_unique<Spark::Editor::LambdaCommand>(                                    \
                        [cs, ci, ct, newSnapshot]()                                                                    \
                        {                                                                                              \
                            Component* c = FindComponent(cs, ci, ct);                                                  \
                            if (auto* p = c ? c->GetData<dataType>() : nullptr)                                        \
                                *p = newSnapshot;                                                                      \
                        },                                                                                             \
                        [cs, ci, ct, oldSnapshot]()                                                                    \
                        {                                                                                              \
                            Component* c = FindComponent(cs, ci, ct);                                                  \
                            if (auto* p = c ? c->GetData<dataType>() : nullptr)                                        \
                                *p = oldSnapshot;                                                                      \
                        },                                                                                             \
                        displayName " Change"));                                                                       \
                }                                                                                                      \
            }                                                                                                          \
            else                                                                                                       \
            {                                                                                                          \
                ImGui::TextDisabled("(Component data unavailable)");                                                   \
            }                                                                                                          \
            ImGui::Unindent(4);                                                                                        \
        }                                                                                                              \
    }

    // Helper to build FieldInfo inline for editor data types
    static Spark::FieldInfo MakeField(const char* name, const char* fieldName, Spark::FieldType type, size_t offset,
                                      size_t size, float rangeMin = 0, float rangeMax = 0, bool hasRange = false)
    {
        Spark::FieldInfo f;
        f.name = name;
        f.fieldName = fieldName;
        f.type = type;
        f.offset = offset;
        f.size = size;
        f.rangeMin = rangeMin;
        f.rangeMax = rangeMax;
        f.hasRange = hasRange;
        return f;
    }

    // Helper to build an enum FieldInfo with named values for combo dropdown
    static Spark::FieldInfo MakeEnumField(const char* name, const char* fieldName, size_t offset, size_t size,
                                          std::vector<std::string> names)
    {
        Spark::FieldInfo f;
        f.name = name;
        f.fieldName = fieldName;
        f.type = Spark::FieldType::Int;
        f.offset = offset;
        f.size = size;
        f.enumNames = std::move(names);
        return f;
    }

    // Shorthand macros for concise field descriptors
#define FIELD_FLOAT(DataType, member, displayName)                                                                     \
    MakeField(displayName, #member, Spark::FieldType::Float, offsetof(DataType, member), sizeof(float))
#define FIELD_FLOAT_RANGE(DataType, member, displayName, lo, hi)                                                       \
    MakeField(displayName, #member, Spark::FieldType::Float, offsetof(DataType, member), sizeof(float), lo, hi, true)
#define FIELD_INT(DataType, member, displayName)                                                                       \
    MakeField(displayName, #member, Spark::FieldType::Int, offsetof(DataType, member), sizeof(int))
#define FIELD_BOOL(DataType, member, displayName)                                                                      \
    MakeField(displayName, #member, Spark::FieldType::Bool, offsetof(DataType, member), sizeof(bool))
#define FIELD_VEC2(DataType, member, displayName)                                                                      \
    MakeField(displayName, #member, Spark::FieldType::Vector2, offsetof(DataType, member), sizeof(XMFLOAT2))
#define FIELD_VEC3(DataType, member, displayName)                                                                      \
    MakeField(displayName, #member, Spark::FieldType::Vector3, offsetof(DataType, member), sizeof(XMFLOAT3))
#define FIELD_VEC4(DataType, member, displayName)                                                                      \
    MakeField(displayName, #member, Spark::FieldType::Vector4, offsetof(DataType, member), sizeof(XMFLOAT4))
#define FIELD_STRING(DataType, member, displayName)                                                                    \
    MakeField(displayName, #member, Spark::FieldType::String, offsetof(DataType, member), sizeof(DataType::member))
#define FIELD_ENUM(DataType, member, displayName, ...)                                                                 \
    MakeEnumField(displayName, #member, offsetof(DataType, member), sizeof(DataType::member),                          \
                  std::vector<std::string>{__VA_ARGS__})

    // ============================================================================
    // Trigger Volume
    // ============================================================================

    void InspectorPanel::RenderTriggerVolumeComponent()
    {
        RENDER_REFLECTED_COMPONENT(ComponentType::TRIGGER_VOLUME, ICON_FA_VECTOR_SQUARE, "Trigger Volume",
                                   TriggerVolumeData, FIELD_INT(TriggerVolumeData, shape, "Shape (0=Sphere,1=AABB)"),
                                   FIELD_FLOAT(TriggerVolumeData, radius, "Radius"),
                                   FIELD_VEC3(TriggerVolumeData, halfExtents, "Half Extents"),
                                   FIELD_STRING(TriggerVolumeData, onEnterEvent, "On Enter Event"),
                                   FIELD_STRING(TriggerVolumeData, onExitEvent, "On Exit Event"),
                                   FIELD_BOOL(TriggerVolumeData, enabled, "Enabled"),
                                   FIELD_BOOL(TriggerVolumeData, oneShot, "One Shot"));
    }

    // ============================================================================
    // Post-Process Volume
    // ============================================================================

    void InspectorPanel::RenderPostProcessVolumeComponent()
    {
        RENDER_REFLECTED_COMPONENT(ComponentType::POST_PROCESS_VOLUME, ICON_FA_ADJUST, "Post-Process Volume",
                                   PostProcessVolumeData, FIELD_BOOL(PostProcessVolumeData, isGlobal, "Is Global"),
                                   FIELD_INT(PostProcessVolumeData, priority, "Priority"),
                                   FIELD_FLOAT_RANGE(PostProcessVolumeData, weight, "Weight", 0.0f, 1.0f),
                                   FIELD_FLOAT(PostProcessVolumeData, blendDistance, "Blend Distance"),
                                   FIELD_BOOL(PostProcessVolumeData, overrideExposure, "Override Exposure"),
                                   FIELD_FLOAT(PostProcessVolumeData, exposure, "Exposure"),
                                   FIELD_BOOL(PostProcessVolumeData, overrideBloom, "Override Bloom"),
                                   FIELD_FLOAT(PostProcessVolumeData, bloomIntensity, "Bloom Intensity"),
                                   FIELD_FLOAT(PostProcessVolumeData, bloomThreshold, "Bloom Threshold"),
                                   FIELD_BOOL(PostProcessVolumeData, overrideColorGrading, "Override Color Grading"),
                                   FIELD_FLOAT_RANGE(PostProcessVolumeData, saturation, "Saturation", 0.0f, 2.0f),
                                   FIELD_FLOAT_RANGE(PostProcessVolumeData, contrast, "Contrast", 0.0f, 2.0f),
                                   FIELD_FLOAT(PostProcessVolumeData, temperature, "Temperature"),
                                   FIELD_BOOL(PostProcessVolumeData, overrideFog, "Override Fog"),
                                   FIELD_FLOAT(PostProcessVolumeData, fogDensity, "Fog Density"));
    }

    // ============================================================================
    // Reflection Probe
    // ============================================================================

    void InspectorPanel::RenderReflectionProbeComponent()
    {
        RENDER_REFLECTED_COMPONENT(ComponentType::REFLECTION_PROBE, ICON_FA_GLOBE, "Reflection Probe",
                                   ReflectionProbeData, FIELD_INT(ReflectionProbeData, resolution, "Resolution"),
                                   FIELD_FLOAT(ReflectionProbeData, influenceRadius, "Influence Radius"),
                                   FIELD_VEC3(ReflectionProbeData, boxExtents, "Box Extents"),
                                   FIELD_BOOL(ReflectionProbeData, useBoxProjection, "Use Box Projection"),
                                   FIELD_BOOL(ReflectionProbeData, isDynamic, "Is Dynamic"),
                                   FIELD_FLOAT(ReflectionProbeData, refreshInterval, "Refresh Interval"),
                                   FIELD_INT(ReflectionProbeData, importance, "Importance"),
                                   FIELD_VEC3(ReflectionProbeData, captureOffset, "Capture Offset"));
    }

    // ============================================================================
    // Light Probe
    // ============================================================================

    void InspectorPanel::RenderLightProbeComponent()
    {
        RENDER_REFLECTED_COMPONENT(ComponentType::LIGHT_PROBE, ICON_FA_SUN, "Light Probe", LightProbeData,
                                   FIELD_FLOAT(LightProbeData, influenceRadius, "Influence Radius"),
                                   FIELD_VEC3(LightProbeData, gridSpacing, "Grid Spacing"),
                                   FIELD_BOOL(LightProbeData, baked, "Baked"),
                                   FIELD_INT(LightProbeData, shOrder, "SH Order"));
    }

    // ============================================================================
    // Nav Obstacle
    // ============================================================================

    void InspectorPanel::RenderNavObstacleComponent()
    {
        RENDER_REFLECTED_COMPONENT(
            ComponentType::NAV_OBSTACLE, ICON_FA_ROUTE, "Nav Obstacle", NavObstacleData,
            FIELD_INT(NavObstacleData, shape, "Shape (0=Box,1=Cylinder)"),
            FIELD_VEC3(NavObstacleData, halfExtents, "Half Extents"), FIELD_FLOAT(NavObstacleData, radius, "Radius"),
            FIELD_FLOAT(NavObstacleData, height, "Height"), FIELD_BOOL(NavObstacleData, carveOnMove, "Carve On Move"));
    }

    // ============================================================================
    // Water Plane
    // ============================================================================

    void InspectorPanel::RenderWaterPlaneComponent()
    {
        RENDER_REFLECTED_COMPONENT(
            ComponentType::WATER_PLANE, ICON_FA_WATER, "Water Plane", WaterPlaneData,
            FIELD_FLOAT(WaterPlaneData, waveHeight, "Wave Height"),
            FIELD_FLOAT(WaterPlaneData, waveSpeed, "Wave Speed"),
            FIELD_FLOAT(WaterPlaneData, waveFrequency, "Wave Frequency"),
            FIELD_FLOAT_RANGE(WaterPlaneData, reflectionStrength, "Reflection Strength", 0.0f, 1.0f),
            FIELD_FLOAT_RANGE(WaterPlaneData, refractionStrength, "Refraction Strength", 0.0f, 1.0f),
            FIELD_BOOL(WaterPlaneData, receiveShadows, "Receive Shadows"));
    }

    // ============================================================================
    // Fog Volume
    // ============================================================================

    void InspectorPanel::RenderFogVolumeComponent()
    {
        RENDER_REFLECTED_COMPONENT(
            ComponentType::FOG_VOLUME, ICON_FA_SMOG, "Fog Volume", FogVolumeData,
            FIELD_VEC3(FogVolumeData, halfExtents, "Half Extents"), FIELD_FLOAT(FogVolumeData, density, "Density"),
            FIELD_VEC4(FogVolumeData, color, "Color"), FIELD_FLOAT(FogVolumeData, falloff, "Falloff"),
            FIELD_FLOAT(FogVolumeData, heightFalloff, "Height Falloff"));
    }

    // ============================================================================
    // LOD Group
    // ============================================================================

    void InspectorPanel::RenderLODGroupComponent()
    {
        RENDER_REFLECTED_COMPONENT(ComponentType::LOD_GROUP, ICON_FA_LAYER_GROUP, "LOD Group", LODGroupData,
                                   FIELD_FLOAT(LODGroupData, lodDistance0, "LOD 0 Distance"),
                                   FIELD_FLOAT(LODGroupData, lodDistance1, "LOD 1 Distance"),
                                   FIELD_FLOAT(LODGroupData, lodDistance2, "LOD 2 Distance"),
                                   FIELD_FLOAT(LODGroupData, lodDistance3, "LOD 3 Distance"),
                                   FIELD_INT(LODGroupData, lodCount, "LOD Count"),
                                   FIELD_FLOAT(LODGroupData, crossFadeDuration, "Cross Fade Duration"),
                                   FIELD_BOOL(LODGroupData, autoGenerate, "Auto Generate"));
    }

    // ============================================================================
    // Spawn Point
    // ============================================================================

    void InspectorPanel::RenderSpawnPointComponent()
    {
        RENDER_REFLECTED_COMPONENT(
            ComponentType::SPAWN_POINT, ICON_FA_LOCATION_ARROW, "Spawn Point", SpawnPointData,
            FIELD_STRING(SpawnPointData, spawnTag, "Spawn Tag"), FIELD_INT(SpawnPointData, teamID, "Team ID"),
            FIELD_FLOAT(SpawnPointData, spawnRadius, "Spawn Radius"),
            FIELD_FLOAT(SpawnPointData, respawnDelay, "Respawn Delay"),
            FIELD_INT(SpawnPointData, maxConcurrent, "Max Concurrent"), FIELD_BOOL(SpawnPointData, enabled, "Enabled"),
            FIELD_INT(SpawnPointData, priority, "Priority"));
    }

    // ============================================================================
    // Audio Reverb Zone
    // ============================================================================

    void InspectorPanel::RenderAudioReverbZoneComponent()
    {
        RENDER_REFLECTED_COMPONENT(
            ComponentType::AUDIO_REVERB_ZONE, ICON_FA_VOLUME_UP, "Audio Reverb Zone", AudioReverbZoneData,
            FIELD_FLOAT(AudioReverbZoneData, innerRadius, "Inner Radius"),
            FIELD_FLOAT(AudioReverbZoneData, outerRadius, "Outer Radius"),
            FIELD_INT(AudioReverbZoneData, reverbPreset, "Preset (0-6)"),
            FIELD_FLOAT(AudioReverbZoneData, decayTime, "Decay Time"),
            FIELD_FLOAT_RANGE(AudioReverbZoneData, earlyReflections, "Early Reflections", 0.0f, 1.0f),
            FIELD_FLOAT_RANGE(AudioReverbZoneData, lateReverbLevel, "Late Reverb Level", 0.0f, 1.0f),
            FIELD_FLOAT_RANGE(AudioReverbZoneData, diffusion, "Diffusion", 0.0f, 1.0f),
            FIELD_FLOAT_RANGE(AudioReverbZoneData, roomSize, "Room Size", 0.0f, 1.0f));
    }

    // ============================================================================
    // Wind Zone
    // ============================================================================

    void InspectorPanel::RenderWindZoneComponent()
    {
        RENDER_REFLECTED_COMPONENT(
            ComponentType::WIND_ZONE, ICON_FA_WIND, "Wind Zone", WindZoneData,
            FIELD_INT(WindZoneData, mode, "Mode (0=Dir,1=Sphere)"), FIELD_VEC3(WindZoneData, direction, "Direction"),
            FIELD_FLOAT(WindZoneData, mainStrength, "Main Strength"),
            FIELD_FLOAT(WindZoneData, turbulenceStrength, "Turbulence"),
            FIELD_FLOAT(WindZoneData, pulseFrequency, "Pulse Frequency"), FIELD_FLOAT(WindZoneData, radius, "Radius"),
            FIELD_BOOL(WindZoneData, affectsParticles, "Affects Particles"),
            FIELD_BOOL(WindZoneData, affectsVegetation, "Affects Vegetation"));
    }

    // ============================================================================
    // Billboard
    // ============================================================================

    void InspectorPanel::RenderBillboardComponent()
    {
        RENDER_REFLECTED_COMPONENT(ComponentType::BILLBOARD, ICON_FA_IMAGE, "Billboard", BillboardData,
                                   FIELD_STRING(BillboardData, texturePath, "Texture Path"),
                                   FIELD_VEC4(BillboardData, color, "Color"),
                                   FIELD_INT(BillboardData, lockAxis, "Lock Axis (0=Full,1=Y,2=None)"),
                                   FIELD_FLOAT(BillboardData, fadeStartDistance, "Fade Start Distance"),
                                   FIELD_FLOAT(BillboardData, fadeEndDistance, "Fade End Distance"),
                                   FIELD_INT(BillboardData, sortingLayer, "Sorting Layer"));
    }

    // ============================================================================
    // Audio Listener
    // ============================================================================

    void InspectorPanel::RenderAudioListenerComponent()
    {
        RENDER_REFLECTED_COMPONENT(ComponentType::AUDIO_LISTENER, ICON_FA_HEADPHONES, "Audio Listener",
                                   AudioListenerData, FIELD_BOOL(AudioListenerData, isActive, "Is Active"),
                                   FIELD_FLOAT(AudioListenerData, volumeScale, "Volume Scale"));
    }

    // ============================================================================
    // Character Controller
    // ============================================================================

    void InspectorPanel::RenderCharacterControllerComponent()
    {
        RENDER_REFLECTED_COMPONENT(ComponentType::CHARACTER_CONTROLLER, ICON_FA_RUNNING, "Character Controller",
                                   CharacterControllerData, FIELD_FLOAT(CharacterControllerData, height, "Height"),
                                   FIELD_FLOAT(CharacterControllerData, radius, "Radius"),
                                   FIELD_FLOAT(CharacterControllerData, slopeLimit, "Slope Limit"),
                                   FIELD_FLOAT(CharacterControllerData, stepHeight, "Step Height"),
                                   FIELD_FLOAT(CharacterControllerData, skinWidth, "Skin Width"),
                                   FIELD_FLOAT(CharacterControllerData, moveSpeed, "Move Speed"),
                                   FIELD_FLOAT(CharacterControllerData, jumpForce, "Jump Force"),
                                   FIELD_FLOAT(CharacterControllerData, gravity, "Gravity"));
    }

    // ============================================================================
    // Skybox
    // ============================================================================

    void InspectorPanel::RenderSkyboxComponent()
    {
        RENDER_REFLECTED_COMPONENT(
            ComponentType::SKYBOX, ICON_FA_CLOUD_SUN, "Skybox", SkyboxData,
            FIELD_ENUM(SkyboxData, mode, "Mode", "Procedural", "Cubemap", "Gradient", "Solid Color"),
            FIELD_STRING(SkyboxData, cubemapPath, "Cubemap Path"), FIELD_VEC4(SkyboxData, topColor, "Top Color"),
            FIELD_VEC4(SkyboxData, bottomColor, "Bottom Color"), FIELD_FLOAT(SkyboxData, exposure, "Exposure"),
            FIELD_FLOAT(SkyboxData, rotation, "Rotation"));
    }

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
    // Migrated from InspectorComponentRenderers_2D.cpp
    // ============================================================================

    // ============================================================================
    // Pixel Perfect (migrated to reflection)
    // ============================================================================

    void InspectorPanel::RenderPixelPerfectComponent()
    {
        RENDER_REFLECTED_COMPONENT(ComponentType::PIXEL_PERFECT, ICON_FA_TH_LARGE, "Pixel Perfect", PixelPerfectData,
                                   FIELD_INT(PixelPerfectData, referenceWidth, "Reference Width"),
                                   FIELD_INT(PixelPerfectData, referenceHeight, "Reference Height"),
                                   FIELD_BOOL(PixelPerfectData, upscaleToFill, "Upscale to Fill"),
                                   FIELD_BOOL(PixelPerfectData, cropToFit, "Crop to Fit"));
    }

    // ============================================================================
    // Sprite Animator (migrated to reflection — minimal)
    // ============================================================================

    void InspectorPanel::RenderSpriteAnimatorComponent()
    {
        bool headerOpen = ImGui::CollapsingHeader(ICON_FA_FILM " Sprite Animator");
        if (ImGui::BeginPopupContextItem("##SpriteAnimatorCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
                RemoveComponent(ComponentType::SPRITE_ANIMATOR);
            ImGui::EndPopup();
        }
        if (headerOpen)
        {
            ImGui::Indent(4);
            ImGui::TextDisabled("Edit clips in the Sprite Animation Editor panel");
            ImGui::Unindent(4);
        }
    }

    // ============================================================================
    // Terrain (migrated to reflection with categories)
    // ============================================================================

    void InspectorPanel::RenderTerrainComponent()
    {
        bool headerOpen = ImGui::CollapsingHeader(ICON_FA_MOUNTAIN " Terrain");
        if (ImGui::BeginPopupContextItem("##TerrainCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
                RemoveComponent(ComponentType::TERRAIN);
            ImGui::EndPopup();
        }
        if (headerOpen)
        {
            ImGui::Indent(4);
            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::TERRAIN);
            auto* d = comp ? comp->GetData<TerrainSceneData>() : nullptr;
            if (d)
            {
                Spark::FieldInfo resField = FIELD_INT(TerrainSceneData, heightmapResolution, "Resolution");
                Spark::FieldInfo sizeField = FIELD_FLOAT(TerrainSceneData, terrainSize, "Size");
                Spark::FieldInfo hsField = FIELD_FLOAT(TerrainSceneData, heightScale, "Height Scale");
                Spark::FieldInfo minHField = FIELD_FLOAT(TerrainSceneData, minHeight, "Min Height");
                Spark::FieldInfo maxHField = FIELD_FLOAT(TerrainSceneData, maxHeight, "Max Height");
                Spark::FieldInfo lodField = FIELD_INT(TerrainSceneData, lodLevels, "LOD Levels");
                lodField.category = "LOD";
                Spark::FieldInfo biasField = FIELD_FLOAT(TerrainSceneData, lodBias, "LOD Bias");
                biasField.category = "LOD";
                Spark::FieldInfo colField = FIELD_BOOL(TerrainSceneData, generateCollider, "Generate Collider");
                colField.category = "Physics";
                Spark::FieldInfo csField = FIELD_BOOL(TerrainSceneData, castShadows, "Cast Shadows");
                csField.category = "Physics";
                Spark::FieldInfo rsField = FIELD_BOOL(TerrainSceneData, receiveShadows, "Receive Shadows");
                rsField.category = "Physics";

                const std::vector<Spark::FieldInfo> fields = {resField, sizeField, hsField,  minHField, maxHField,
                                                              lodField, biasField, colField, csField,   rsField};
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

    // ============================================================================
    // Batch 2 — Migrated from InspectorComponentRenderers_2D.cpp
    // ============================================================================

    // ============================================================================
    // Sprite Renderer (migrated to reflection)
    // ============================================================================

    void InspectorPanel::RenderSpriteRendererComponent()
    {
        RENDER_REFLECTED_COMPONENT(
            ComponentType::SPRITE_RENDERER, ICON_FA_IMAGE, "Sprite Renderer", SpriteRendererData,
            FIELD_STRING(SpriteRendererData, texturePath, "Texture"), FIELD_VEC4(SpriteRendererData, color, "Color"),
            FIELD_VEC2(SpriteRendererData, pivot, "Pivot"),
            FIELD_FLOAT(SpriteRendererData, pixelsPerUnit, "Pixels/Unit"),
            FIELD_INT(SpriteRendererData, sortingLayer, "Sorting Layer"),
            FIELD_INT(SpriteRendererData, orderInLayer, "Order in Layer"),
            FIELD_BOOL(SpriteRendererData, flipX, "Flip X"), FIELD_BOOL(SpriteRendererData, flipY, "Flip Y"));
    }

    // ============================================================================
    // Camera 2D (migrated to reflection)
    // ============================================================================

    void InspectorPanel::RenderCamera2DComponent()
    {
        RENDER_REFLECTED_COMPONENT(
            ComponentType::CAMERA_2D, ICON_FA_CAMERA, "Camera 2D", Camera2DData,
            FIELD_FLOAT(Camera2DData, orthoSize, "Ortho Size"), FIELD_FLOAT(Camera2DData, zoom, "Zoom"),
            FIELD_FLOAT(Camera2DData, nearPlane, "Near Plane"), FIELD_FLOAT(Camera2DData, farPlane, "Far Plane"),
            FIELD_FLOAT(Camera2DData, followSmoothing, "Follow Smoothing"),
            FIELD_VEC2(Camera2DData, deadZone, "Dead Zone"), FIELD_VEC4(Camera2DData, clearColor, "Clear Color"),
            FIELD_BOOL(Camera2DData, isMain2DCamera, "Main 2D Camera"));
    }

    // ============================================================================
    // Tilemap (migrated to reflection)
    // ============================================================================

    void InspectorPanel::RenderTilemapComponent()
    {
        bool headerOpen = ImGui::CollapsingHeader(ICON_FA_TH " Tilemap");
        if (ImGui::BeginPopupContextItem("##TilemapCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
                RemoveComponent(ComponentType::TILEMAP);
            ImGui::EndPopup();
        }
        if (headerOpen)
        {
            ImGui::Indent(4);
            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::TILEMAP);
            auto* d = comp ? comp->GetData<TilemapData>() : nullptr;
            if (d)
            {
                static const std::vector<Spark::FieldInfo> fields = {
                    FIELD_STRING(TilemapData, tilesetTexturePath, "Tileset"),
                    FIELD_INT(TilemapData, tileWidth, "Tile Width"),
                    FIELD_INT(TilemapData, tileHeight, "Tile Height"),
                    FIELD_INT(TilemapData, mapWidth, "Map Width"),
                    FIELD_INT(TilemapData, mapHeight, "Map Height"),
                    FIELD_FLOAT(TilemapData, pixelsPerUnit, "Pixels/Unit"),
                    FIELD_INT(TilemapData, sortingLayer, "Sorting Layer"),
                    FIELD_BOOL(TilemapData, generateCollision, "Generate Collision"),
                };
                RenderReflectedFields(d, fields);
                ImGui::TextDisabled("Paint tiles in the Tilemap Editor panel");
            }
            else
            {
                ImGui::TextDisabled("(Component data unavailable)");
            }
            ImGui::Unindent(4);
        }
    }

    // ============================================================================
    // Nine-Slice (migrated to reflection with categories)
    // ============================================================================

    void InspectorPanel::RenderNineSliceComponent()
    {
        bool headerOpen = ImGui::CollapsingHeader(ICON_FA_BORDER_ALL " Nine-Slice");
        if (ImGui::BeginPopupContextItem("##NineSliceCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
                RemoveComponent(ComponentType::NINE_SLICE);
            ImGui::EndPopup();
        }
        if (headerOpen)
        {
            ImGui::Indent(4);
            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::NINE_SLICE);
            auto* d = comp ? comp->GetData<NineSliceData>() : nullptr;
            if (d)
            {
                Spark::FieldInfo texField = FIELD_STRING(NineSliceData, texturePath, "Texture");
                Spark::FieldInfo leftField = FIELD_FLOAT(NineSliceData, borderLeft, "Left");
                leftField.category = "Borders (pixels)";
                Spark::FieldInfo topField = FIELD_FLOAT(NineSliceData, borderTop, "Top");
                topField.category = "Borders (pixels)";
                Spark::FieldInfo rightField = FIELD_FLOAT(NineSliceData, borderRight, "Right");
                rightField.category = "Borders (pixels)";
                Spark::FieldInfo bottomField = FIELD_FLOAT(NineSliceData, borderBottom, "Bottom");
                bottomField.category = "Borders (pixels)";
                Spark::FieldInfo sizeField = FIELD_VEC2(NineSliceData, size, "Size");
                Spark::FieldInfo colorField = FIELD_VEC4(NineSliceData, color, "Color");
                Spark::FieldInfo fillField = FIELD_BOOL(NineSliceData, fillCenter, "Fill Center");
                Spark::FieldInfo sortField = FIELD_INT(NineSliceData, sortingLayer, "Sorting Layer");

                const std::vector<Spark::FieldInfo> fields = {texField,  leftField,  topField,  rightField, bottomField,
                                                              sizeField, colorField, fillField, sortField};
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
    // Parallax Background (migrated to reflection)
    // ============================================================================

    void InspectorPanel::RenderParallaxBGComponent()
    {
        RENDER_REFLECTED_COMPONENT(
            ComponentType::PARALLAX_BG, ICON_FA_LAYER_GROUP, "Parallax Background", ParallaxLayerData,
            FIELD_STRING(ParallaxLayerData, texturePath, "Texture"),
            FIELD_VEC2(ParallaxLayerData, scrollSpeed, "Scroll Speed"), FIELD_BOOL(ParallaxLayerData, tileX, "Tile X"),
            FIELD_BOOL(ParallaxLayerData, tileY, "Tile Y"), FIELD_VEC4(ParallaxLayerData, tint, "Tint"),
            FIELD_INT(ParallaxLayerData, sortOrder, "Sort Order"));
    }

} // namespace SparkEditor
