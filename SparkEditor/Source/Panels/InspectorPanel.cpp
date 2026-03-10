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
#include <imgui.h>
#include <iostream>
#include <algorithm>
#include <cstring>

namespace SparkEditor
{

    InspectorPanel::InspectorPanel() : EditorPanel("Inspector", "inspector_panel") {}

    bool InspectorPanel::Initialize()
    {
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

        return std::find(obj->componentTypes.begin(), obj->componentTypes.end(), type)
               != obj->componentTypes.end();
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
                    comps.erase(std::remove_if(comps.begin(), comps.end(),
                                               [capturedID, type](const Component& c)
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
                    comps.erase(std::remove_if(comps.begin(), comps.end(),
                                               [capturedID, type](const Component& c)
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

    static Component* FindComponent(SceneFile* scene, ObjectID objectID, ComponentType type)
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

    static void DrawVec3Control(const char* label, float* values, float resetValue, float speed)
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
        {
            RenderMeshRendererComponent();
        }

        if (HasComponent(ComponentType::LIGHT))
        {
            RenderLightComponent();
        }

        if (HasComponent(ComponentType::CAMERA))
        {
            RenderCameraComponent();
        }

        if (HasComponent(ComponentType::RIGID_BODY))
        {
            RenderRigidBodyComponent();
        }

        if (HasComponent(ComponentType::COLLIDER))
        {
            RenderColliderComponent();
        }

        if (HasComponent(ComponentType::AUDIO_SOURCE))
        {
            RenderAudioSourceComponent();
        }
    }

    // ============================================================================
    // Transform Component
    // ============================================================================

    void InspectorPanel::RenderTransformComponent()
    {
        if (ImGui::CollapsingHeader(ICON_FA_ARROWS_ALT " Transform", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent(4);

            SceneObject* obj = nullptr;
            if (m_scene && m_inspectedObjectID != INVALID_OBJECT_ID)
            {
                obj = m_scene->FindObject(m_inspectedObjectID);
            }

            if (obj)
            {
                // Read actual transform data
                float position[3] = {obj->transform.position.x, obj->transform.position.y,
                                     obj->transform.position.z};
                // Convert quaternion to Euler angles for display
                // Simplified extraction (roll, pitch, yaw from quaternion)
                const auto& q = obj->transform.rotation;
                float sinrCosp = 2.0f * (q.w * q.x + q.y * q.z);
                float cosrCosp = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
                float roll = std::atan2(sinrCosp, cosrCosp) * (180.0f / 3.14159265f);

                float sinp = 2.0f * (q.w * q.y - q.z * q.x);
                float pitch = 0.0f;
                if (std::abs(sinp) >= 1.0f)
                    pitch = std::copysign(90.0f, sinp);
                else
                    pitch = std::asin(sinp) * (180.0f / 3.14159265f);

                float sinyCosp = 2.0f * (q.w * q.z + q.x * q.y);
                float cosyCosp = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
                float yaw = std::atan2(sinyCosp, cosyCosp) * (180.0f / 3.14159265f);

                float rotation[3] = {roll, pitch, yaw};
                float scale[3] = {obj->transform.scale.x, obj->transform.scale.y, obj->transform.scale.z};

                // Save old values for undo
                float oldPos[3] = {position[0], position[1], position[2]};
                float oldRot[3] = {rotation[0], rotation[1], rotation[2]};
                float oldScale[3] = {scale[0], scale[1], scale[2]};

                DrawVec3Control("Position", position, 0.0f, 0.1f);
                DrawVec3Control("Rotation", rotation, 0.0f, 1.0f);
                DrawVec3Control("Scale", scale, 1.0f, 0.1f);

                // Write back if changed
                bool posChanged = (position[0] != oldPos[0] || position[1] != oldPos[1]
                                   || position[2] != oldPos[2]);
                bool rotChanged = (rotation[0] != oldRot[0] || rotation[1] != oldRot[1]
                                   || rotation[2] != oldRot[2]);
                bool scaleChanged = (scale[0] != oldScale[0] || scale[1] != oldScale[1]
                                     || scale[2] != oldScale[2]);

                if (posChanged || rotChanged || scaleChanged)
                {
                    XMFLOAT3 newPos = {position[0], position[1], position[2]};
                    XMFLOAT3 newScale = {scale[0], scale[1], scale[2]};

                    // Convert Euler back to quaternion
                    float radX = rotation[0] * (3.14159265f / 180.0f);
                    float radY = rotation[1] * (3.14159265f / 180.0f);
                    float radZ = rotation[2] * (3.14159265f / 180.0f);
                    float cx = std::cos(radX * 0.5f), sx = std::sin(radX * 0.5f);
                    float cy = std::cos(radY * 0.5f), sy = std::sin(radY * 0.5f);
                    float cz = std::cos(radZ * 0.5f), sz = std::sin(radZ * 0.5f);

                    XMFLOAT4 newRot = {
                        sx * cy * cz - cx * sy * sz,
                        cx * sy * cz + sx * cy * sz,
                        cx * cy * sz - sx * sy * cz,
                        cx * cy * cz + sx * sy * sz
                    };

                    Transform oldTransform = obj->transform;
                    SceneFile* capturedScene = m_scene;
                    ObjectID capturedID = m_inspectedObjectID;

                    auto& history = Spark::Editor::CommandHistory::GetInstance();
                    history.Execute(std::make_unique<Spark::Editor::LambdaCommand>(
                        [capturedScene, capturedID, newPos, newRot, newScale]()
                        {
                            SceneObject* o = capturedScene->FindObject(capturedID);
                            if (o)
                            {
                                o->transform.position = newPos;
                                o->transform.rotation = newRot;
                                o->transform.scale = newScale;
                            }
                        },
                        [capturedScene, capturedID, oldTransform]()
                        {
                            SceneObject* o = capturedScene->FindObject(capturedID);
                            if (o)
                            {
                                o->transform.position = oldTransform.position;
                                o->transform.rotation = oldTransform.rotation;
                                o->transform.scale = oldTransform.scale;
                            }
                        },
                        "Transform Change"));
                }
            }
            else
            {
                // Fallback for when no scene is bound
                static float position[3] = {0.0f, 0.0f, 0.0f};
                static float rotation[3] = {0.0f, 0.0f, 0.0f};
                static float scale[3] = {1.0f, 1.0f, 1.0f};
                DrawVec3Control("Position", position, 0.0f, 0.1f);
                DrawVec3Control("Rotation", rotation, 0.0f, 1.0f);
                DrawVec3Control("Scale", scale, 1.0f, 0.1f);
            }

            ImGui::Unindent(4);
        }
    }

    // ============================================================================
    // Mesh Renderer Component
    // ============================================================================

    void InspectorPanel::RenderMeshRendererComponent()
    {
        bool headerOpen = ImGui::CollapsingHeader(ICON_FA_SHAPES " Mesh Renderer");

        // Right-click context menu for removal
        if (ImGui::BeginPopupContextItem("##MeshRendererCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
            {
                RemoveComponent(ComponentType::MESH_RENDERER);
            }
            ImGui::EndPopup();
        }

        if (headerOpen)
        {
            ImGui::Indent(4);

            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::MESH_RENDERER);
            MeshRenderer* mr = comp ? comp->GetData<MeshRenderer>() : nullptr;

            if (mr)
            {
                // Mesh path
                char meshBuf[256];
                strncpy(meshBuf, mr->meshAssetPath.c_str(), sizeof(meshBuf) - 1);
                meshBuf[sizeof(meshBuf) - 1] = '\0';
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputText("Mesh", meshBuf, sizeof(meshBuf), ImGuiInputTextFlags_EnterReturnsTrue))
                {
                    mr->meshAssetPath = meshBuf;
                }

                // Material path
                char matBuf[256];
                strncpy(matBuf, mr->materialAssetPath.c_str(), sizeof(matBuf) - 1);
                matBuf[sizeof(matBuf) - 1] = '\0';
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputText("Material", matBuf, sizeof(matBuf), ImGuiInputTextFlags_EnterReturnsTrue))
                {
                    mr->materialAssetPath = matBuf;
                }

                ImGui::Checkbox("Cast Shadows", &mr->castShadows);
                ImGui::SameLine();
                ImGui::Checkbox("Receive Shadows", &mr->receiveShadows);

                ImGui::DragInt("Render Layer", &mr->renderLayer, 1, 0, 31);

                float tint[4] = {mr->tintColor.x, mr->tintColor.y, mr->tintColor.z, mr->tintColor.w};
                if (ImGui::ColorEdit4("Tint Color", tint))
                {
                    mr->tintColor = {tint[0], tint[1], tint[2], tint[3]};
                }
            }
            else
            {
                ImGui::TextDisabled("(Component data unavailable)");
            }

            ImGui::Unindent(4);
        }
    }

    // ============================================================================
    // Light Component
    // ============================================================================

    void InspectorPanel::RenderLightComponent()
    {
        bool headerOpen = ImGui::CollapsingHeader(ICON_FA_LIGHTBULB " Light");

        if (ImGui::BeginPopupContextItem("##LightCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
            {
                RemoveComponent(ComponentType::LIGHT);
            }
            ImGui::EndPopup();
        }

        if (headerOpen)
        {
            ImGui::Indent(4);

            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::LIGHT);
            SparkEditor::Light* light = comp ? comp->GetData<SparkEditor::Light>() : nullptr;

            if (light)
            {
                const char* lightTypes[] = {"Directional", "Point", "Spot", "Area"};
                int lightType = static_cast<int>(light->type);
                if (ImGui::Combo("Type", &lightType, lightTypes, IM_ARRAYSIZE(lightTypes)))
                {
                    light->type = static_cast<SparkEditor::Light::Type>(lightType);
                }

                float color[3] = {light->color.x, light->color.y, light->color.z};
                if (ImGui::ColorEdit3("Color", color))
                {
                    light->color = {color[0], color[1], color[2]};
                }

                ImGui::DragFloat("Intensity", &light->intensity, 0.1f, 0.0f, 100.0f);

                if (light->type == SparkEditor::Light::POINT || light->type == SparkEditor::Light::SPOT)
                {
                    ImGui::DragFloat("Range", &light->range, 0.5f, 0.0f, 1000.0f);
                }

                if (light->type == SparkEditor::Light::SPOT)
                {
                    ImGui::DragFloat("Spot Angle", &light->spotAngle, 1.0f, 1.0f, 179.0f);
                    ImGui::DragFloat("Inner Angle", &light->spotInnerAngle, 1.0f, 1.0f, light->spotAngle);
                }

                ImGui::Checkbox("Cast Shadows", &light->castShadows);

                if (light->castShadows)
                {
                    const char* shadowSizes[] = {"256", "512", "1024", "2048", "4096"};
                    int shadowSizeValues[] = {256, 512, 1024, 2048, 4096};
                    int currentShadowIdx = 2; // Default to 1024
                    for (int i = 0; i < 5; ++i)
                    {
                        if (light->shadowMapSize == shadowSizeValues[i])
                        {
                            currentShadowIdx = i;
                            break;
                        }
                    }
                    if (ImGui::Combo("Shadow Map Size", &currentShadowIdx, shadowSizes, IM_ARRAYSIZE(shadowSizes)))
                    {
                        light->shadowMapSize = shadowSizeValues[currentShadowIdx];
                    }
                }
            }
            else
            {
                ImGui::TextDisabled("(Component data unavailable)");
            }

            ImGui::Unindent(4);
        }
    }

    // ============================================================================
    // Camera Component
    // ============================================================================

    void InspectorPanel::RenderCameraComponent()
    {
        bool headerOpen = ImGui::CollapsingHeader(ICON_FA_CAMERA " Camera");

        if (ImGui::BeginPopupContextItem("##CameraCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
            {
                RemoveComponent(ComponentType::CAMERA);
            }
            ImGui::EndPopup();
        }

        if (headerOpen)
        {
            ImGui::Indent(4);

            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::CAMERA);
            SparkEditor::Camera* cam = comp ? comp->GetData<SparkEditor::Camera>() : nullptr;

            if (cam)
            {
                const char* projTypes[] = {"Perspective", "Orthographic"};
                int projType = static_cast<int>(cam->projectionType);
                if (ImGui::Combo("Projection", &projType, projTypes, IM_ARRAYSIZE(projTypes)))
                {
                    cam->projectionType = static_cast<SparkEditor::Camera::ProjectionType>(projType);
                }

                if (cam->projectionType == SparkEditor::Camera::PERSPECTIVE)
                {
                    ImGui::DragFloat("Field of View", &cam->fieldOfView, 1.0f, 1.0f, 179.0f);
                }
                else
                {
                    ImGui::DragFloat("Ortho Size", &cam->orthographicSize, 0.1f, 0.01f, 1000.0f);
                }

                ImGui::DragFloat("Near Plane", &cam->nearPlane, 0.01f, 0.001f, cam->farPlane);
                ImGui::DragFloat("Far Plane", &cam->farPlane, 1.0f, cam->nearPlane, 100000.0f);

                float clearCol[4] = {cam->clearColor.x, cam->clearColor.y, cam->clearColor.z, cam->clearColor.w};
                if (ImGui::ColorEdit4("Clear Color", clearCol))
                {
                    cam->clearColor = {clearCol[0], clearCol[1], clearCol[2], clearCol[3]};
                }

                ImGui::Checkbox("Main Camera", &cam->isMainCamera);
            }
            else
            {
                ImGui::TextDisabled("(Component data unavailable)");
            }

            ImGui::Unindent(4);
        }
    }

    // ============================================================================
    // Rigid Body Component
    // ============================================================================

    void InspectorPanel::RenderRigidBodyComponent()
    {
        bool headerOpen = ImGui::CollapsingHeader(ICON_FA_GLOBE " Rigidbody");

        if (ImGui::BeginPopupContextItem("##RigidBodyCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
            {
                RemoveComponent(ComponentType::RIGID_BODY);
            }
            ImGui::EndPopup();
        }

        if (headerOpen)
        {
            ImGui::Indent(4);

            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::RIGID_BODY);
            RigidBody* rb = comp ? comp->GetData<RigidBody>() : nullptr;

            if (rb)
            {
                const char* bodyTypes[] = {"Static", "Kinematic", "Dynamic"};
                int bodyType = static_cast<int>(rb->bodyType);
                if (ImGui::Combo("Body Type", &bodyType, bodyTypes, IM_ARRAYSIZE(bodyTypes)))
                {
                    rb->bodyType = static_cast<RigidBody::BodyType>(bodyType);
                }

                if (rb->bodyType == RigidBody::DYNAMIC)
                {
                    ImGui::DragFloat("Mass", &rb->mass, 0.1f, 0.01f, 10000.0f);
                }

                ImGui::DragFloat("Drag", &rb->drag, 0.01f, 0.0f, 100.0f);
                ImGui::DragFloat("Angular Drag", &rb->angularDrag, 0.01f, 0.0f, 100.0f);
                ImGui::Checkbox("Use Gravity", &rb->useGravity);
                ImGui::Checkbox("Is Kinematic", &rb->isKinematic);

                if (ImGui::TreeNode("Freeze Constraints"))
                {
                    ImGui::Text("Position:");
                    ImGui::SameLine();
                    ImGui::Checkbox("X##FPX", &rb->freezePositionX);
                    ImGui::SameLine();
                    ImGui::Checkbox("Y##FPY", &rb->freezePositionY);
                    ImGui::SameLine();
                    ImGui::Checkbox("Z##FPZ", &rb->freezePositionZ);

                    ImGui::Text("Rotation:");
                    ImGui::SameLine();
                    ImGui::Checkbox("X##FRX", &rb->freezeRotationX);
                    ImGui::SameLine();
                    ImGui::Checkbox("Y##FRY", &rb->freezeRotationY);
                    ImGui::SameLine();
                    ImGui::Checkbox("Z##FRZ", &rb->freezeRotationZ);

                    ImGui::TreePop();
                }
            }
            else
            {
                ImGui::TextDisabled("(Component data unavailable)");
            }

            ImGui::Unindent(4);
        }
    }

    // ============================================================================
    // Collider Component
    // ============================================================================

    void InspectorPanel::RenderColliderComponent()
    {
        bool headerOpen = ImGui::CollapsingHeader(ICON_FA_VECTOR_SQUARE " Collider");

        if (ImGui::BeginPopupContextItem("##ColliderCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
            {
                RemoveComponent(ComponentType::COLLIDER);
            }
            ImGui::EndPopup();
        }

        if (headerOpen)
        {
            ImGui::Indent(4);

            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::COLLIDER);
            SparkEditor::Collider* col = comp ? comp->GetData<SparkEditor::Collider>() : nullptr;

            if (col)
            {
                const char* colliderTypes[] = {"Box", "Sphere", "Capsule", "Mesh", "Terrain"};
                int colType = static_cast<int>(col->type);
                if (ImGui::Combo("Shape", &colType, colliderTypes, IM_ARRAYSIZE(colliderTypes)))
                {
                    col->type = static_cast<SparkEditor::Collider::ColliderType>(colType);
                }

                ImGui::Checkbox("Is Trigger", &col->isTrigger);

                float center[3] = {col->center.x, col->center.y, col->center.z};
                if (ImGui::DragFloat3("Center", center, 0.1f))
                {
                    col->center = {center[0], center[1], center[2]};
                }

                switch (col->type)
                {
                case SparkEditor::Collider::BOX:
                {
                    float size[3] = {col->size.x, col->size.y, col->size.z};
                    if (ImGui::DragFloat3("Size", size, 0.1f, 0.001f))
                    {
                        col->size = {size[0], size[1], size[2]};
                    }
                    break;
                }
                case SparkEditor::Collider::SPHERE:
                {
                    ImGui::DragFloat("Radius", &col->radius, 0.01f, 0.001f, 1000.0f);
                    break;
                }
                case SparkEditor::Collider::CAPSULE:
                {
                    ImGui::DragFloat("Radius", &col->radius, 0.01f, 0.001f, 1000.0f);
                    ImGui::DragFloat("Height", &col->height, 0.1f, 0.001f, 1000.0f);
                    break;
                }
                case SparkEditor::Collider::MESH:
                {
                    char meshBuf[256];
                    strncpy(meshBuf, col->meshAssetPath.c_str(), sizeof(meshBuf) - 1);
                    meshBuf[sizeof(meshBuf) - 1] = '\0';
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::InputText("Mesh##ColMesh", meshBuf, sizeof(meshBuf),
                                         ImGuiInputTextFlags_EnterReturnsTrue))
                    {
                        col->meshAssetPath = meshBuf;
                    }
                    break;
                }
                default:
                    break;
                }

                ImGui::Separator();
                ImGui::DragFloat("Friction", &col->friction, 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("Bounciness", &col->bounciness, 0.01f, 0.0f, 1.0f);
            }
            else
            {
                ImGui::TextDisabled("(Component data unavailable)");
            }

            ImGui::Unindent(4);
        }
    }

    // ============================================================================
    // Audio Source Component
    // ============================================================================

    void InspectorPanel::RenderAudioSourceComponent()
    {
        bool headerOpen = ImGui::CollapsingHeader(ICON_FA_VOLUME_UP " Audio Source");

        if (ImGui::BeginPopupContextItem("##AudioSourceCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
            {
                RemoveComponent(ComponentType::AUDIO_SOURCE);
            }
            ImGui::EndPopup();
        }

        if (headerOpen)
        {
            ImGui::Indent(4);

            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::AUDIO_SOURCE);
            AudioSource* audio = comp ? comp->GetData<AudioSource>() : nullptr;

            if (audio)
            {
                char clipBuf[256];
                strncpy(clipBuf, audio->audioClipPath.c_str(), sizeof(clipBuf) - 1);
                clipBuf[sizeof(clipBuf) - 1] = '\0';
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputText("Audio Clip", clipBuf, sizeof(clipBuf),
                                     ImGuiInputTextFlags_EnterReturnsTrue))
                {
                    audio->audioClipPath = clipBuf;
                }

                ImGui::Checkbox("Play On Awake", &audio->playOnAwake);
                ImGui::SameLine();
                ImGui::Checkbox("Loop", &audio->loop);

                ImGui::SliderFloat("Volume", &audio->volume, 0.0f, 1.0f);
                ImGui::DragFloat("Pitch", &audio->pitch, 0.01f, 0.01f, 3.0f);

                ImGui::SliderFloat("Spatial Blend", &audio->spatialBlend, 0.0f, 1.0f, "%.2f (2D-3D)");

                if (audio->spatialBlend > 0.0f)
                {
                    ImGui::DragFloat("Min Distance", &audio->minDistance, 0.1f, 0.0f, audio->maxDistance);
                    ImGui::DragFloat("Max Distance", &audio->maxDistance, 1.0f, audio->minDistance, 10000.0f);
                }

                ImGui::SliderInt("Priority", &audio->priority, 0, 255);
            }
            else
            {
                ImGui::TextDisabled("(Component data unavailable)");
            }

            ImGui::Unindent(4);
        }
    }

    // ============================================================================
    // Add Component Menu
    // ============================================================================

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
            ImGui::EndPopup();
        }
    }

} // namespace SparkEditor
