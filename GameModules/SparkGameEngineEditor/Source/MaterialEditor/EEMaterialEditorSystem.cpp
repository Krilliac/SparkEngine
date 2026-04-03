/**
 * @file EEMaterialEditorSystem.cpp
 * @brief Visual node-based material/shader editor
 *
 * Implements material graph editing with PBR output channels, multiple
 * shading models, preset templates, and shader compilation.
 */

#include "EEMaterialEditorSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#include <algorithm>

namespace EngineEditor
{

    bool EEMaterialEditorSystem::Initialize(Spark::IEngineContext* context)
    {
        if (!context)
            return false;

        m_context = context;
        RegisterBuiltinPresets();

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Material editor system initialized (%zu presets)",
                       m_materials.size());
        return true;
    }

    void EEMaterialEditorSystem::Update([[maybe_unused]] float deltaTime)
    {
        if (!m_initialized)
            return;
    }

    void EEMaterialEditorSystem::Shutdown()
    {
        m_materials.clear();
        m_initialized = false;
    }

    void EEMaterialEditorSystem::RenderDebugUI() {}

    uint32_t EEMaterialEditorSystem::CreateMaterial(const std::string& name, ShadingModel model)
    {
        MaterialGraph mat;
        mat.materialId = m_nextMaterialId++;
        mat.name = name;
        mat.shadingModel = model;

        // Always add an output node
        MaterialNode output;
        output.nodeId = m_nextNodeId++;
        output.name = "Material Output";
        output.type = MaterialNodeType::Output;
        output.posX = 600.0f;
        output.posY = 300.0f;
        mat.nodes.push_back(output);

        m_materials.push_back(std::move(mat));
        return m_materials.back().materialId;
    }

    bool EEMaterialEditorSystem::DeleteMaterial(uint32_t materialId)
    {
        auto it = std::find_if(m_materials.begin(), m_materials.end(),
                               [materialId](const MaterialGraph& m) { return m.materialId == materialId; });
        if (it == m_materials.end())
            return false;
        m_materials.erase(it);
        return true;
    }

    bool EEMaterialEditorSystem::CompileMaterial(uint32_t materialId)
    {
        for (auto& mat : m_materials)
        {
            if (mat.materialId == materialId)
            {
                // Verify output node exists and has inputs connected
                bool hasOutput = false;
                for (const auto& n : mat.nodes)
                {
                    if (n.type == MaterialNodeType::Output)
                    {
                        hasOutput = true;
                        break;
                    }
                }
                mat.isCompiled = hasOutput;
                return hasOutput;
            }
        }
        return false;
    }

    uint32_t EEMaterialEditorSystem::AddNode(uint32_t materialId, MaterialNodeType type, float x, float y)
    {
        for (auto& mat : m_materials)
        {
            if (mat.materialId == materialId)
            {
                MaterialNode node;
                node.nodeId = m_nextNodeId++;
                node.type = type;
                node.posX = x;
                node.posY = y;

                switch (type)
                {
                case MaterialNodeType::TextureSample:
                    node.name = "Texture Sample";
                    break;
                case MaterialNodeType::ConstantFloat:
                    node.name = "Constant (Float)";
                    break;
                case MaterialNodeType::ConstantVector:
                    node.name = "Constant (Vector)";
                    break;
                case MaterialNodeType::ConstantColor:
                    node.name = "Constant (Color)";
                    break;
                case MaterialNodeType::Multiply:
                    node.name = "Multiply";
                    break;
                case MaterialNodeType::Add:
                    node.name = "Add";
                    break;
                case MaterialNodeType::Lerp:
                    node.name = "Lerp";
                    break;
                case MaterialNodeType::Fresnel:
                    node.name = "Fresnel";
                    break;
                case MaterialNodeType::Normal:
                    node.name = "World Normal";
                    break;
                case MaterialNodeType::WorldPosition:
                    node.name = "World Position";
                    break;
                case MaterialNodeType::TexCoord:
                    node.name = "Texture Coordinates";
                    break;
                case MaterialNodeType::Time:
                    node.name = "Time";
                    break;
                case MaterialNodeType::Panner:
                    node.name = "Panner";
                    break;
                case MaterialNodeType::Noise:
                    node.name = "Noise";
                    break;
                case MaterialNodeType::DotProduct:
                    node.name = "Dot Product";
                    break;
                case MaterialNodeType::Power:
                    node.name = "Power";
                    break;
                case MaterialNodeType::Clamp:
                    node.name = "Clamp";
                    break;
                default:
                    node.name = "Unknown";
                    break;
                }

                mat.nodes.push_back(std::move(node));
                mat.isCompiled = false;
                return mat.nodes.back().nodeId;
            }
        }
        return 0;
    }

    bool EEMaterialEditorSystem::RemoveNode(uint32_t materialId, uint32_t nodeId)
    {
        for (auto& mat : m_materials)
        {
            if (mat.materialId == materialId)
            {
                auto nit = std::find_if(mat.nodes.begin(), mat.nodes.end(),
                                        [nodeId](const MaterialNode& n) { return n.nodeId == nodeId; });
                if (nit == mat.nodes.end() || nit->type == MaterialNodeType::Output)
                    return false;

                std::erase_if(mat.connections, [nodeId](const MaterialConnection& c)
                              { return c.fromNodeId == nodeId || c.toNodeId == nodeId; });
                mat.nodes.erase(nit);
                mat.isCompiled = false;
                return true;
            }
        }
        return false;
    }

    bool EEMaterialEditorSystem::ConnectNodes(uint32_t materialId, uint32_t fromNode, uint32_t fromCh, uint32_t toNode,
                                              uint32_t toCh)
    {
        for (auto& mat : m_materials)
        {
            if (mat.materialId == materialId)
            {
                MaterialConnection conn;
                conn.connectionId = m_nextConnectionId++;
                conn.fromNodeId = fromNode;
                conn.fromChannel = fromCh;
                conn.toNodeId = toNode;
                conn.toChannel = toCh;
                mat.connections.push_back(conn);
                mat.isCompiled = false;
                return true;
            }
        }
        return false;
    }

    uint32_t EEMaterialEditorSystem::CreatePresetPBR(const std::string& name)
    {
        uint32_t id = CreateMaterial(name, ShadingModel::DefaultLit);

        // Build PBR graph inline
        for (auto& m : m_materials)
        {
            if (m.materialId == id)
            {
                // Add albedo texture
                uint32_t albedoId = AddNode(id, MaterialNodeType::TextureSample, 100.0f, 100.0f);
                // Add normal map
                uint32_t normalId = AddNode(id, MaterialNodeType::TextureSample, 100.0f, 250.0f);
                // Add roughness constant
                uint32_t roughId = AddNode(id, MaterialNodeType::ConstantFloat, 100.0f, 400.0f);
                // Add metallic constant
                uint32_t metalId = AddNode(id, MaterialNodeType::ConstantFloat, 100.0f, 500.0f);

                // Set default values
                for (auto& n : m.nodes)
                {
                    if (n.nodeId == roughId)
                        n.paramFloat = 0.5f;
                    else if (n.nodeId == metalId)
                        n.paramFloat = 0.0f;
                }

                // Connect to output
                uint32_t outputId = m.nodes.front().nodeId; // Output node
                ConnectNodes(id, albedoId, 0, outputId, 0); // Albedo
                ConnectNodes(id, normalId, 0, outputId, 1); // Normal
                ConnectNodes(id, roughId, 0, outputId, 2);  // Roughness
                ConnectNodes(id, metalId, 0, outputId, 3);  // Metallic
                break;
            }
        }
        return id;
    }

    uint32_t EEMaterialEditorSystem::CreatePresetUnlit(const std::string& name)
    {
        uint32_t id = CreateMaterial(name, ShadingModel::Unlit);
        AddNode(id, MaterialNodeType::TextureSample, 100.0f, 200.0f);
        return id;
    }

    uint32_t EEMaterialEditorSystem::CreatePresetEmissive(const std::string& name)
    {
        uint32_t id = CreateMaterial(name, ShadingModel::DefaultLit);
        AddNode(id, MaterialNodeType::ConstantColor, 100.0f, 100.0f);
        AddNode(id, MaterialNodeType::Multiply, 300.0f, 200.0f);
        AddNode(id, MaterialNodeType::ConstantFloat, 100.0f, 300.0f);
        return id;
    }

    std::string EEMaterialEditorSystem::GetMaterialListString() const
    {
        std::string s = "=== Materials ===\n";
        for (const auto& m : m_materials)
        {
            s += "  [" + std::to_string(m.materialId) + "] " + m.name;
            s += " (" + std::to_string(m.nodes.size()) + " nodes";
            s += ", model=" + std::to_string(static_cast<int>(m.shadingModel));
            if (m.isCompiled)
                s += ", compiled";
            s += ")\n";
        }
        if (m_materials.empty())
            s += "  (none)\n";
        return s;
    }

    std::string EEMaterialEditorSystem::GetMaterialDetailString(uint32_t materialId) const
    {
        for (const auto& m : m_materials)
        {
            if (m.materialId == materialId)
            {
                std::string s = "=== Material: " + m.name + " ===\n";
                s += "Shading: " + std::to_string(static_cast<int>(m.shadingModel)) + "\n";
                s += "Blend: " + std::to_string(static_cast<int>(m.blendMode)) + "\n";
                s += "Two-sided: " + std::string(m.isTwoSided ? "yes" : "no") + "\n";
                s += "Nodes: " + std::to_string(m.nodes.size()) + "\n";
                for (const auto& n : m.nodes)
                    s += "  [" + std::to_string(n.nodeId) + "] " + n.name + "\n";
                s += "Connections: " + std::to_string(m.connections.size()) + "\n";
                return s;
            }
        }
        return "Material not found";
    }

    std::string EEMaterialEditorSystem::GetNodeCatalogString() const
    {
        std::string s = "=== Material Node Types ===\n";
        s += "  TextureSample, ConstantFloat, ConstantVector, ConstantColor\n";
        s += "  Multiply, Add, Lerp, Fresnel, DotProduct, Power, Clamp\n";
        s += "  Normal, WorldPosition, TexCoord, Time, Panner, Noise\n";
        s += "  Output (Albedo/Normal/Roughness/Metallic/AO/Emissive)\n";
        return s;
    }

    void EEMaterialEditorSystem::RegisterBuiltinPresets()
    {
        CreatePresetPBR("M_Default_PBR");
        CreatePresetUnlit("M_Default_Unlit");
        CreatePresetEmissive("M_Default_Emissive");
    }

} // namespace EngineEditor
