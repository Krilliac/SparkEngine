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
#include "SparkUI/Runtime.h"
#include "SparkUI/Widgets.h"
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
        bool headerOpen = Spark::Ui::SectionHeader("Audio Source", ICON_FA_VOLUME_UP, false);

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
            ImGui::Indent(Spark::Ui::Px(4.0f));

            Component* comp = FindComponent(m_scene, m_inspectedObjectID, ComponentType::AUDIO_SOURCE);
            AudioSource* audio = comp ? comp->GetData<AudioSource>() : nullptr;

            if (audio)
            {
                if (Spark::Ui::BeginPropertyGrid("##AudioSourceProps"))
                {
                    char clipBuf[256];
                    strncpy(clipBuf, audio->audioClipPath.c_str(), sizeof(clipBuf) - 1);
                    clipBuf[sizeof(clipBuf) - 1] = '\0';
                    Spark::Ui::PropertyLabel("Audio Clip");
                    if (ImGui::InputText("##AudioClip", clipBuf, sizeof(clipBuf), ImGuiInputTextFlags_EnterReturnsTrue))
                    {
                        audio->audioClipPath = clipBuf;
                    }

                    Spark::Ui::PropertyLabel("Play On Awake");
                    ImGui::Checkbox("##PlayOnAwake", &audio->playOnAwake);
                    Spark::Ui::PropertyLabel("Loop");
                    ImGui::Checkbox("##Loop", &audio->loop);

                    Spark::Ui::PropertyLabel("Volume");
                    ImGui::SliderFloat("##Volume", &audio->volume, 0.0f, 1.0f);
                    Spark::Ui::PropertyLabel("Pitch");
                    ImGui::DragFloat("##Pitch", &audio->pitch, 0.01f, 0.01f, 3.0f);

                    Spark::Ui::PropertyLabel("Spatial Blend");
                    ImGui::SliderFloat("##SpatialBlend", &audio->spatialBlend, 0.0f, 1.0f, "%.2f (2D-3D)");

                    if (audio->spatialBlend > 0.0f)
                    {
                        Spark::Ui::PropertyLabel("Min Distance");
                        ImGui::DragFloat("##MinDistance", &audio->minDistance, 0.1f, 0.0f, audio->maxDistance);
                        Spark::Ui::PropertyLabel("Max Distance");
                        ImGui::DragFloat("##MaxDistance", &audio->maxDistance, 1.0f, audio->minDistance, 10000.0f);
                    }

                    Spark::Ui::PropertyLabel("Priority");
                    ImGui::SliderInt("##Priority", &audio->priority, 0, 255);
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

    // Terrain — migrated to InspectorComponentRenderers_Reflected.cpp (reflection with categories)

    // SpriteRenderer, SpriteAnimator, Camera2D, Tilemap, NineSlice, ParallaxBG,
    // PixelPerfect, Terrain — migrated to InspectorComponentRenderers_Reflected.cpp

    // RigidBody2D, Collider2D — migrated to InspectorComponentRenderers_Reflected.cpp
    // (uses conditional field visibility for shape/body-type dependent fields)

} // namespace SparkEditor
