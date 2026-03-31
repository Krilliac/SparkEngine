/**
 * @file InspectorComponentRenderers_Core3D.cpp
 * @brief Inspector renderers for core 3D components: Transform, MeshRenderer, Light, Camera, RigidBody, Collider
 *
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
#include <cstring>

namespace SparkEditor
{

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

                DrawVec3Control("Position", position, 0.0f, 0.1f);
                DrawVec3Control("Rotation", rotation, 0.0f, 1.0f);
                DrawVec3Control("Scale", scale, 1.0f, 0.1f);

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
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "Inspector: removing MeshRenderer from object %llu",
                               static_cast<unsigned long long>(m_inspectedObjectID));
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
                    SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Inspector: mesh asset changed to '%s'", meshBuf);
                    mr->meshAssetPath = meshBuf;
                }

                // Material path
                char matBuf[256];
                strncpy(matBuf, mr->materialAssetPath.c_str(), sizeof(matBuf) - 1);
                matBuf[sizeof(matBuf) - 1] = '\0';
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputText("Material", matBuf, sizeof(matBuf), ImGuiInputTextFlags_EnterReturnsTrue))
                {
                    SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Inspector: material asset changed to '%s'", matBuf);
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
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "Inspector: removing Light from object %llu",
                               static_cast<unsigned long long>(m_inspectedObjectID));
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
                    SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Inspector: light type changed to '%s'",
                                    lightTypes[lightType]);
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
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "Inspector: removing RigidBody from object %llu",
                               static_cast<unsigned long long>(m_inspectedObjectID));
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
                    SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Inspector: rigid body type changed to '%s'",
                                    bodyTypes[bodyType]);
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

} // namespace SparkEditor
