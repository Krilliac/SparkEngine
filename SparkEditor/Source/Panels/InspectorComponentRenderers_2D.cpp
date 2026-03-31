/**
 * @file InspectorComponentRenderers_2D.cpp
 * @brief Inspector renderers for 2D, audio, and terrain components
 *
 * Contains: AudioSource, Terrain, SpriteRenderer, SpriteAnimator, Camera2D,
 * Tilemap, NineSlice, ParallaxBG, PixelPerfect, RigidBody2D, Collider2D.
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

} // namespace SparkEditor
