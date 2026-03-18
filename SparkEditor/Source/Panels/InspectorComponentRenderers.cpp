/**
 * @file InspectorComponentRenderers.cpp
 * @brief Per-component rendering methods for the Inspector panel
 *
 * Contains all RenderXxxComponent() methods that draw ImGui widgets
 * for each ECS component type. Split from InspectorPanel.cpp.
 */

#include "InspectorPanel.h"
#include "../Core/EditorIcons.h"
#include "../Core/EditorFonts.h"
#include "../CommandHistory.h"
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
                bool posChanged = (position[0] != oldPos[0] || position[1] != oldPos[1] || position[2] != oldPos[2]);
                bool rotChanged = (rotation[0] != oldRot[0] || rotation[1] != oldRot[1] || rotation[2] != oldRot[2]);
                bool scaleChanged = (scale[0] != oldScale[0] || scale[1] != oldScale[1] || scale[2] != oldScale[2]);

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

                    XMFLOAT4 newRot = {sx * cy * cz - cx * sy * sz, cx * sy * cz + sx * cy * sz,
                                       cx * cy * sz - sx * sy * cz, cx * cy * cz + sx * sy * sz};

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
                if (ImGui::InputText("Audio Clip", clipBuf, sizeof(clipBuf), ImGuiInputTextFlags_EnterReturnsTrue))
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
    // Terrain Component
    // ============================================================================

    void InspectorPanel::RenderTerrainComponent()
    {
        bool headerOpen = ImGui::CollapsingHeader(ICON_FA_MOUNTAIN " Terrain");

        if (ImGui::BeginPopupContextItem("##TerrainCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
            {
                RemoveComponent(ComponentType::TERRAIN);
            }
            ImGui::EndPopup();
        }

        if (headerOpen)
        {
            ImGui::Indent(4);

            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::TERRAIN);
            TerrainSceneData* terrain = comp ? comp->GetData<TerrainSceneData>() : nullptr;

            if (terrain)
            {
                ImGui::DragInt("Resolution", &terrain->heightmapResolution, 1.0f, 33, 4097);
                ImGui::DragFloat("Size", &terrain->terrainSize, 10.0f, 100.0f, 10000.0f, "%.0f m");
                ImGui::DragFloat("Height Scale", &terrain->heightScale, 0.1f, 0.1f, 100.0f);
                ImGui::DragFloat("Min Height", &terrain->minHeight, 0.5f, -1000.0f, terrain->maxHeight);
                ImGui::DragFloat("Max Height", &terrain->maxHeight, 0.5f, terrain->minHeight, 1000.0f);

                ImGui::Separator();
                ImGui::TextDisabled("LOD");
                ImGui::DragInt("LOD Levels", &terrain->lodLevels, 0.1f, 1, 8);
                ImGui::DragFloat("LOD Bias", &terrain->lodBias, 0.1f, 0.1f, 4.0f, "%.1f");

                ImGui::Separator();
                ImGui::TextDisabled("Physics");
                ImGui::Checkbox("Generate Collider", &terrain->generateCollider);
                ImGui::Checkbox("Cast Shadows", &terrain->castShadows);
                ImGui::Checkbox("Receive Shadows", &terrain->receiveShadows);
            }
            else
            {
                ImGui::TextDisabled("(Component data unavailable)");
            }

            ImGui::Unindent(4);
        }
    }

    // ============================================================================
    // Sprite Renderer Component
    // ============================================================================

    void InspectorPanel::RenderSpriteRendererComponent()
    {
        bool headerOpen = ImGui::CollapsingHeader(ICON_FA_IMAGE " Sprite Renderer");
        if (ImGui::BeginPopupContextItem("##SpriteRendererCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
                RemoveComponent(ComponentType::SPRITE_RENDERER);
            ImGui::EndPopup();
        }
        if (headerOpen)
        {
            ImGui::Indent(4);
            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::SPRITE_RENDERER);
            SpriteRendererData* sr = comp ? comp->GetData<SpriteRendererData>() : nullptr;
            if (sr)
            {
                char texBuf[256];
                strncpy(texBuf, sr->texturePath.c_str(), sizeof(texBuf) - 1);
                texBuf[sizeof(texBuf) - 1] = '\0';
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputText("Texture", texBuf, sizeof(texBuf), ImGuiInputTextFlags_EnterReturnsTrue))
                    sr->texturePath = texBuf;

                float color[4] = {sr->color.x, sr->color.y, sr->color.z, sr->color.w};
                if (ImGui::ColorEdit4("Color", color))
                    sr->color = {color[0], color[1], color[2], color[3]};

                float pivot[2] = {sr->pivot.x, sr->pivot.y};
                if (ImGui::DragFloat2("Pivot", pivot, 0.01f, 0.0f, 1.0f))
                    sr->pivot = {pivot[0], pivot[1]};

                ImGui::DragFloat("Pixels/Unit", &sr->pixelsPerUnit, 1.0f, 1.0f, 1000.0f);
                ImGui::DragInt("Sorting Layer", &sr->sortingLayer);
                ImGui::DragInt("Order in Layer", &sr->orderInLayer);
                ImGui::Checkbox("Flip X", &sr->flipX);
                ImGui::SameLine();
                ImGui::Checkbox("Flip Y", &sr->flipY);
            }
            else
            {
                ImGui::TextDisabled("(Component data unavailable)");
            }
            ImGui::Unindent(4);
        }
    }

    // ============================================================================
    // Sprite Animator Component
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
    // Camera 2D Component
    // ============================================================================

    void InspectorPanel::RenderCamera2DComponent()
    {
        bool headerOpen = ImGui::CollapsingHeader(ICON_FA_CAMERA " Camera 2D");
        if (ImGui::BeginPopupContextItem("##Camera2DCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
                RemoveComponent(ComponentType::CAMERA_2D);
            ImGui::EndPopup();
        }
        if (headerOpen)
        {
            ImGui::Indent(4);
            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::CAMERA_2D);
            Camera2DData* cam = comp ? comp->GetData<Camera2DData>() : nullptr;
            if (cam)
            {
                ImGui::DragFloat("Ortho Size", &cam->orthoSize, 0.1f, 0.1f, 100.0f);
                ImGui::DragFloat("Zoom", &cam->zoom, 0.01f, 0.1f, 10.0f);
                ImGui::DragFloat("Near Plane", &cam->nearPlane, 1.0f, -1000.0f, cam->farPlane);
                ImGui::DragFloat("Far Plane", &cam->farPlane, 1.0f, cam->nearPlane, 1000.0f);
                ImGui::DragFloat("Follow Smoothing", &cam->followSmoothing, 0.01f, 0.0f, 1.0f);

                float dz[2] = {cam->deadZone.x, cam->deadZone.y};
                if (ImGui::DragFloat2("Dead Zone", dz, 0.1f, 0.0f, 20.0f))
                    cam->deadZone = {dz[0], dz[1]};

                float cc[4] = {cam->clearColor.x, cam->clearColor.y, cam->clearColor.z, cam->clearColor.w};
                if (ImGui::ColorEdit4("Clear Color", cc))
                    cam->clearColor = {cc[0], cc[1], cc[2], cc[3]};

                ImGui::Checkbox("Main 2D Camera", &cam->isMain2DCamera);
            }
            else
            {
                ImGui::TextDisabled("(Component data unavailable)");
            }
            ImGui::Unindent(4);
        }
    }

    // ============================================================================
    // Tilemap Component
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
            TilemapData* tm = comp ? comp->GetData<TilemapData>() : nullptr;
            if (tm)
            {
                char tilesetBuf[256];
                strncpy(tilesetBuf, tm->tilesetTexturePath.c_str(), sizeof(tilesetBuf) - 1);
                tilesetBuf[sizeof(tilesetBuf) - 1] = '\0';
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputText("Tileset", tilesetBuf, sizeof(tilesetBuf), ImGuiInputTextFlags_EnterReturnsTrue))
                    tm->tilesetTexturePath = tilesetBuf;

                ImGui::DragInt("Tile Width", &tm->tileWidth, 1.0f, 1, 256);
                ImGui::DragInt("Tile Height", &tm->tileHeight, 1.0f, 1, 256);
                ImGui::DragInt("Map Width", &tm->mapWidth, 1.0f, 0, 1024);
                ImGui::DragInt("Map Height", &tm->mapHeight, 1.0f, 0, 1024);
                ImGui::DragFloat("Pixels/Unit", &tm->pixelsPerUnit, 1.0f, 1.0f, 1000.0f);
                ImGui::DragInt("Sorting Layer", &tm->sortingLayer);
                ImGui::Checkbox("Generate Collision", &tm->generateCollision);

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
    // Nine-Slice Sprite Component
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
            NineSliceData* ns = comp ? comp->GetData<NineSliceData>() : nullptr;
            if (ns)
            {
                ImGui::InputText("Texture", ns->texturePath, sizeof(ns->texturePath));

                ImGui::TextDisabled("Borders (pixels)");
                ImGui::DragFloat("Left", &ns->borderLeft, 1.0f, 0.0f, 256.0f);
                ImGui::DragFloat("Top", &ns->borderTop, 1.0f, 0.0f, 256.0f);
                ImGui::DragFloat("Right", &ns->borderRight, 1.0f, 0.0f, 256.0f);
                ImGui::DragFloat("Bottom", &ns->borderBottom, 1.0f, 0.0f, 256.0f);

                float sz[2] = {ns->size.x, ns->size.y};
                if (ImGui::DragFloat2("Size", sz, 0.1f, 0.01f))
                    ns->size = {sz[0], sz[1]};

                float color[4] = {ns->color.x, ns->color.y, ns->color.z, ns->color.w};
                if (ImGui::ColorEdit4("Color", color))
                    ns->color = {color[0], color[1], color[2], color[3]};

                ImGui::Checkbox("Fill Center", &ns->fillCenter);
                ImGui::DragInt("Sorting Layer", &ns->sortingLayer);
            }
            else
            {
                ImGui::TextDisabled("(Component data unavailable)");
            }
            ImGui::Unindent(4);
        }
    }

    // ============================================================================
    // Parallax Background Component
    // ============================================================================

    void InspectorPanel::RenderParallaxBGComponent()
    {
        bool headerOpen = ImGui::CollapsingHeader(ICON_FA_LAYER_GROUP " Parallax Background");
        if (ImGui::BeginPopupContextItem("##ParallaxBGCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
                RemoveComponent(ComponentType::PARALLAX_BG);
            ImGui::EndPopup();
        }
        if (headerOpen)
        {
            ImGui::Indent(4);
            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::PARALLAX_BG);
            ParallaxLayerData* pl = comp ? comp->GetData<ParallaxLayerData>() : nullptr;
            if (pl)
            {
                char texBuf[256];
                strncpy(texBuf, pl->texturePath.c_str(), sizeof(texBuf) - 1);
                texBuf[sizeof(texBuf) - 1] = '\0';
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputText("Texture", texBuf, sizeof(texBuf), ImGuiInputTextFlags_EnterReturnsTrue))
                    pl->texturePath = texBuf;

                float speed[2] = {pl->scrollSpeed.x, pl->scrollSpeed.y};
                if (ImGui::DragFloat2("Scroll Speed", speed, 0.01f))
                    pl->scrollSpeed = {speed[0], speed[1]};

                ImGui::Checkbox("Tile X", &pl->tileX);
                ImGui::SameLine();
                ImGui::Checkbox("Tile Y", &pl->tileY);

                float tint[4] = {pl->tint.x, pl->tint.y, pl->tint.z, pl->tint.w};
                if (ImGui::ColorEdit4("Tint", tint))
                    pl->tint = {tint[0], tint[1], tint[2], tint[3]};

                ImGui::DragInt("Sort Order", &pl->sortOrder);
            }
            else
            {
                ImGui::TextDisabled("(Component data unavailable)");
            }
            ImGui::Unindent(4);
        }
    }

    // ============================================================================
    // Pixel Perfect Component
    // ============================================================================

    void InspectorPanel::RenderPixelPerfectComponent()
    {
        bool headerOpen = ImGui::CollapsingHeader(ICON_FA_TH_LARGE " Pixel Perfect");
        if (ImGui::BeginPopupContextItem("##PixelPerfectCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
                RemoveComponent(ComponentType::PIXEL_PERFECT);
            ImGui::EndPopup();
        }
        if (headerOpen)
        {
            ImGui::Indent(4);
            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::PIXEL_PERFECT);
            PixelPerfectData* pp = comp ? comp->GetData<PixelPerfectData>() : nullptr;
            if (pp)
            {
                ImGui::DragInt("Reference Width", &pp->referenceWidth, 1.0f, 64, 3840);
                ImGui::DragInt("Reference Height", &pp->referenceHeight, 1.0f, 64, 2160);
                ImGui::Checkbox("Upscale to Fill", &pp->upscaleToFill);
                ImGui::Checkbox("Crop to Fit", &pp->cropToFit);
            }
            else
            {
                ImGui::TextDisabled("(Component data unavailable)");
            }
            ImGui::Unindent(4);
        }
    }

    // ============================================================================
    // Rigid Body 2D Component
    // ============================================================================

    void InspectorPanel::RenderRigidBody2DComponent()
    {
        bool headerOpen = ImGui::CollapsingHeader(ICON_FA_GLOBE " Rigid Body 2D");
        if (ImGui::BeginPopupContextItem("##RigidBody2DCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
                RemoveComponent(ComponentType::RIGID_BODY_2D);
            ImGui::EndPopup();
        }
        if (headerOpen)
        {
            ImGui::Indent(4);
            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::RIGID_BODY_2D);
            RigidBody2DData* rb = comp ? comp->GetData<RigidBody2DData>() : nullptr;
            if (rb)
            {
                const char* bodyTypes[] = {"Static", "Kinematic", "Dynamic"};
                ImGui::Combo("Body Type", &rb->bodyType, bodyTypes, IM_ARRAYSIZE(bodyTypes));

                if (rb->bodyType == 2) // Dynamic
                    ImGui::DragFloat("Mass", &rb->mass, 0.1f, 0.01f, 1000.0f);

                ImGui::DragFloat("Gravity Scale", &rb->gravityScale, 0.1f, -10.0f, 10.0f);
                ImGui::DragFloat("Linear Damping", &rb->linearDamping, 0.01f, 0.0f, 100.0f);
                ImGui::DragFloat("Angular Damping", &rb->angularDamping, 0.01f, 0.0f, 100.0f);
                ImGui::DragFloat("Friction", &rb->friction, 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("Restitution", &rb->restitution, 0.01f, 0.0f, 1.0f);
                ImGui::Checkbox("Fixed Rotation", &rb->fixedRotation);
                ImGui::Checkbox("Bullet (CCD)", &rb->isBullet);
            }
            else
            {
                ImGui::TextDisabled("(Component data unavailable)");
            }
            ImGui::Unindent(4);
        }
    }

    // ============================================================================
    // Collider 2D Component
    // ============================================================================

    void InspectorPanel::RenderCollider2DComponent()
    {
        bool headerOpen = ImGui::CollapsingHeader(ICON_FA_VECTOR_SQUARE " Collider 2D");
        if (ImGui::BeginPopupContextItem("##Collider2DCtx"))
        {
            if (ImGui::MenuItem(ICON_FA_TRASH " Remove Component"))
                RemoveComponent(ComponentType::COLLIDER_2D);
            ImGui::EndPopup();
        }
        if (headerOpen)
        {
            ImGui::Indent(4);
            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::COLLIDER_2D);
            Collider2DData* col = comp ? comp->GetData<Collider2DData>() : nullptr;
            if (col)
            {
                const char* shapes[] = {"Box", "Circle", "Capsule", "Polygon", "Edge"};
                ImGui::Combo("Shape", &col->shape, shapes, IM_ARRAYSIZE(shapes));

                float offset[2] = {col->offset.x, col->offset.y};
                if (ImGui::DragFloat2("Offset", offset, 0.01f))
                    col->offset = {offset[0], offset[1]};

                switch (col->shape)
                {
                case 0: // Box
                {
                    float he[2] = {col->halfExtents.x, col->halfExtents.y};
                    if (ImGui::DragFloat2("Half Extents", he, 0.01f, 0.001f))
                        col->halfExtents = {he[0], he[1]};
                    break;
                }
                case 1: // Circle
                    ImGui::DragFloat("Radius", &col->radius, 0.01f, 0.001f, 100.0f);
                    break;
                case 2: // Capsule
                    ImGui::DragFloat("Radius", &col->radius, 0.01f, 0.001f, 100.0f);
                    ImGui::DragFloat("Height", &col->height, 0.01f, 0.001f, 100.0f);
                    break;
                default:
                    break;
                }

                ImGui::Checkbox("Is Trigger", &col->isTrigger);
            }
            else
            {
                ImGui::TextDisabled("(Component data unavailable)");
            }
            ImGui::Unindent(4);
        }
    }

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
                RemoveComponent(ComponentType::SCRIPT);
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

    // ============================================================================
    // Add Component Menu
    // ============================================================================


} // namespace SparkEditor
