/**
 * @file InspectorPanel.cpp
 * @brief Implementation of the Inspector panel
 * @author Spark Engine Team
 * @date 2025
 *
 * Reads/writes actual entity component data from SceneFile and routes
 * all mutations through CommandHistory for undo/redo support.
 */

#include "InspectorPanel.h"
#include "../Core/EditorIcons.h"
#include "../Core/EditorFonts.h"
#include "../CommandHistory.h"
#include "../../../SparkEngine/Source/Utils/ContainerUtils.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"
#include <imgui.h>
#include <iostream>
#include <algorithm>
#include <cstring>

namespace SparkEditor
{

    InspectorPanel::InspectorPanel() : EditorPanel("Inspector", "inspector_panel") {}

    bool InspectorPanel::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        std::cout << "Initializing Inspector panel\n";
        return true;
    }

    void InspectorPanel::Update(float /*deltaTime*/)
    {
        // Nothing frame-rate dependent to update
    }

    void InspectorPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            if (m_inspectedObjectID == INVALID_OBJECT_ID && m_inspectedObject.empty())
            {
                float avail = ImGui::GetContentRegionAvail().y;
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + avail * 0.4f);
                float textWidth = ImGui::CalcTextSize(ICON_FA_MOUSE_POINTER " Select an object to inspect").x;
                ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textWidth) * 0.5f);
                ImGui::TextColored(ImVec4(0.55f, 0.58f, 0.62f, 1.0f),
                                   ICON_FA_MOUSE_POINTER " Select an object to inspect");
            }
            else
            {
                RenderObjectProperties();
                ImGui::Spacing();
                RenderComponentList();

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Full-width "Add Component" button
                float btnWidth = ImGui::GetContentRegionAvail().x;
                if (ImGui::Button(ICON_FA_PLUS " Add Component", ImVec2(btnWidth, 30)))
                {
                    m_showAddComponentMenu = true;
                }

                if (m_showAddComponentMenu)
                {
                    RenderAddComponentMenu();
                }
            }
        }
        EndPanel();
    }

    void InspectorPanel::Shutdown()
    {
        std::cout << "Shutting down Inspector panel\n";
    }

    bool InspectorPanel::HandleEvent(const std::string& eventType, void* eventData)
    {
        if (eventType == "ObjectSelected" && eventData)
        {
            SetInspectedObject(*static_cast<std::string*>(eventData));
            return true;
        }
        if (eventType == "ObjectSelectedByID" && eventData)
        {
            SetInspectedObjectByID(*static_cast<ObjectID*>(eventData));
            return true;
        }
        return false;
    }

    void InspectorPanel::SetScene(SceneFile* scene)
    {
        m_scene = scene;
    }

    void InspectorPanel::SetInspectedObjectByID(ObjectID objectID)
    {
        m_inspectedObjectID = objectID;
        if (m_scene)
        {
            SceneObject* obj = m_scene->FindObject(objectID);
            if (obj)
            {
                m_inspectedObject = obj->name;
            }
            else
            {
                m_inspectedObject.clear();
            }
        }
    }

    void InspectorPanel::SetInspectedObject(const std::string& objectId)
    {
        m_inspectedObject = objectId;
        // Try to find by name if we have a scene
        if (m_scene && !objectId.empty())
        {
            auto found = m_scene->FindObjectsByName(objectId);
            if (!found.empty())
            {
                m_inspectedObjectID = found[0]->id;
            }
        }
    }

    // ============================================================================
    // Helpers
    // ============================================================================

    bool InspectorPanel::HasComponent(ComponentType type) const
    {
        if (!m_scene || m_inspectedObjectID == INVALID_OBJECT_ID)
            return false;

        SceneObject* obj = m_scene->FindObject(m_inspectedObjectID);
        if (!obj)
            return false;

        return Spark::ContainerUtils::Contains(obj->componentTypes, type);
    }

    void InspectorPanel::AddComponent(ComponentType type)
    {
        if (!m_scene || m_inspectedObjectID == INVALID_OBJECT_ID)
            return;

        SceneObject* obj = m_scene->FindObject(m_inspectedObjectID);
        if (!obj)
            return;

        // Check if already has this component
        if (HasComponent(type))
            return;

        const ObjectID capturedID = m_inspectedObjectID;
        SceneFile* capturedScene = m_scene;
        auto& history = Spark::Editor::CommandHistory::GetInstance();

        history.Execute(std::make_unique<Spark::Editor::LambdaCommand>(
            [capturedScene, capturedID, type]()
            {
                SceneObject* o = capturedScene->FindObject(capturedID);
                if (o)
                {
                    o->componentTypes.push_back(type);

                    // Also create a Component entry with default data
                    Component comp;
                    comp.type = type;
                    comp.objectID = capturedID;
                    comp.enabled = true;

                    switch (type)
                    {
                    case ComponentType::MESH_RENDERER:
                    {
                        MeshRenderer mr;
                        comp.SetData(mr);
                        break;
                    }
                    case ComponentType::LIGHT:
                    {
                        SparkEditor::Light light;
                        comp.SetData(light);
                        break;
                    }
                    case ComponentType::CAMERA:
                    {
                        SparkEditor::Camera cam;
                        comp.SetData(cam);
                        break;
                    }
                    case ComponentType::RIGID_BODY:
                    {
                        RigidBody rb;
                        comp.SetData(rb);
                        break;
                    }
                    case ComponentType::COLLIDER:
                    {
                        SparkEditor::Collider col;
                        comp.SetData(col);
                        break;
                    }
                    case ComponentType::AUDIO_SOURCE:
                    {
                        AudioSource as;
                        comp.SetData(as);
                        break;
                    }
                    case ComponentType::SPRITE_RENDERER:
                        comp.SetData(SpriteRendererData{});
                        break;
                    case ComponentType::CAMERA_2D:
                        comp.SetData(Camera2DData{});
                        break;
                    case ComponentType::RIGID_BODY_2D:
                        comp.SetData(RigidBody2DData{});
                        break;
                    case ComponentType::COLLIDER_2D:
                        comp.SetData(Collider2DData{});
                        break;
                    case ComponentType::TILEMAP:
                        comp.SetData(TilemapData{});
                        break;
                    case ComponentType::NINE_SLICE:
                        comp.SetData(NineSliceData{});
                        break;
                    case ComponentType::PIXEL_PERFECT:
                        comp.SetData(PixelPerfectData{});
                        break;
                    case ComponentType::PARTICLE_SYSTEM:
                        comp.SetData(ParticleEmitterData{});
                        break;
                    case ComponentType::ANIMATION:
                        comp.SetData(AnimationControllerData{});
                        break;
                    case ComponentType::SCRIPT:
                        comp.SetData(ScriptData{});
                        break;
                    case ComponentType::HEALTH:
                        comp.SetData(HealthData{});
                        break;
                    case ComponentType::AI_AGENT:
                        comp.SetData(AIAgentData{});
                        break;
                    case ComponentType::SPLINE:
                        comp.SetData(SplineData{});
                        break;
                    case ComponentType::SPLINE_FOLLOWER:
                        comp.SetData(SplineFollowerData{});
                        break;
                    case ComponentType::DECAL:
                        comp.SetData(DecalData{});
                        break;
                    case ComponentType::PROJECTILE:
                        comp.SetData(ProjectileData{});
                        break;
                    case ComponentType::INTERACTION:
                        comp.SetData(InteractionData{});
                        break;
                    case ComponentType::WEATHER:
                        comp.SetData(WeatherData{});
                        break;
                    case ComponentType::NETWORK_IDENTITY:
                        comp.SetData(NetworkIdentityData{});
                        break;
                    case ComponentType::PARALLAX_BG:
                        comp.SetData(ParallaxLayerData{});
                        break;
                    case ComponentType::TRIGGER_VOLUME:
                        comp.SetData(TriggerVolumeData{});
                        break;
                    case ComponentType::POST_PROCESS_VOLUME:
                        comp.SetData(PostProcessVolumeData{});
                        break;
                    case ComponentType::REFLECTION_PROBE:
                        comp.SetData(ReflectionProbeData{});
                        break;
                    case ComponentType::LIGHT_PROBE:
                        comp.SetData(LightProbeData{});
                        break;
                    case ComponentType::NAV_OBSTACLE:
                        comp.SetData(NavObstacleData{});
                        break;
                    case ComponentType::WATER_PLANE:
                        comp.SetData(WaterPlaneData{});
                        break;
                    case ComponentType::FOG_VOLUME:
                        comp.SetData(FogVolumeData{});
                        break;
                    case ComponentType::LOD_GROUP:
                        comp.SetData(LODGroupData{});
                        break;
                    case ComponentType::SPAWN_POINT:
                        comp.SetData(SpawnPointData{});
                        break;
                    case ComponentType::AUDIO_REVERB_ZONE:
                        comp.SetData(AudioReverbZoneData{});
                        break;
                    case ComponentType::WIND_ZONE:
                        comp.SetData(WindZoneData{});
                        break;
                    case ComponentType::PHYSICS_JOINT:
                        comp.SetData(PhysicsJointData{});
                        break;
                    case ComponentType::OCCLUDER:
                        comp.SetData(OccluderData{});
                        break;
                    case ComponentType::COVER_POINT:
                        comp.SetData(CoverPointData{});
                        break;
                    case ComponentType::TACTICAL_POINT:
                        comp.SetData(TacticalPointData{});
                        break;
                    case ComponentType::DESTRUCTIBLE:
                        comp.SetData(DestructibleData{});
                        break;
                    case ComponentType::CINEMATIC_TRIGGER:
                        comp.SetData(CinematicTriggerData{});
                        break;
                    case ComponentType::DIALOGUE_TRIGGER:
                        comp.SetData(DialogueTriggerData{});
                        break;
                    case ComponentType::AREA_BOUNDARY:
                        comp.SetData(AreaBoundaryData{});
                        break;
                    case ComponentType::BILLBOARD:
                        comp.SetData(BillboardData{});
                        break;
                    case ComponentType::AUDIO_LISTENER:
                        comp.SetData(AudioListenerData{});
                        break;
                    case ComponentType::CHARACTER_CONTROLLER:
                        comp.SetData(CharacterControllerData{});
                        break;
                    case ComponentType::NAV_REGION:
                        comp.SetData(NavRegionData{});
                        break;
                    case ComponentType::NAV_LINK:
                        comp.SetData(NavLinkData{});
                        break;
                    case ComponentType::SKYBOX:
                        comp.SetData(SkyboxData{});
                        break;
                    case ComponentType::CONSTANT_FORCE:
                        comp.SetData(ConstantForceData{});
                        break;
                    case ComponentType::FORCE_REGION:
                        comp.SetData(ForceRegionData{});
                        break;
                    case ComponentType::RAGDOLL:
                        comp.SetData(RagdollData{});
                        break;
                    case ComponentType::SOFT_BODY:
                        comp.SetData(SoftBodyData{});
                        break;
                    case ComponentType::VEHICLE:
                        comp.SetData(VehicleData{});
                        break;
                    case ComponentType::BUOYANCY_VOLUME:
                        comp.SetData(BuoyancyVolumeData{});
                        break;
                    case ComponentType::SPRING_ARM:
                        comp.SetData(SpringArmData{});
                        break;
                    case ComponentType::LINE_RENDERER:
                        comp.SetData(LineRendererData{});
                        break;
                    case ComponentType::TRAIL_RENDERER:
                        comp.SetData(TrailRendererData{});
                        break;
                    case ComponentType::TEXT_3D:
                        comp.SetData(Text3DData{});
                        break;
                    case ComponentType::FOLIAGE_VOLUME:
                        comp.SetData(FoliageVolumeData{});
                        break;
                    default:
                        break;
                    }

                    capturedScene->components.push_back(comp);
                }
            },
            [capturedScene, capturedID, type]()
            {
                SceneObject* o = capturedScene->FindObject(capturedID);
                if (o)
                {
                    auto& types = o->componentTypes;
                    types.erase(std::remove(types.begin(), types.end(), type), types.end());

                    // Remove component data
                    auto& comps = capturedScene->components;
                    comps.erase(std::remove_if(comps.begin(), comps.end(), [capturedID, type](const Component& c)
                                               { return c.objectID == capturedID && c.type == type; }),
                                comps.end());
                }
            },
            "Add Component"));
    }

    void InspectorPanel::RemoveComponent(ComponentType type)
    {
        if (!m_scene || m_inspectedObjectID == INVALID_OBJECT_ID)
            return;

        if (!HasComponent(type))
            return;

        const ObjectID capturedID = m_inspectedObjectID;
        SceneFile* capturedScene = m_scene;

        // Capture the component data for undo
        Component savedComp;
        for (auto& c : capturedScene->components)
        {
            if (c.objectID == capturedID && c.type == type)
            {
                savedComp = c;
                break;
            }
        }

        auto& history = Spark::Editor::CommandHistory::GetInstance();

        history.Execute(std::make_unique<Spark::Editor::LambdaCommand>(
            [capturedScene, capturedID, type]()
            {
                SceneObject* o = capturedScene->FindObject(capturedID);
                if (o)
                {
                    auto& types = o->componentTypes;
                    types.erase(std::remove(types.begin(), types.end(), type), types.end());

                    auto& comps = capturedScene->components;
                    comps.erase(std::remove_if(comps.begin(), comps.end(), [capturedID, type](const Component& c)
                                               { return c.objectID == capturedID && c.type == type; }),
                                comps.end());
                }
            },
            [capturedScene, capturedID, type, savedComp]()
            {
                SceneObject* o = capturedScene->FindObject(capturedID);
                if (o)
                {
                    o->componentTypes.push_back(type);
                    capturedScene->components.push_back(savedComp);
                }
            },
            "Remove Component"));
    }

    // ============================================================================
    // Helper: find the Component record for the inspected object of given type
    // ============================================================================

    Component* InspectorPanel::FindComponent(SceneFile* scene, ObjectID objectID, ComponentType type)
    {
        if (!scene)
            return nullptr;
        for (auto& c : scene->components)
        {
            if (c.objectID == objectID && c.type == type)
                return &c;
        }
        return nullptr;
    }

    // ============================================================================
    // Drawing Helpers
    // ============================================================================

    void InspectorPanel::DrawVec3Control(const char* label, float* values, float resetValue, float speed)
    {
        ImVec4 xColor(0.9f, 0.2f, 0.2f, 1.0f);
        ImVec4 yColor(0.2f, 0.8f, 0.2f, 1.0f);
        ImVec4 zColor(0.2f, 0.4f, 0.9f, 1.0f);

        ImGui::PushID(label);
        ImGui::Text("%s", label);
        ImGui::SameLine(90);

        float width = (ImGui::GetContentRegionAvail().x - 60) / 3.0f;

        // X
        ImGui::PushStyleColor(ImGuiCol_Button, xColor);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        if (ImGui::Button("X", ImVec2(20, 20)))
            values[0] = resetValue;
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(width);
        ImGui::DragFloat("##X", &values[0], speed);
        ImGui::SameLine();

        // Y
        ImGui::PushStyleColor(ImGuiCol_Button, yColor);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.9f, 0.3f, 1.0f));
        if (ImGui::Button("Y", ImVec2(20, 20)))
            values[1] = resetValue;
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(width);
        ImGui::DragFloat("##Y", &values[1], speed);
        ImGui::SameLine();

        // Z
        ImGui::PushStyleColor(ImGuiCol_Button, zColor);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.5f, 1.0f, 1.0f));
        if (ImGui::Button("Z", ImVec2(20, 20)))
            values[2] = resetValue;
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(width);
        ImGui::DragFloat("##Z", &values[2], speed);

        ImGui::PopID();
    }

    // ============================================================================
    // Object Properties
    // ============================================================================

    void InspectorPanel::RenderObjectProperties()
    {
        SceneObject* obj = nullptr;
        if (m_scene && m_inspectedObjectID != INVALID_OBJECT_ID)
        {
            obj = m_scene->FindObject(m_inspectedObjectID);
        }

        if (ImGui::CollapsingHeader(ICON_FA_CUBE " Object Properties", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent(4);

            // Name editing
            char nameBuffer[256];
            if (obj)
            {
                strncpy(nameBuffer, obj->name.c_str(), sizeof(nameBuffer) - 1);
            }
            else
            {
                strncpy(nameBuffer, m_inspectedObject.c_str(), sizeof(nameBuffer) - 1);
            }
            nameBuffer[sizeof(nameBuffer) - 1] = '\0';

            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##Name", nameBuffer, sizeof(nameBuffer), ImGuiInputTextFlags_EnterReturnsTrue))
            {
                if (obj)
                {
                    std::string oldName = obj->name;
                    std::string newName = nameBuffer;
                    SceneFile* capturedScene = m_scene;
                    ObjectID capturedID = m_inspectedObjectID;

                    auto& history = Spark::Editor::CommandHistory::GetInstance();
                    history.Execute(std::make_unique<Spark::Editor::LambdaCommand>(
                        [capturedScene, capturedID, newName]()
                        {
                            SceneObject* o = capturedScene->FindObject(capturedID);
                            if (o)
                                o->name = newName;
                        },
                        [capturedScene, capturedID, oldName]()
                        {
                            SceneObject* o = capturedScene->FindObject(capturedID);
                            if (o)
                                o->name = oldName;
                        },
                        "Rename '" + oldName + "' to '" + newName + "'"));

                    m_inspectedObject = newName;
                }
                else
                {
                    m_inspectedObject = nameBuffer;
                }
            }

            // Active / Static checkboxes
            if (obj)
            {
                bool active = obj->active;
                if (ImGui::Checkbox("Active", &active))
                {
                    bool oldActive = obj->active;
                    SceneFile* capturedScene = m_scene;
                    ObjectID capturedID = m_inspectedObjectID;
                    auto& history = Spark::Editor::CommandHistory::GetInstance();
                    history.Execute(std::make_unique<Spark::Editor::LambdaCommand>(
                        [capturedScene, capturedID, active]()
                        {
                            SceneObject* o = capturedScene->FindObject(capturedID);
                            if (o)
                                o->active = active;
                        },
                        [capturedScene, capturedID, oldActive]()
                        {
                            SceneObject* o = capturedScene->FindObject(capturedID);
                            if (o)
                                o->active = oldActive;
                        },
                        active ? "Activate Object" : "Deactivate Object"));
                }
                ImGui::SameLine();
                bool isStatic = obj->staticObject;
                if (ImGui::Checkbox("Static", &isStatic))
                {
                    bool oldStatic = obj->staticObject;
                    SceneFile* capturedScene = m_scene;
                    ObjectID capturedID = m_inspectedObjectID;
                    auto& history = Spark::Editor::CommandHistory::GetInstance();
                    history.Execute(std::make_unique<Spark::Editor::LambdaCommand>(
                        [capturedScene, capturedID, isStatic]()
                        {
                            SceneObject* o = capturedScene->FindObject(capturedID);
                            if (o)
                                o->staticObject = isStatic;
                        },
                        [capturedScene, capturedID, oldStatic]()
                        {
                            SceneObject* o = capturedScene->FindObject(capturedID);
                            if (o)
                                o->staticObject = oldStatic;
                        },
                        isStatic ? "Set Static" : "Set Dynamic"));
                }
            }
            else
            {
                bool active = true;
                ImGui::Checkbox("Active", &active);
                ImGui::SameLine();
                bool isStatic = false;
                ImGui::Checkbox("Static", &isStatic);
            }

            // Tag dropdown
            const char* tags[] = {"Default", "Player", "Enemy", "Weapon", "Terrain", "Trigger"};
            int currentTag = 0;
            if (obj)
            {
                for (int i = 0; i < IM_ARRAYSIZE(tags); ++i)
                {
                    if (obj->tag == tags[i])
                    {
                        currentTag = i;
                        break;
                    }
                }
            }
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
            if (ImGui::Combo("Tag", &currentTag, tags, IM_ARRAYSIZE(tags)))
            {
                if (obj)
                {
                    std::string oldTag = obj->tag;
                    std::string newTag = tags[currentTag];
                    SceneFile* capturedScene = m_scene;
                    ObjectID capturedID = m_inspectedObjectID;
                    auto& history = Spark::Editor::CommandHistory::GetInstance();
                    history.Execute(std::make_unique<Spark::Editor::LambdaCommand>(
                        [capturedScene, capturedID, newTag]()
                        {
                            SceneObject* o = capturedScene->FindObject(capturedID);
                            if (o)
                                o->tag = newTag;
                        },
                        [capturedScene, capturedID, oldTag]()
                        {
                            SceneObject* o = capturedScene->FindObject(capturedID);
                            if (o)
                                o->tag = oldTag;
                        },
                        "Change Tag to " + newTag));
                }
            }

            // Layer dropdown
            const char* layers[] = {"Default", "Ignore Raycast", "Water", "UI", "Ground", "Player"};
            int currentLayer = 0;
            if (obj)
            {
                currentLayer = (std::min)(obj->layer, static_cast<int>(IM_ARRAYSIZE(layers) - 1));
                currentLayer = (std::max)(currentLayer, 0);
            }
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
            if (ImGui::Combo("Layer", &currentLayer, layers, IM_ARRAYSIZE(layers)))
            {
                if (obj)
                {
                    int oldLayer = obj->layer;
                    int newLayer = currentLayer;
                    SceneFile* capturedScene = m_scene;
                    ObjectID capturedID = m_inspectedObjectID;
                    auto& history = Spark::Editor::CommandHistory::GetInstance();
                    history.Execute(std::make_unique<Spark::Editor::LambdaCommand>(
                        [capturedScene, capturedID, newLayer]()
                        {
                            SceneObject* o = capturedScene->FindObject(capturedID);
                            if (o)
                                o->layer = newLayer;
                        },
                        [capturedScene, capturedID, oldLayer]()
                        {
                            SceneObject* o = capturedScene->FindObject(capturedID);
                            if (o)
                                o->layer = oldLayer;
                        },
                        "Change Layer"));
                }
            }

            ImGui::Unindent(4);
        }
    }

    // ============================================================================
    // Component List
    // ============================================================================

    void InspectorPanel::RenderComponentList()
    {
        RenderTransformComponent();

        if (HasComponent(ComponentType::MESH_RENDERER))
            RenderMeshRendererComponent();
        if (HasComponent(ComponentType::LIGHT))
            RenderLightComponent();
        if (HasComponent(ComponentType::CAMERA))
            RenderCameraComponent();
        if (HasComponent(ComponentType::RIGID_BODY))
            RenderRigidBodyComponent();
        if (HasComponent(ComponentType::COLLIDER))
            RenderColliderComponent();
        if (HasComponent(ComponentType::AUDIO_SOURCE))
            RenderAudioSourceComponent();
        if (HasComponent(ComponentType::TERRAIN))
            RenderTerrainComponent();

        // 2D / Sprite components
        if (HasComponent(ComponentType::SPRITE_RENDERER))
            RenderSpriteRendererComponent();
        if (HasComponent(ComponentType::SPRITE_ANIMATOR))
            RenderSpriteAnimatorComponent();
        if (HasComponent(ComponentType::CAMERA_2D))
            RenderCamera2DComponent();
        if (HasComponent(ComponentType::TILEMAP))
            RenderTilemapComponent();
        if (HasComponent(ComponentType::NINE_SLICE))
            RenderNineSliceComponent();
        if (HasComponent(ComponentType::PARALLAX_BG))
            RenderParallaxBGComponent();
        if (HasComponent(ComponentType::PIXEL_PERFECT))
            RenderPixelPerfectComponent();
        if (HasComponent(ComponentType::RIGID_BODY_2D))
            RenderRigidBody2DComponent();
        if (HasComponent(ComponentType::COLLIDER_2D))
            RenderCollider2DComponent();

        // Animation & Effects
        if (HasComponent(ComponentType::PARTICLE_SYSTEM))
            RenderParticleEmitterComponent();
        if (HasComponent(ComponentType::ANIMATION))
            RenderAnimationControllerComponent();
        if (HasComponent(ComponentType::SCRIPT))
            RenderScriptComponent();

        // Gameplay components
        if (HasComponent(ComponentType::HEALTH))
            RenderHealthComponent();
        if (HasComponent(ComponentType::AI_AGENT))
            RenderAIAgentComponent();
        if (HasComponent(ComponentType::SPLINE))
            RenderSplineComponent();
        if (HasComponent(ComponentType::SPLINE_FOLLOWER))
            RenderSplineFollowerComponent();
        if (HasComponent(ComponentType::DECAL))
            RenderDecalComponent();
        if (HasComponent(ComponentType::PROJECTILE))
            RenderProjectileComponent();
        if (HasComponent(ComponentType::INTERACTION))
            RenderInteractionComponent();
        if (HasComponent(ComponentType::WEATHER))
            RenderWeatherComponent();
        if (HasComponent(ComponentType::NETWORK_IDENTITY))
            RenderNetworkIdentityComponent();
    }

    void InspectorPanel::RenderAddComponentMenu()
    {
        if (m_showAddComponentMenu)
        {
            ImGui::OpenPopup("AddComponentMenu");
        }

        if (ImGui::BeginPopup("AddComponentMenu"))
        {
            ImGui::Text(ICON_FA_SEARCH " Search Components");
            ImGui::Separator();

            if (ImGui::BeginMenu(ICON_FA_SHAPES " Rendering"))
            {
                if (ImGui::MenuItem("Mesh Renderer", nullptr, false, !HasComponent(ComponentType::MESH_RENDERER)))
                {
                    AddComponent(ComponentType::MESH_RENDERER);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Particle System", nullptr, false, !HasComponent(ComponentType::PARTICLE_SYSTEM)))
                {
                    AddComponent(ComponentType::PARTICLE_SYSTEM);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Line Renderer", nullptr, false, !HasComponent(ComponentType::LINE_RENDERER)))
                {
                    AddComponent(ComponentType::LINE_RENDERER);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Trail Renderer", nullptr, false, !HasComponent(ComponentType::TRAIL_RENDERER)))
                {
                    AddComponent(ComponentType::TRAIL_RENDERER);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Text 3D", nullptr, false, !HasComponent(ComponentType::TEXT_3D)))
                {
                    AddComponent(ComponentType::TEXT_3D);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Skybox", nullptr, false, !HasComponent(ComponentType::SKYBOX)))
                {
                    AddComponent(ComponentType::SKYBOX);
                    m_showAddComponentMenu = false;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(ICON_FA_LIGHTBULB " Lighting"))
            {
                if (ImGui::MenuItem("Light", nullptr, false, !HasComponent(ComponentType::LIGHT)))
                {
                    AddComponent(ComponentType::LIGHT);
                    m_showAddComponentMenu = false;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(ICON_FA_CAMERA " Camera"))
            {
                if (ImGui::MenuItem("Camera", nullptr, false, !HasComponent(ComponentType::CAMERA)))
                {
                    AddComponent(ComponentType::CAMERA);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Spring Arm", nullptr, false, !HasComponent(ComponentType::SPRING_ARM)))
                {
                    AddComponent(ComponentType::SPRING_ARM);
                    m_showAddComponentMenu = false;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(ICON_FA_VECTOR_SQUARE " Physics"))
            {
                if (ImGui::MenuItem("Box Collider", nullptr, false, !HasComponent(ComponentType::COLLIDER)))
                {
                    AddComponent(ComponentType::COLLIDER);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Rigidbody", nullptr, false, !HasComponent(ComponentType::RIGID_BODY)))
                {
                    AddComponent(ComponentType::RIGID_BODY);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Physics Joint", nullptr, false, !HasComponent(ComponentType::PHYSICS_JOINT)))
                {
                    AddComponent(ComponentType::PHYSICS_JOINT);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Character Controller", nullptr, false,
                                    !HasComponent(ComponentType::CHARACTER_CONTROLLER)))
                {
                    AddComponent(ComponentType::CHARACTER_CONTROLLER);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Constant Force", nullptr, false, !HasComponent(ComponentType::CONSTANT_FORCE)))
                {
                    AddComponent(ComponentType::CONSTANT_FORCE);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Ragdoll", nullptr, false, !HasComponent(ComponentType::RAGDOLL)))
                {
                    AddComponent(ComponentType::RAGDOLL);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Soft Body / Cloth", nullptr, false, !HasComponent(ComponentType::SOFT_BODY)))
                {
                    AddComponent(ComponentType::SOFT_BODY);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Vehicle", nullptr, false, !HasComponent(ComponentType::VEHICLE)))
                {
                    AddComponent(ComponentType::VEHICLE);
                    m_showAddComponentMenu = false;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(ICON_FA_CODE " Scripting"))
            {
                if (ImGui::MenuItem("Script", nullptr, false, !HasComponent(ComponentType::SCRIPT)))
                {
                    AddComponent(ComponentType::SCRIPT);
                    m_showAddComponentMenu = false;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(ICON_FA_VOLUME_UP " Audio"))
            {
                if (ImGui::MenuItem("Audio Source", nullptr, false, !HasComponent(ComponentType::AUDIO_SOURCE)))
                {
                    AddComponent(ComponentType::AUDIO_SOURCE);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Audio Listener", nullptr, false, !HasComponent(ComponentType::AUDIO_LISTENER)))
                {
                    AddComponent(ComponentType::AUDIO_LISTENER);
                    m_showAddComponentMenu = false;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(ICON_FA_RUNNING " Animation"))
            {
                if (ImGui::MenuItem("Animation", nullptr, false, !HasComponent(ComponentType::ANIMATION)))
                {
                    AddComponent(ComponentType::ANIMATION);
                    m_showAddComponentMenu = false;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(ICON_FA_IMAGE " 2D / Sprites"))
            {
                if (ImGui::MenuItem("Sprite Renderer", nullptr, false, !HasComponent(ComponentType::SPRITE_RENDERER)))
                {
                    AddComponent(ComponentType::SPRITE_RENDERER);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Sprite Animator", nullptr, false, !HasComponent(ComponentType::SPRITE_ANIMATOR)))
                {
                    AddComponent(ComponentType::SPRITE_ANIMATOR);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Tilemap", nullptr, false, !HasComponent(ComponentType::TILEMAP)))
                {
                    AddComponent(ComponentType::TILEMAP);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Camera 2D", nullptr, false, !HasComponent(ComponentType::CAMERA_2D)))
                {
                    AddComponent(ComponentType::CAMERA_2D);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Nine-Slice Sprite", nullptr, false, !HasComponent(ComponentType::NINE_SLICE)))
                {
                    AddComponent(ComponentType::NINE_SLICE);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Parallax Background", nullptr, false, !HasComponent(ComponentType::PARALLAX_BG)))
                {
                    AddComponent(ComponentType::PARALLAX_BG);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Pixel Perfect", nullptr, false, !HasComponent(ComponentType::PIXEL_PERFECT)))
                {
                    AddComponent(ComponentType::PIXEL_PERFECT);
                    m_showAddComponentMenu = false;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(ICON_FA_VECTOR_SQUARE " Physics 2D"))
            {
                if (ImGui::MenuItem("Rigid Body 2D", nullptr, false, !HasComponent(ComponentType::RIGID_BODY_2D)))
                {
                    AddComponent(ComponentType::RIGID_BODY_2D);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Collider 2D", nullptr, false, !HasComponent(ComponentType::COLLIDER_2D)))
                {
                    AddComponent(ComponentType::COLLIDER_2D);
                    m_showAddComponentMenu = false;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(ICON_FA_MOUNTAIN " Terrain"))
            {
                if (ImGui::MenuItem("Terrain", nullptr, false, !HasComponent(ComponentType::TERRAIN)))
                {
                    AddComponent(ComponentType::TERRAIN);
                    m_showAddComponentMenu = false;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(ICON_FA_CUBE " Volumes"))
            {
                if (ImGui::MenuItem("Trigger Volume", nullptr, false, !HasComponent(ComponentType::TRIGGER_VOLUME)))
                {
                    AddComponent(ComponentType::TRIGGER_VOLUME);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Post-Process Volume", nullptr, false,
                                    !HasComponent(ComponentType::POST_PROCESS_VOLUME)))
                {
                    AddComponent(ComponentType::POST_PROCESS_VOLUME);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Fog Volume", nullptr, false, !HasComponent(ComponentType::FOG_VOLUME)))
                {
                    AddComponent(ComponentType::FOG_VOLUME);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Audio Reverb Zone", nullptr, false,
                                    !HasComponent(ComponentType::AUDIO_REVERB_ZONE)))
                {
                    AddComponent(ComponentType::AUDIO_REVERB_ZONE);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Wind Zone", nullptr, false, !HasComponent(ComponentType::WIND_ZONE)))
                {
                    AddComponent(ComponentType::WIND_ZONE);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Cinematic Trigger", nullptr, false,
                                    !HasComponent(ComponentType::CINEMATIC_TRIGGER)))
                {
                    AddComponent(ComponentType::CINEMATIC_TRIGGER);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Area Boundary", nullptr, false, !HasComponent(ComponentType::AREA_BOUNDARY)))
                {
                    AddComponent(ComponentType::AREA_BOUNDARY);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Force Region", nullptr, false, !HasComponent(ComponentType::FORCE_REGION)))
                {
                    AddComponent(ComponentType::FORCE_REGION);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Buoyancy Volume", nullptr, false, !HasComponent(ComponentType::BUOYANCY_VOLUME)))
                {
                    AddComponent(ComponentType::BUOYANCY_VOLUME);
                    m_showAddComponentMenu = false;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(ICON_FA_LIGHTBULB " Probes"))
            {
                if (ImGui::MenuItem("Reflection Probe", nullptr, false, !HasComponent(ComponentType::REFLECTION_PROBE)))
                {
                    AddComponent(ComponentType::REFLECTION_PROBE);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Light Probe", nullptr, false, !HasComponent(ComponentType::LIGHT_PROBE)))
                {
                    AddComponent(ComponentType::LIGHT_PROBE);
                    m_showAddComponentMenu = false;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(ICON_FA_WATER " Environment"))
            {
                if (ImGui::MenuItem("Water Plane", nullptr, false, !HasComponent(ComponentType::WATER_PLANE)))
                {
                    AddComponent(ComponentType::WATER_PLANE);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("LOD Group", nullptr, false, !HasComponent(ComponentType::LOD_GROUP)))
                {
                    AddComponent(ComponentType::LOD_GROUP);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("NavMesh Obstacle", nullptr, false, !HasComponent(ComponentType::NAV_OBSTACLE)))
                {
                    AddComponent(ComponentType::NAV_OBSTACLE);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Occluder", nullptr, false, !HasComponent(ComponentType::OCCLUDER)))
                {
                    AddComponent(ComponentType::OCCLUDER);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Billboard", nullptr, false, !HasComponent(ComponentType::BILLBOARD)))
                {
                    AddComponent(ComponentType::BILLBOARD);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Foliage Volume", nullptr, false, !HasComponent(ComponentType::FOLIAGE_VOLUME)))
                {
                    AddComponent(ComponentType::FOLIAGE_VOLUME);
                    m_showAddComponentMenu = false;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(ICON_FA_HEART " Gameplay"))
            {
                if (ImGui::MenuItem("Health", nullptr, false, !HasComponent(ComponentType::HEALTH)))
                {
                    AddComponent(ComponentType::HEALTH);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Spawn Point", nullptr, false, !HasComponent(ComponentType::SPAWN_POINT)))
                {
                    AddComponent(ComponentType::SPAWN_POINT);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Interaction", nullptr, false, !HasComponent(ComponentType::INTERACTION)))
                {
                    AddComponent(ComponentType::INTERACTION);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Projectile", nullptr, false, !HasComponent(ComponentType::PROJECTILE)))
                {
                    AddComponent(ComponentType::PROJECTILE);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Decal", nullptr, false, !HasComponent(ComponentType::DECAL)))
                {
                    AddComponent(ComponentType::DECAL);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Weather Zone", nullptr, false, !HasComponent(ComponentType::WEATHER)))
                {
                    AddComponent(ComponentType::WEATHER);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Destructible", nullptr, false, !HasComponent(ComponentType::DESTRUCTIBLE)))
                {
                    AddComponent(ComponentType::DESTRUCTIBLE);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Dialogue Trigger", nullptr, false, !HasComponent(ComponentType::DIALOGUE_TRIGGER)))
                {
                    AddComponent(ComponentType::DIALOGUE_TRIGGER);
                    m_showAddComponentMenu = false;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(ICON_FA_BRAIN " AI"))
            {
                if (ImGui::MenuItem("AI Agent", nullptr, false, !HasComponent(ComponentType::AI_AGENT)))
                {
                    AddComponent(ComponentType::AI_AGENT);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Cover Point", nullptr, false, !HasComponent(ComponentType::COVER_POINT)))
                {
                    AddComponent(ComponentType::COVER_POINT);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Tactical Point", nullptr, false, !HasComponent(ComponentType::TACTICAL_POINT)))
                {
                    AddComponent(ComponentType::TACTICAL_POINT);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Nav Region", nullptr, false, !HasComponent(ComponentType::NAV_REGION)))
                {
                    AddComponent(ComponentType::NAV_REGION);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Nav Link", nullptr, false, !HasComponent(ComponentType::NAV_LINK)))
                {
                    AddComponent(ComponentType::NAV_LINK);
                    m_showAddComponentMenu = false;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(ICON_FA_BEZIER_CURVE " Splines"))
            {
                if (ImGui::MenuItem("Spline", nullptr, false, !HasComponent(ComponentType::SPLINE)))
                {
                    AddComponent(ComponentType::SPLINE);
                    m_showAddComponentMenu = false;
                }
                if (ImGui::MenuItem("Spline Follower", nullptr, false, !HasComponent(ComponentType::SPLINE_FOLLOWER)))
                {
                    AddComponent(ComponentType::SPLINE_FOLLOWER);
                    m_showAddComponentMenu = false;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(ICON_FA_NETWORK_WIRED " Networking"))
            {
                if (ImGui::MenuItem("Network Identity", nullptr, false, !HasComponent(ComponentType::NETWORK_IDENTITY)))
                {
                    AddComponent(ComponentType::NETWORK_IDENTITY);
                    m_showAddComponentMenu = false;
                }
                ImGui::EndMenu();
            }
            ImGui::EndPopup();
        }
    }

} // namespace SparkEditor
