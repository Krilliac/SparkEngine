/**
 * @file InspectorComponentRenderers_Core3D.cpp
 * @brief Inspector renderers for core 3D components: Transform, MeshRenderer, Light, Camera, RigidBody, Collider
 *
 * Split from InspectorComponentRenderers.cpp for maintainability.
 */

#include "InspectorPanel.h"
#include "../AssetPipeline/EditorAssetDrag.h"
#include "../Core/EditorIcons.h"
#include "../Core/EditorFonts.h"
#include "../CommandHistory.h"
#include "Utils/LogMacros.h"
#include "Utils/MathUtils.h"
#include "SparkUI/Runtime.h"
#include "SparkUI/Widgets.h"
#include <imgui.h>
#include <algorithm>
#include <cstring>
#include <initializer_list>

namespace SparkEditor
{

    // ============================================================================
    // Transform Component
    // ============================================================================

    void InspectorPanel::RenderTransformComponent()
    {
        if (Spark::Ui::SectionHeader("Transform", ICON_FA_ARROWS_ALT, true))
        {
            ImGui::Indent(Spark::Ui::Px(4.0f));

            SceneObject* obj = nullptr;
            if (m_scene && m_inspectedObjectID != INVALID_OBJECT_ID)
            {
                obj = m_scene->FindObject(m_inspectedObjectID);
            }

            if (obj)
            {
                // Read actual transform data
                float position[3] = {obj->transform.position.x, obj->transform.position.y, obj->transform.position.z};
                // Convert quaternion to Euler angles for display
                const auto& q = obj->transform.rotation;
                XMFLOAT3 euler = MathUtils::QuaternionToEulerDegrees(q.x, q.y, q.z, q.w);
                float rotation[3] = {euler.x, euler.y, euler.z};
                float scale[3] = {obj->transform.scale.x, obj->transform.scale.y, obj->transform.scale.z};

                // Save old values for undo
                float oldPos[3] = {position[0], position[1], position[2]};
                float oldRot[3] = {rotation[0], rotation[1], rotation[2]};
                float oldScale[3] = {scale[0], scale[1], scale[2]};

                if (Spark::Ui::BeginPropertyGrid("##Transform"))
                {
                    Spark::Ui::PropertyLabel("Position");
                    DrawVec3Control("##Position", position, 0.0f, 0.1f);
                    Spark::Ui::PropertyLabel("Rotation");
                    DrawVec3Control("##Rotation", rotation, 0.0f, 1.0f);
                    Spark::Ui::PropertyLabel("Scale");
                    DrawVec3Control("##Scale", scale, 1.0f, 0.1f);
                    Spark::Ui::EndPropertyGrid();
                }

                // Write back if changed
                bool posChanged = (position[0] != oldPos[0] || position[1] != oldPos[1] || position[2] != oldPos[2]);
                bool rotChanged = (rotation[0] != oldRot[0] || rotation[1] != oldRot[1] || rotation[2] != oldRot[2]);
                bool scaleChanged = (scale[0] != oldScale[0] || scale[1] != oldScale[1] || scale[2] != oldScale[2]);

                if (posChanged || rotChanged || scaleChanged)
                {
                    SPARK_LOG_DEBUG(Spark::LogCategory::Editor,
                                    "Inspector: transform changed for object %llu (pos=%d rot=%d scale=%d)",
                                    static_cast<unsigned long long>(m_inspectedObjectID), posChanged, rotChanged,
                                    scaleChanged);
                    XMFLOAT3 newPos = {position[0], position[1], position[2]};
                    XMFLOAT3 newScale = {scale[0], scale[1], scale[2]};

                    // Convert Euler back to quaternion
                    XMFLOAT4 newRot = MathUtils::EulerDegreesToQuaternion(rotation[0], rotation[1], rotation[2]);

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
                if (Spark::Ui::BeginPropertyGrid("##Transform"))
                {
                    Spark::Ui::PropertyLabel("Position");
                    DrawVec3Control("##Position", position, 0.0f, 0.1f);
                    Spark::Ui::PropertyLabel("Rotation");
                    DrawVec3Control("##Rotation", rotation, 0.0f, 1.0f);
                    Spark::Ui::PropertyLabel("Scale");
                    DrawVec3Control("##Scale", scale, 1.0f, 0.1f);
                    Spark::Ui::EndPropertyGrid();
                }
            }

            ImGui::Unindent(Spark::Ui::Px(4.0f));
        }
    }

    // ============================================================================
    // Mesh Renderer Component
    // ============================================================================

    void InspectorPanel::RenderMeshRendererComponent()
    {
        bool headerOpen = Spark::Ui::SectionHeader("Mesh Renderer", ICON_FA_SHAPES, false);

        // Right-click context menu for removal
        if (ImGui::BeginPopupContextItem("##MeshRendererCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
            {
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "Inspector: removing MeshRenderer from object %llu",
                               static_cast<unsigned long long>(m_inspectedObjectID));
                RemoveComponent(ComponentType::MESH_RENDERER);
            }
            ImGui::EndPopup();
        }

        if (headerOpen)
        {
            ImGui::Indent(Spark::Ui::Px(4.0f));

            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::MESH_RENDERER);
            MeshRenderer* mr = comp ? comp->GetData<MeshRenderer>() : nullptr;

            if (mr)
            {
                if (Spark::Ui::BeginPropertyGrid("##MeshRendererProps"))
                {
                    // Mesh path
                    char meshBuf[256];
                    strncpy(meshBuf, mr->meshAssetPath.c_str(), sizeof(meshBuf) - 1);
                    meshBuf[sizeof(meshBuf) - 1] = '\0';
                    Spark::Ui::PropertyLabel("Mesh");
                    if (ImGui::InputText("##Mesh", meshBuf, sizeof(meshBuf), ImGuiInputTextFlags_EnterReturnsTrue))
                    {
                        SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Inspector: mesh asset changed to '%s'", meshBuf);
                        const std::string oldPath = mr->meshAssetPath;
                        const std::string newPath = meshBuf;
                        SceneFile* capturedScene = m_scene;
                        ObjectID capturedID = m_inspectedObjectID;
                        auto& history = Spark::Editor::CommandHistory::GetInstance();
                        history.Execute(std::make_unique<Spark::Editor::LambdaCommand>(
                            [capturedScene, capturedID, newPath]()
                            {
                                if (Component* c =
                                        FindComponent(capturedScene, capturedID, ComponentType::MESH_RENDERER))
                                {
                                    if (MeshRenderer* data = c->GetData<MeshRenderer>())
                                    {
                                        data->meshAssetPath = newPath;
                                    }
                                }
                            },
                            [capturedScene, capturedID, oldPath]()
                            {
                                if (Component* c =
                                        FindComponent(capturedScene, capturedID, ComponentType::MESH_RENDERER))
                                {
                                    if (MeshRenderer* data = c->GetData<MeshRenderer>())
                                    {
                                        data->meshAssetPath = oldPath;
                                    }
                                }
                            },
                            "Assign Mesh Asset"));
                    }
                    if (ImGui::BeginDragDropTarget())
                    {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetDragPayloadType))
                        {
                            std::string newPath;
                            if (DecodeAssetDragPayload(payload->Data, payload->DataSize, EditorAssetKind::Mesh,
                                                       newPath))
                            {
                                const std::string oldPath = mr->meshAssetPath;
                                SceneFile* capturedScene = m_scene;
                                ObjectID capturedID = m_inspectedObjectID;
                                auto& history = Spark::Editor::CommandHistory::GetInstance();
                                history.Execute(std::make_unique<Spark::Editor::LambdaCommand>(
                                    [capturedScene, capturedID, newPath]()
                                    {
                                        if (Component* c =
                                                FindComponent(capturedScene, capturedID, ComponentType::MESH_RENDERER))
                                            if (MeshRenderer* data = c->GetData<MeshRenderer>())
                                                data->meshAssetPath = newPath;
                                    },
                                    [capturedScene, capturedID, oldPath]()
                                    {
                                        if (Component* c =
                                                FindComponent(capturedScene, capturedID, ComponentType::MESH_RENDERER))
                                            if (MeshRenderer* data = c->GetData<MeshRenderer>())
                                                data->meshAssetPath = oldPath;
                                    },
                                    "Assign Mesh Asset"));
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }

                    // Material path
                    char matBuf[256];
                    strncpy(matBuf, mr->materialAssetPath.c_str(), sizeof(matBuf) - 1);
                    matBuf[sizeof(matBuf) - 1] = '\0';
                    Spark::Ui::PropertyLabel("Material");
                    if (ImGui::InputText("##Material", matBuf, sizeof(matBuf), ImGuiInputTextFlags_EnterReturnsTrue))
                    {
                        SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Inspector: material asset changed to '%s'",
                                        matBuf);
                        const std::string oldPath = mr->materialAssetPath;
                        const std::string newPath = matBuf;
                        SceneFile* capturedScene = m_scene;
                        ObjectID capturedID = m_inspectedObjectID;
                        auto& history = Spark::Editor::CommandHistory::GetInstance();
                        history.Execute(std::make_unique<Spark::Editor::LambdaCommand>(
                            [capturedScene, capturedID, newPath]()
                            {
                                if (Component* c =
                                        FindComponent(capturedScene, capturedID, ComponentType::MESH_RENDERER))
                                {
                                    if (MeshRenderer* data = c->GetData<MeshRenderer>())
                                    {
                                        data->materialAssetPath = newPath;
                                    }
                                }
                            },
                            [capturedScene, capturedID, oldPath]()
                            {
                                if (Component* c =
                                        FindComponent(capturedScene, capturedID, ComponentType::MESH_RENDERER))
                                {
                                    if (MeshRenderer* data = c->GetData<MeshRenderer>())
                                    {
                                        data->materialAssetPath = oldPath;
                                    }
                                }
                            },
                            "Assign Material Asset"));
                    }
                    if (ImGui::BeginDragDropTarget())
                    {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetDragPayloadType))
                        {
                            std::string newPath;
                            if (DecodeAssetDragPayload(payload->Data, payload->DataSize, EditorAssetKind::Material,
                                                       newPath))
                            {
                                const std::string oldPath = mr->materialAssetPath;
                                SceneFile* capturedScene = m_scene;
                                ObjectID capturedID = m_inspectedObjectID;
                                auto& history = Spark::Editor::CommandHistory::GetInstance();
                                history.Execute(std::make_unique<Spark::Editor::LambdaCommand>(
                                    [capturedScene, capturedID, newPath]()
                                    {
                                        if (Component* c =
                                                FindComponent(capturedScene, capturedID, ComponentType::MESH_RENDERER))
                                            if (MeshRenderer* data = c->GetData<MeshRenderer>())
                                                data->materialAssetPath = newPath;
                                    },
                                    [capturedScene, capturedID, oldPath]()
                                    {
                                        if (Component* c =
                                                FindComponent(capturedScene, capturedID, ComponentType::MESH_RENDERER))
                                            if (MeshRenderer* data = c->GetData<MeshRenderer>())
                                                data->materialAssetPath = oldPath;
                                    },
                                    "Assign Material Asset"));
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }

                    Spark::Ui::PropertyLabel("Cast Shadows");
                    ImGui::Checkbox("##CastShadows", &mr->castShadows);
                    Spark::Ui::PropertyLabel("Receive Shadows");
                    ImGui::Checkbox("##ReceiveShadows", &mr->receiveShadows);

                    Spark::Ui::PropertyLabel("Render Layer");
                    ImGui::DragInt("##RenderLayer", &mr->renderLayer, 1, 0, 31);

                    float tint[4] = {mr->tintColor.x, mr->tintColor.y, mr->tintColor.z, mr->tintColor.w};
                    Spark::Ui::PropertyLabel("Tint Color");
                    if (ImGui::ColorEdit4("##TintColor", tint))
                    {
                        mr->tintColor = {tint[0], tint[1], tint[2], tint[3]};
                    }
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

    // ============================================================================
    // Light Component
    // ============================================================================

    void InspectorPanel::RenderLightComponent()
    {
        bool headerOpen = Spark::Ui::SectionHeader("Light", ICON_FA_LIGHTBULB, false);

        if (ImGui::BeginPopupContextItem("##LightCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
            {
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "Inspector: removing Light from object %llu",
                               static_cast<unsigned long long>(m_inspectedObjectID));
                RemoveComponent(ComponentType::LIGHT);
            }
            ImGui::EndPopup();
        }

        if (headerOpen)
        {
            ImGui::Indent(Spark::Ui::Px(4.0f));

            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::LIGHT);
            SparkEditor::Light* light = comp ? comp->GetData<SparkEditor::Light>() : nullptr;

            if (light)
            {
                if (Spark::Ui::BeginPropertyGrid("##LightProps"))
                {
                    const char* lightTypes[] = {"Directional", "Point", "Spot", "Area"};
                    int lightType = static_cast<int>(light->type);
                    Spark::Ui::PropertyLabel("Type");
                    if (ImGui::Combo("##Type", &lightType, lightTypes, IM_ARRAYSIZE(lightTypes)))
                    {
                        SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Inspector: light type changed to '%s'",
                                        lightTypes[lightType]);
                        light->type = static_cast<SparkEditor::Light::Type>(lightType);
                    }

                    float color[3] = {light->color.x, light->color.y, light->color.z};
                    Spark::Ui::PropertyLabel("Color");
                    if (ImGui::ColorEdit3("##Color", color))
                    {
                        light->color = {color[0], color[1], color[2]};
                    }

                    Spark::Ui::PropertyLabel("Intensity");
                    ImGui::DragFloat("##Intensity", &light->intensity, 0.1f, 0.0f, 100.0f);

                    if (light->type == SparkEditor::Light::POINT || light->type == SparkEditor::Light::SPOT)
                    {
                        Spark::Ui::PropertyLabel("Range");
                        ImGui::DragFloat("##Range", &light->range, 0.5f, 0.0f, 1000.0f);
                    }

                    if (light->type == SparkEditor::Light::SPOT)
                    {
                        Spark::Ui::PropertyLabel("Spot Angle");
                        ImGui::DragFloat("##SpotAngle", &light->spotAngle, 1.0f, 1.0f, 179.0f);
                        Spark::Ui::PropertyLabel("Inner Angle");
                        ImGui::DragFloat("##InnerAngle", &light->spotInnerAngle, 1.0f, 1.0f, light->spotAngle);
                    }

                    Spark::Ui::PropertyLabel("Cast Shadows");
                    ImGui::Checkbox("##CastShadows", &light->castShadows);

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
                        Spark::Ui::PropertyLabel("Shadow Map Size");
                        if (ImGui::Combo("##ShadowMapSize", &currentShadowIdx, shadowSizes, IM_ARRAYSIZE(shadowSizes)))
                        {
                            light->shadowMapSize = shadowSizeValues[currentShadowIdx];
                        }
                    }
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

    // ============================================================================
    // Camera Component
    // ============================================================================

    void InspectorPanel::RenderCameraComponent()
    {
        bool headerOpen = Spark::Ui::SectionHeader("Camera", ICON_FA_CAMERA, false);

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
            ImGui::Indent(Spark::Ui::Px(4.0f));

            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::CAMERA);
            SparkEditor::Camera* cam = comp ? comp->GetData<SparkEditor::Camera>() : nullptr;

            if (cam)
            {
                if (Spark::Ui::BeginPropertyGrid("##CameraProps"))
                {
                    const char* projTypes[] = {"Perspective", "Orthographic"};
                    int projType = static_cast<int>(cam->projectionType);
                    Spark::Ui::PropertyLabel("Projection");
                    if (ImGui::Combo("##Projection", &projType, projTypes, IM_ARRAYSIZE(projTypes)))
                    {
                        cam->projectionType = static_cast<SparkEditor::Camera::ProjectionType>(projType);
                    }

                    if (cam->projectionType == SparkEditor::Camera::PERSPECTIVE)
                    {
                        Spark::Ui::PropertyLabel("Field of View");
                        ImGui::DragFloat("##FieldofView", &cam->fieldOfView, 1.0f, 1.0f, 179.0f);
                    }
                    else
                    {
                        Spark::Ui::PropertyLabel("Ortho Size");
                        ImGui::DragFloat("##OrthoSize", &cam->orthographicSize, 0.1f, 0.01f, 1000.0f);
                    }

                    Spark::Ui::PropertyLabel("Near Plane");
                    ImGui::DragFloat("##NearPlane", &cam->nearPlane, 0.01f, 0.001f, cam->farPlane);
                    Spark::Ui::PropertyLabel("Far Plane");
                    ImGui::DragFloat("##FarPlane", &cam->farPlane, 1.0f, cam->nearPlane, 100000.0f);

                    float clearCol[4] = {cam->clearColor.x, cam->clearColor.y, cam->clearColor.z, cam->clearColor.w};
                    Spark::Ui::PropertyLabel("Clear Color");
                    if (ImGui::ColorEdit4("##ClearColor", clearCol))
                    {
                        cam->clearColor = {clearCol[0], clearCol[1], clearCol[2], clearCol[3]};
                    }

                    Spark::Ui::PropertyLabel("Main Camera");
                    ImGui::Checkbox("##MainCamera", &cam->isMainCamera);
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

    // ============================================================================
    // Rigid Body Component
    // ============================================================================

    void InspectorPanel::RenderRigidBodyComponent()
    {
        bool headerOpen = Spark::Ui::SectionHeader("Rigidbody", ICON_FA_GLOBE, false);

        if (ImGui::BeginPopupContextItem("##RigidBodyCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
            {
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "Inspector: removing RigidBody from object %llu",
                               static_cast<unsigned long long>(m_inspectedObjectID));
                RemoveComponent(ComponentType::RIGID_BODY);
            }
            ImGui::EndPopup();
        }

        if (headerOpen)
        {
            ImGui::Indent(Spark::Ui::Px(4.0f));

            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::RIGID_BODY);
            RigidBody* rb = comp ? comp->GetData<RigidBody>() : nullptr;

            if (rb)
            {
                if (Spark::Ui::BeginPropertyGrid("##RigidBodyProps"))
                {
                    const char* bodyTypes[] = {"Static", "Kinematic", "Dynamic"};
                    int bodyType = static_cast<int>(rb->bodyType);
                    Spark::Ui::PropertyLabel("Body Type");
                    if (ImGui::Combo("##BodyType", &bodyType, bodyTypes, IM_ARRAYSIZE(bodyTypes)))
                    {
                        SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Inspector: rigid body type changed to '%s'",
                                        bodyTypes[bodyType]);
                        rb->bodyType = static_cast<RigidBody::BodyType>(bodyType);
                    }

                    if (rb->bodyType == RigidBody::DYNAMIC)
                    {
                        Spark::Ui::PropertyLabel("Mass");
                        ImGui::DragFloat("##Mass", &rb->mass, 0.1f, 0.01f, 10000.0f);
                    }

                    Spark::Ui::PropertyLabel("Drag");
                    ImGui::DragFloat("##Drag", &rb->drag, 0.01f, 0.0f, 100.0f);
                    Spark::Ui::PropertyLabel("Angular Drag");
                    ImGui::DragFloat("##AngularDrag", &rb->angularDrag, 0.01f, 0.0f, 100.0f);
                    Spark::Ui::PropertyLabel("Use Gravity");
                    ImGui::Checkbox("##UseGravity", &rb->useGravity);
                    Spark::Ui::PropertyLabel("Is Kinematic");
                    ImGui::Checkbox("##IsKinematic", &rb->isKinematic);

                    // Freeze flags: one row per kind; the axis letters label the checkboxes.
                    Spark::Ui::PropertyLabel("Freeze Position");
                    ImGui::Checkbox("X##FPX", &rb->freezePositionX);
                    ImGui::SameLine();
                    ImGui::Checkbox("Y##FPY", &rb->freezePositionY);
                    ImGui::SameLine();
                    ImGui::Checkbox("Z##FPZ", &rb->freezePositionZ);

                    Spark::Ui::PropertyLabel("Freeze Rotation");
                    ImGui::Checkbox("X##FRX", &rb->freezeRotationX);
                    ImGui::SameLine();
                    ImGui::Checkbox("Y##FRY", &rb->freezeRotationY);
                    ImGui::SameLine();
                    ImGui::Checkbox("Z##FRZ", &rb->freezeRotationZ);
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

    // ============================================================================
    // Collider Component
    // ============================================================================

    void InspectorPanel::RenderColliderComponent()
    {
        bool headerOpen = Spark::Ui::SectionHeader("Collider", ICON_FA_VECTOR_SQUARE, false);

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
            ImGui::Indent(Spark::Ui::Px(4.0f));

            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::COLLIDER);
            SparkEditor::Collider* col = comp ? comp->GetData<SparkEditor::Collider>() : nullptr;

            if (col)
            {
                if (Spark::Ui::BeginPropertyGrid("##ColliderProps"))
                {
                    const char* colliderTypes[] = {"Box", "Sphere", "Capsule", "Mesh", "Terrain"};
                    int colType = static_cast<int>(col->type);
                    Spark::Ui::PropertyLabel("Shape");
                    if (ImGui::Combo("##Shape", &colType, colliderTypes, IM_ARRAYSIZE(colliderTypes)))
                    {
                        col->type = static_cast<SparkEditor::Collider::ColliderType>(colType);
                    }

                    Spark::Ui::PropertyLabel("Is Trigger");
                    ImGui::Checkbox("##IsTrigger", &col->isTrigger);

                    float center[3] = {col->center.x, col->center.y, col->center.z};
                    Spark::Ui::PropertyLabel("Center");
                    DrawVec3Control("##Center", center, 0.0f, 0.1f);
                    if (center[0] != col->center.x || center[1] != col->center.y || center[2] != col->center.z)
                    {
                        col->center = {center[0], center[1], center[2]};
                    }

                    switch (col->type)
                    {
                    case SparkEditor::Collider::BOX:
                    {
                        float size[3] = {col->size.x, col->size.y, col->size.z};
                        Spark::Ui::PropertyLabel("Size");
                        DrawVec3Control("##Size", size, 1.0f, 0.1f);
                        if (size[0] != col->size.x || size[1] != col->size.y || size[2] != col->size.z)
                        {
                            // An edited box extent stays positive, as the old drag range enforced.
                            col->size = {(std::max)(size[0], 0.001f), (std::max)(size[1], 0.001f),
                                         (std::max)(size[2], 0.001f)};
                        }
                        break;
                    }
                    case SparkEditor::Collider::SPHERE:
                    {
                        Spark::Ui::PropertyLabel("Radius");
                        ImGui::DragFloat("##Radius", &col->radius, 0.01f, 0.001f, 1000.0f);
                        break;
                    }
                    case SparkEditor::Collider::CAPSULE:
                    {
                        Spark::Ui::PropertyLabel("Radius");
                        ImGui::DragFloat("##Radius", &col->radius, 0.01f, 0.001f, 1000.0f);
                        Spark::Ui::PropertyLabel("Height");
                        ImGui::DragFloat("##Height", &col->height, 0.1f, 0.001f, 1000.0f);
                        break;
                    }
                    case SparkEditor::Collider::MESH:
                    {
                        char meshBuf[256];
                        strncpy(meshBuf, col->meshAssetPath.c_str(), sizeof(meshBuf) - 1);
                        meshBuf[sizeof(meshBuf) - 1] = '\0';
                        Spark::Ui::PropertyLabel("Mesh");
                        if (ImGui::InputText("##ColMesh", meshBuf, sizeof(meshBuf),
                                             ImGuiInputTextFlags_EnterReturnsTrue))
                        {
                            col->meshAssetPath = meshBuf;
                        }
                        break;
                    }
                    default:
                        break;
                    }

                    Spark::Ui::PropertyLabel("Friction");
                    ImGui::DragFloat("##Friction", &col->friction, 0.01f, 0.0f, 1.0f);
                    Spark::Ui::PropertyLabel("Bounciness");
                    ImGui::DragFloat("##Bounciness", &col->bounciness, 0.01f, 0.0f, 1.0f);
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

} // namespace SparkEditor
