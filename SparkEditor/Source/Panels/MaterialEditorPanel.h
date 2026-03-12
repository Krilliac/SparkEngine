/**
 * @file MaterialEditorPanel.h
 * @brief Visual material and shader property editor
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a node-graph-style material editor for creating and editing PBR
 * materials. Supports editing shader parameters, texture slot assignment,
 * render-state configuration, and live preview with the scene's lighting.
 */

#pragma once

#include "../Core/EditorPanel.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace SparkEditor
{

    // ========================================================================
    // Material Data Structures
    // ========================================================================

    /// @brief Shader parameter types supported in the material editor.
    enum class ShaderParamType
    {
        Float,
        Float2,
        Float3,
        Float4,
        Int,
        Bool,
        Texture2D,
        TextureCube,
        Color,
        Matrix4x4
    };

    /// @brief A single editable shader parameter.
    struct ShaderParameter
    {
        std::string name;
        std::string displayName;
        ShaderParamType type = ShaderParamType::Float;
        std::string group; ///< UI grouping (e.g., "Surface", "Normal", "Emission")

        // Values stored as floats for numeric types
        float floatValues[16] = {};
        int intValue = 0;
        bool boolValue = false;
        std::string texturePath;

        // UI hints
        float minValue = 0.0f;
        float maxValue = 1.0f;
        float step = 0.01f;
        std::string tooltip;
        bool isHDR = false; ///< For color parameters
    };

    /// @brief Texture slot in a material.
    struct TextureSlot
    {
        std::string name; ///< Slot name (e.g., "Albedo", "Normal", "Roughness")
        std::string texturePath;
        int bindSlot = 0; ///< Shader register slot
        bool isAssigned = false;

        // Sampling options
        enum class FilterMode
        {
            Point,
            Bilinear,
            Trilinear,
            Anisotropic
        };
        FilterMode filter = FilterMode::Trilinear;

        enum class WrapMode
        {
            Repeat,
            Clamp,
            Mirror,
            Border
        };
        WrapMode wrapU = WrapMode::Repeat;
        WrapMode wrapV = WrapMode::Repeat;

        float tilingU = 1.0f;
        float tilingV = 1.0f;
        float offsetU = 0.0f;
        float offsetV = 0.0f;
    };

    /// @brief Render state configuration for a material.
    struct MaterialRenderState
    {
        enum class BlendMode
        {
            Opaque,
            AlphaBlend,
            Additive,
            Multiply,
            Custom
        };
        BlendMode blendMode = BlendMode::Opaque;

        enum class CullMode
        {
            Back,
            Front,
            None
        };
        CullMode cullMode = CullMode::Back;

        bool depthWrite = true;
        bool depthTest = true;
        bool castShadows = true;
        bool receiveShadows = true;
        float alphaClipThreshold = 0.5f;
        int renderQueue = 2000; ///< Sorting order
    };

    /// @brief Complete material definition.
    struct MaterialDefinition
    {
        std::string name;
        std::string shaderPath;
        std::string filePath; ///< Asset path on disk

        std::vector<ShaderParameter> parameters;
        std::vector<TextureSlot> textureSlots;
        MaterialRenderState renderState;

        bool isModified = false;
        bool isBuiltIn = false; ///< Engine default material (read-only)

        /// @brief Find a parameter by name.
        ShaderParameter* FindParameter(const std::string& paramName)
        {
            for (auto& p : parameters)
            {
                if (p.name == paramName)
                    return &p;
            }
            return nullptr;
        }

        /// @brief Find a texture slot by name.
        TextureSlot* FindTextureSlot(const std::string& slotName)
        {
            for (auto& s : textureSlots)
            {
                if (s.name == slotName)
                    return &s;
            }
            return nullptr;
        }
    };

    // ========================================================================
    // Material Editor Panel
    // ========================================================================

    /**
     * @brief Visual material editor panel
     *
     * Features:
     * - Material list with search and filtering
     * - PBR property editing (albedo, metallic, roughness, normal, emission, AO)
     * - Texture slot assignment with drag-and-drop from asset browser
     * - Tiling, offset, and sampling controls per texture
     * - Render state editing (blend mode, cull mode, depth, shadows)
     * - Shader selection and parameter auto-discovery
     * - Live preview sphere/cube with scene lighting
     * - Material comparison (side-by-side)
     * - Material instancing (create variants from a base)
     * - Undo/redo support via command pattern
     * - Save/load materials as .spkmat files
     */
    class MaterialEditorPanel : public EditorPanel
    {
      public:
        MaterialEditorPanel();
        ~MaterialEditorPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;
        bool HandleEvent(const std::string& eventType, void* eventData) override;

        /// @brief Open a material for editing by path.
        void OpenMaterial(const std::string& materialPath);

        /// @brief Create a new material from a shader.
        void CreateMaterial(const std::string& name, const std::string& shaderPath);

        /// @brief Save the current material.
        bool SaveMaterial();

        /// @brief Check if the material has unsaved changes.
        bool HasUnsavedChanges() const;

      private:
        void RenderMaterialList();
        void RenderShaderSelector();
        void RenderParameterEditor();
        void RenderParameterGroup(const std::string& groupName, std::vector<ShaderParameter*>& params);
        void RenderFloatParam(ShaderParameter& param);
        void RenderVectorParam(ShaderParameter& param);
        void RenderColorParam(ShaderParameter& param);
        void RenderBoolParam(ShaderParameter& param);
        void RenderTextureParam(ShaderParameter& param);
        void RenderTextureSlots();
        void RenderTextureSlotEditor(TextureSlot& slot);
        void RenderRenderStateEditor();
        void RenderPreview();
        void RenderToolbar();
        void LoadDefaultMaterials();
        void PopulateDefaultPBRParameters(MaterialDefinition& material);

        // Materials
        std::vector<MaterialDefinition> m_materials;
        int m_selectedMaterialIndex = -1;
        MaterialDefinition* GetSelectedMaterial();

        // Available shaders
        struct ShaderInfo
        {
            std::string name;
            std::string path;
            std::string description;
        };
        std::vector<ShaderInfo> m_availableShaders;

        // UI state
        std::string m_searchFilter;
        bool m_showPreview = true;
        bool m_showRenderState = false;
        float m_previewTime = 0.0f;

        enum class PreviewShape
        {
            Sphere,
            Cube,
            Plane,
            Cylinder,
            Custom
        };
        PreviewShape m_previewShape = PreviewShape::Sphere;

        enum class PreviewLighting
        {
            Default,
            SceneLights,
            IBLOnly,
            Unlit
        };
        PreviewLighting m_previewLighting = PreviewLighting::Default;

        bool m_previewRotate = true;
        float m_previewRotationSpeed = 30.0f; ///< Degrees per second
    };

} // namespace SparkEditor
