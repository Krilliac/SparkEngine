/**
 * @file InspectorComponentRenderers_Reflected2D.cpp
 * @brief Reflection-driven inspector renderers for 2D components
 *
 * Split from InspectorComponentRenderers_Reflected.cpp. Contains the
 * renderers migrated from InspectorComponentRenderers_2D.cpp:
 * PixelPerfect, SpriteAnimator, Terrain, SpriteRenderer, Camera2D,
 * Tilemap, NineSlice, ParallaxBG.
 */

#include "InspectorComponentRenderers_ReflectedInternal.h"

namespace SparkEditor
{

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
