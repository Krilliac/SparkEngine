/**
 * @file InspectorComponentRenderers_2D.cpp
 * @brief Inspector renderers for 2D components with complex conditional UI
 *
 * Contains: AudioSource, RigidBody2D, Collider2D.
 * Simpler components migrated to InspectorComponentRenderers_Reflected.cpp:
 * SpriteRenderer, SpriteAnimator, Camera2D, Tilemap, NineSlice, ParallaxBG,
 * PixelPerfect, Terrain.
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

    // Terrain — migrated to InspectorComponentRenderers_Reflected.cpp (reflection with categories)

    // SpriteRenderer, SpriteAnimator, Camera2D, Tilemap, NineSlice, ParallaxBG,
    // PixelPerfect, Terrain — migrated to InspectorComponentRenderers_Reflected.cpp

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

} // namespace SparkEditor
