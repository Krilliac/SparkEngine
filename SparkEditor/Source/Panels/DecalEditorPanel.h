/**
 * @file DecalEditorPanel.h
 * @brief Decal material and surface mapping editor panel
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <string>
#include <vector>

namespace SparkEditor
{

    /**
     * @brief Panel for authoring decal materials and surface-to-decal mappings
     *
     * Exposes the DecalSystem's material library and surface mapping table.
     * Allows editing decal textures, opacity, fade times, and per-surface
     * decal assignments (e.g., concrete -> bullet_hole_concrete).
     */
    class DecalEditorPanel : public EditorPanel
    {
      public:
        DecalEditorPanel();
        ~DecalEditorPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

      private:
        struct DecalMaterialEntry
        {
            char name[64] = {};
            char albedoPath[256] = {};
            char normalPath[256] = {};
            char roughnessPath[256] = {};
            float opacity = 1.0f;
            float fadeTime = 5.0f;
            bool affectsNormals = true;
            bool affectsRoughness = true;
        };

        struct SurfaceMappingEntry
        {
            int surfaceType = 0;    // index into SurfaceType enum
            int decalMaterial = -1; // index into m_materials
        };

        void RenderMaterialList();
        void RenderMaterialDetails();
        void RenderSurfaceMappings();

        std::vector<DecalMaterialEntry> m_materials;
        std::vector<SurfaceMappingEntry> m_mappings;

        int m_selectedMaterial = -1;
        int m_maxDecals = 256;
        int m_activeDecalCount = 0;
    };

} // namespace SparkEditor
