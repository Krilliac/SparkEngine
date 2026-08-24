/**
 * @file BasicMaterialEditorPanel.h
 * @brief Editor for the basic-path material JSONs under Assets/Materials
 * @author Spark Engine Team
 * @date 2026
 *
 * Edits the small material JSON files consumed by GraphicsEngine::GetOrLoadBasicMaterial
 * (schema: name, shader:PBR, albedo path, normal path, roughness path-or-scalar,
 * metallic scalar, ao scalar, optional tiling[2]). Distinct from MaterialEditorPanel,
 * which edits the legacy .spkmat property/graph materials.
 *
 * Features:
 * - Recursive browse of Assets/Materials/**.json grouped by folder
 * - Field editing with texture-path existence validation
 * - Texture thumbnails + tiled albedo preview via the editor's attach-mode GraphicsEngine
 * - Save writes pretty JSON back with a stable field order; Revert reloads from disk
 * - Apply-live: save invalidates the GraphicsEngine basic-material cache so the
 *   Scene View picks up the change on its next draw
 */

#pragma once

#include "../Core/EditorPanel.h"

#include <filesystem>
#include <string>
#include <vector>

class GraphicsEngine;

namespace SparkEditor
{

    /**
     * @brief Editor panel for basic-path material JSON files.
     *
     * Non-owning GraphicsEngine pointer is wired by EditorUI::SetGraphicsDevice()
     * (same pattern as SceneViewPanel::SetGraphics). The panel degrades gracefully
     * without it: no thumbnails, no live cache invalidation, editing still works.
     */
    class BasicMaterialEditorPanel : public EditorPanel
    {
      public:
        BasicMaterialEditorPanel();
        ~BasicMaterialEditorPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

        std::string GetTypeName() const override { return "BasicMaterialEditorPanel"; }

        /// @brief Give the panel access to the editor's attach-mode GraphicsEngine
        ///        (thumbnails + save-time material-cache invalidation). Non-owning.
        void SetGraphics(GraphicsEngine* graphics) { m_graphics = graphics; }

      private:
        /// @brief One material JSON document loaded into editable fields.
        struct MaterialDoc
        {
            std::filesystem::path diskPath; ///< Native filesystem path (never round-tripped through ACP)
            std::string enginePath;         ///< Canonical "Assets/Materials/..." cache key (forward slashes)
            std::string group;              ///< Folder relative to Assets/Materials ("" = root)
            std::string fileName;           ///< File name without directory (e.g. "Metal.json")

            char name[128] = {};
            char shader[32] = {};
            char albedoPath[260] = {};
            char normalPath[260] = {};
            char roughnessPath[260] = {};
            bool roughnessIsTexture = false;
            float roughness = 0.5f;
            float metallic = 0.0f;
            float ao = 1.0f;
            float tiling[2] = {1.0f, 1.0f};
            bool hasTiling = false; ///< Whether the file had (or the user enabled) a tiling key

            /// Unknown top-level keys preserved verbatim ("\"emissive\": [0.02, 0.0, 0.04]")
            /// so Save never drops fields this panel doesn't edit.
            std::vector<std::string> extraFields;

            bool loaded = false;
            bool modified = false;
            std::string loadError;
        };

        void ScanMaterials();
        bool LoadDoc(MaterialDoc& doc);
        bool SaveDoc(MaterialDoc& doc);
        bool SaveAllModified();

        void RenderToolbar();
        void RenderMaterialList();
        void RenderMaterialProperties(MaterialDoc& doc);
        bool RenderTextureField(const char* label, char* buf, size_t bufSize);
        void RenderThumbnail(const char* label, const char* assetRelPath, float tilingX, float tilingY);

        /// @brief Map an asset-relative texture path ("Textures/x.png" or "Assets/Textures/x.png")
        ///        to a filesystem path openable from the editor's cwd.
        std::filesystem::path ResolveAssetDiskPath(const std::string& assetRelPath) const;

        /// @brief True if the texture path in @p buf resolves to an existing file (empty counts as valid).
        bool TexturePathExists(const char* buf) const;

        std::vector<MaterialDoc> m_materials;
        int m_selected = -1;
        char m_searchFilter[128] = {};

        /// Canonical active project root; empty while no project is open.
        std::string m_projectRoot;

        GraphicsEngine* m_graphics = nullptr; ///< Non-owning; owned by EditorUI
        float m_thumbSize = 96.0f;
    };

} // namespace SparkEditor
