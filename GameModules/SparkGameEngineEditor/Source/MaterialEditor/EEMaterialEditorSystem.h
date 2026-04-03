/**
 * @file EEMaterialEditorSystem.h
 * @brief Visual node-based material/shader editor
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a visual material graph editor with texture sampling, math
 * operations, UV manipulation, and output to PBR material channels
 * (albedo, normal, roughness, metallic, AO, emissive).
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/EngineEditorEnums.h"

#include <cstdint>
#include <string>
#include <vector>

namespace EngineEditor
{

    /// @brief A node in the material graph
    struct MaterialNode
    {
        uint32_t nodeId = 0;
        std::string name;
        MaterialNodeType type = MaterialNodeType::ConstantFloat;
        float posX = 0.0f;
        float posY = 0.0f;
        float paramFloat = 0.0f;
        float paramR = 1.0f, paramG = 1.0f, paramB = 1.0f, paramA = 1.0f;
        std::string texturePath;
    };

    /// @brief A connection in the material graph
    struct MaterialConnection
    {
        uint32_t connectionId = 0;
        uint32_t fromNodeId = 0;
        uint32_t fromChannel = 0; ///< Output channel index
        uint32_t toNodeId = 0;
        uint32_t toChannel = 0; ///< Input channel index
    };

    /// @brief A complete material definition
    struct MaterialGraph
    {
        uint32_t materialId = 0;
        std::string name;
        ShadingModel shadingModel = ShadingModel::DefaultLit;
        MaterialBlendMode blendMode = MaterialBlendMode::Opaque;
        bool isTwoSided = false;
        bool isWireframe = false;
        std::vector<MaterialNode> nodes;
        std::vector<MaterialConnection> connections;
        bool isCompiled = false;
    };

    /**
     * @brief Visual material editor for no-code shader authoring
     *
     * Manages material graphs with node-based editing, PBR output channels,
     * multiple shading models, and shader compilation.
     */
    class EEMaterialEditorSystem
    {
      public:
        EEMaterialEditorSystem() = default;
        ~EEMaterialEditorSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime);
        void Shutdown();
        void RenderDebugUI();

        // Material management
        uint32_t CreateMaterial(const std::string& name, ShadingModel model);
        bool DeleteMaterial(uint32_t materialId);
        bool CompileMaterial(uint32_t materialId);

        // Node operations
        uint32_t AddNode(uint32_t materialId, MaterialNodeType type, float x, float y);
        bool RemoveNode(uint32_t materialId, uint32_t nodeId);
        bool ConnectNodes(uint32_t materialId, uint32_t fromNode, uint32_t fromCh, uint32_t toNode, uint32_t toCh);

        // Presets
        uint32_t CreatePresetPBR(const std::string& name);
        uint32_t CreatePresetUnlit(const std::string& name);
        uint32_t CreatePresetEmissive(const std::string& name);

        // Queries
        size_t GetMaterialCount() const { return m_materials.size(); }
        std::string GetMaterialListString() const;
        std::string GetMaterialDetailString(uint32_t materialId) const;
        std::string GetNodeCatalogString() const;

      private:
        void RegisterBuiltinPresets();

        Spark::IEngineContext* m_context{nullptr};
        std::vector<MaterialGraph> m_materials;
        uint32_t m_nextMaterialId{1};
        uint32_t m_nextNodeId{1};
        uint32_t m_nextConnectionId{1};
        bool m_initialized{false};
    };

} // namespace EngineEditor
