/**
 * @file VisualScriptSystem.cpp
 * @brief Implementation of the node-based visual scripting system
 * @author Spark Engine Team
 * @date 2026
 */

#include "VisualScriptSystem.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <queue>
#include <set>
#include <sstream>

namespace Spark::Scripting
{

    // ============================================================================
    // ScriptNode
    // ============================================================================

    ScriptNode::ScriptNode(NodeID id, const std::string& name, NodeCategory category)
        : m_id(id)
        , m_name(name)
        , m_category(category)
    {
    }

    PinID ScriptNode::AddInputPin(const std::string& name, PinType type, const PinValue& defaultVal)
    {
        PinID pinId = m_nextPinId++;
        PinDesc desc;
        desc.id = pinId;
        desc.name = name;
        desc.type = type;
        desc.direction = PinDirection::Input;
        desc.defaultValue = defaultVal;
        m_inputPins.push_back(std::move(desc));
        return pinId;
    }

    PinID ScriptNode::AddOutputPin(const std::string& name, PinType type)
    {
        PinID pinId = m_nextPinId++;
        PinDesc desc;
        desc.id = pinId;
        desc.name = name;
        desc.type = type;
        desc.direction = PinDirection::Output;
        m_outputPins.push_back(std::move(desc));
        return pinId;
    }

    const PinDesc* ScriptNode::FindPin(PinID pinId) const
    {
        for (const auto& pin : m_inputPins)
        {
            if (pin.id == pinId)
            {
                return &pin;
            }
        }
        for (const auto& pin : m_outputPins)
        {
            if (pin.id == pinId)
            {
                return &pin;
            }
        }
        return nullptr;
    }

    // ============================================================================
    // VisualScriptGraph
    // ============================================================================

    VisualScriptGraph::VisualScriptGraph(const std::string& name)
        : m_name(name)
    {
    }

    // -- Node management --------------------------------------------------------

    NodeID VisualScriptGraph::AddNode(const std::string& typeName, float posX, float posY)
    {
        const auto* tmpl = NodeLibrary::GetInstance().FindTemplate(typeName);
        if (!tmpl)
        {
            return INVALID_NODE_ID;
        }

        NodeID nodeId = m_nextNodeId++;
        ScriptNode node(nodeId, typeName, tmpl->category);
        node.SetPosition(posX, posY);
        node.SetCodeTemplate(tmpl->codeTemplate);

        if (tmpl->executeCallback)
        {
            node.SetExecuteCallback(tmpl->executeCallback);
        }

        for (const auto& pin : tmpl->inputPins)
        {
            node.AddInputPin(pin.name, pin.type, pin.defaultValue);
        }
        for (const auto& pin : tmpl->outputPins)
        {
            node.AddOutputPin(pin.name, pin.type);
        }

        m_nodes.emplace(nodeId, std::move(node));
        return nodeId;
    }

    bool VisualScriptGraph::RemoveNode(NodeID id)
    {
        auto it = m_nodes.find(id);
        if (it == m_nodes.end())
        {
            return false;
        }

        // Remove all links connected to this node
        m_links.erase(std::remove_if(m_links.begin(), m_links.end(),
                                     [id](const LinkDesc& link)
                                     { return link.sourceNode == id || link.targetNode == id; }),
                       m_links.end());

        m_nodes.erase(it);
        return true;
    }

    ScriptNode* VisualScriptGraph::GetNode(NodeID id)
    {
        auto it = m_nodes.find(id);
        return (it != m_nodes.end()) ? &it->second : nullptr;
    }

    const ScriptNode* VisualScriptGraph::GetNode(NodeID id) const
    {
        auto it = m_nodes.find(id);
        return (it != m_nodes.end()) ? &it->second : nullptr;
    }

    // -- Link management --------------------------------------------------------

    LinkID VisualScriptGraph::AddLink(NodeID srcNode, PinID srcPin, NodeID dstNode, PinID dstPin)
    {
        if (!CanConnect(srcNode, srcPin, dstNode, dstPin))
        {
            return INVALID_LINK_ID;
        }

        // Remove any existing link to the destination input pin (inputs accept one connection)
        m_links.erase(std::remove_if(m_links.begin(), m_links.end(),
                                     [dstNode, dstPin](const LinkDesc& link)
                                     { return link.targetNode == dstNode && link.targetPin == dstPin; }),
                       m_links.end());

        LinkID linkId = m_nextLinkId++;
        LinkDesc link;
        link.id = linkId;
        link.sourceNode = srcNode;
        link.sourcePin = srcPin;
        link.targetNode = dstNode;
        link.targetPin = dstPin;
        m_links.push_back(link);
        return linkId;
    }

    bool VisualScriptGraph::RemoveLink(LinkID id)
    {
        auto it = std::remove_if(m_links.begin(), m_links.end(),
                                 [id](const LinkDesc& link) { return link.id == id; });
        if (it == m_links.end())
        {
            return false;
        }
        m_links.erase(it, m_links.end());
        return true;
    }

    bool VisualScriptGraph::CanConnect(NodeID srcNode, PinID srcPin, NodeID dstNode, PinID dstPin) const
    {
        if (srcNode == dstNode)
        {
            return false;
        }

        const auto* src = GetNode(srcNode);
        const auto* dst = GetNode(dstNode);
        if (!src || !dst)
        {
            return false;
        }

        const auto* srcPinDesc = src->FindPin(srcPin);
        const auto* dstPinDesc = dst->FindPin(dstPin);
        if (!srcPinDesc || !dstPinDesc)
        {
            return false;
        }

        // Source must be output, destination must be input
        if (srcPinDesc->direction != PinDirection::Output || dstPinDesc->direction != PinDirection::Input)
        {
            return false;
        }

        // Type compatibility: same type, or either is Any, or Execution matches only Execution
        if (srcPinDesc->type == PinType::Execution || dstPinDesc->type == PinType::Execution)
        {
            if (srcPinDesc->type != dstPinDesc->type)
            {
                return false;
            }
        }
        else if (srcPinDesc->type != dstPinDesc->type && srcPinDesc->type != PinType::Any &&
                 dstPinDesc->type != PinType::Any)
        {
            return false;
        }

        // Cycle detection: would adding this link create a cycle?
        if (HasCycle(srcNode, dstNode))
        {
            return false;
        }

        return true;
    }

    // -- Variables ---------------------------------------------------------------

    void VisualScriptGraph::AddVariable(const std::string& name, PinType type, const PinValue& defaultVal,
                                        bool isPublic)
    {
        // Overwrite if already exists
        for (auto& var : m_variables)
        {
            if (var.name == name)
            {
                var.type = type;
                var.value = defaultVal;
                var.isPublic = isPublic;
                return;
            }
        }
        m_variables.push_back({name, type, defaultVal, isPublic});
    }

    bool VisualScriptGraph::RemoveVariable(const std::string& name)
    {
        auto it = std::remove_if(m_variables.begin(), m_variables.end(),
                                 [&name](const GraphVariable& v) { return v.name == name; });
        if (it == m_variables.end())
        {
            return false;
        }
        m_variables.erase(it, m_variables.end());
        return true;
    }

    GraphVariable* VisualScriptGraph::GetVariable(const std::string& name)
    {
        for (auto& var : m_variables)
        {
            if (var.name == name)
            {
                return &var;
            }
        }
        return nullptr;
    }

    // -- Cycle detection --------------------------------------------------------

    bool VisualScriptGraph::HasCycle(NodeID fromNode, NodeID toNode) const
    {
        // Check if there is already a path from toNode to fromNode (which would make a cycle)
        std::unordered_map<NodeID, bool> visited;
        return HasCycleHelper(toNode, fromNode, visited);
    }

    bool VisualScriptGraph::HasCycleHelper(NodeID current, NodeID target,
                                            std::unordered_map<NodeID, bool>& visited) const
    {
        if (current == target)
        {
            return true;
        }
        if (visited.count(current))
        {
            return false;
        }
        visited[current] = true;

        for (const auto& link : m_links)
        {
            if (link.sourceNode == current)
            {
                if (HasCycleHelper(link.targetNode, target, visited))
                {
                    return true;
                }
            }
        }
        return false;
    }

    // -- Topological sort -------------------------------------------------------

    std::vector<NodeID> VisualScriptGraph::TopologicalSort() const
    {
        std::unordered_map<NodeID, int> inDegree;
        for (const auto& [id, node] : m_nodes)
        {
            inDegree[id] = 0;
        }
        for (const auto& link : m_links)
        {
            inDegree[link.targetNode]++;
        }

        std::queue<NodeID> zeroIn;
        for (const auto& [id, deg] : inDegree)
        {
            if (deg == 0)
            {
                zeroIn.push(id);
            }
        }

        std::vector<NodeID> sorted;
        sorted.reserve(m_nodes.size());
        while (!zeroIn.empty())
        {
            NodeID current = zeroIn.front();
            zeroIn.pop();
            sorted.push_back(current);

            for (const auto& link : m_links)
            {
                if (link.sourceNode == current)
                {
                    inDegree[link.targetNode]--;
                    if (inDegree[link.targetNode] == 0)
                    {
                        zeroIn.push(link.targetNode);
                    }
                }
            }
        }
        return sorted;
    }

    // -- Validation -------------------------------------------------------------

    VisualScriptGraph::ValidationResult VisualScriptGraph::Validate() const
    {
        ValidationResult result;

        if (m_nodes.empty())
        {
            result.warnings.push_back("Graph is empty — no nodes present.");
            return result;
        }

        // Check for event nodes
        bool hasEventNode = false;
        for (const auto& [id, node] : m_nodes)
        {
            if (node.GetCategory() == NodeCategory::Event)
            {
                hasEventNode = true;
            }
        }
        if (!hasEventNode)
        {
            result.warnings.push_back("Graph has no event nodes — nothing will trigger execution.");
        }

        // Check for unconnected required pins
        for (const auto& [id, node] : m_nodes)
        {
            for (const auto& pin : node.GetInputPins())
            {
                if (pin.type == PinType::Execution)
                {
                    continue; // Execution inputs are optional (node may be called from multiple places)
                }

                bool connected = false;
                for (const auto& link : m_links)
                {
                    if (link.targetNode == id && link.targetPin == pin.id)
                    {
                        connected = true;
                        break;
                    }
                }

                // If not connected and no default value, warn
                if (!connected && std::holds_alternative<std::monostate>(pin.defaultValue))
                {
                    result.warnings.push_back("Node '" + node.GetName() + "' (ID " + std::to_string(id) +
                                              "): input pin '" + pin.name + "' is unconnected with no default value.");
                }
            }
        }

        // Check for type mismatches on existing links
        for (const auto& link : m_links)
        {
            const auto* srcNode = GetNode(link.sourceNode);
            const auto* dstNode = GetNode(link.targetNode);
            if (!srcNode || !dstNode)
            {
                result.isValid = false;
                result.errors.push_back("Link " + std::to_string(link.id) + " references missing node.");
                continue;
            }

            const auto* srcPin = srcNode->FindPin(link.sourcePin);
            const auto* dstPin = dstNode->FindPin(link.targetPin);
            if (!srcPin || !dstPin)
            {
                result.isValid = false;
                result.errors.push_back("Link " + std::to_string(link.id) + " references missing pin.");
                continue;
            }

            if (srcPin->type != dstPin->type && srcPin->type != PinType::Any && dstPin->type != PinType::Any)
            {
                result.isValid = false;
                result.errors.push_back("Link " + std::to_string(link.id) + ": type mismatch between '" +
                                        srcNode->GetName() + "." + srcPin->name + "' and '" + dstNode->GetName() +
                                        "." + dstPin->name + "'.");
            }
        }

        // Check for unreachable nodes (not connected to any event node via execution flow)
        std::set<NodeID> reachable;
        std::queue<NodeID> work;
        for (const auto& [id, node] : m_nodes)
        {
            if (node.GetCategory() == NodeCategory::Event)
            {
                reachable.insert(id);
                work.push(id);
            }
        }
        while (!work.empty())
        {
            NodeID current = work.front();
            work.pop();
            for (const auto& link : m_links)
            {
                if (link.sourceNode == current && !reachable.count(link.targetNode))
                {
                    reachable.insert(link.targetNode);
                    work.push(link.targetNode);
                }
            }
        }
        for (const auto& [id, node] : m_nodes)
        {
            if (!reachable.count(id))
            {
                result.warnings.push_back("Node '" + node.GetName() + "' (ID " + std::to_string(id) +
                                          ") is unreachable from any event node.");
            }
        }

        return result;
    }

    // -- Code generation helpers ------------------------------------------------

    std::string VisualScriptGraph::PinTypeToASType(PinType type) const
    {
        switch (type)
        {
        case PinType::Bool:
            return "bool";
        case PinType::Int:
            return "int";
        case PinType::Float:
            return "float";
        case PinType::String:
            return "string";
        case PinType::Vector2:
            return "Vector2";
        case PinType::Vector3:
            return "Vector3";
        case PinType::Vector4:
            return "Vector4";
        case PinType::Entity:
            return "uint64";
        case PinType::Execution:
        case PinType::Any:
        default:
            return "void";
        }
    }

    std::string VisualScriptGraph::PinValueToASLiteral(const PinValue& value, PinType type) const
    {
        if (std::holds_alternative<std::monostate>(value))
        {
            switch (type)
            {
            case PinType::Bool:
                return "false";
            case PinType::Int:
                return "0";
            case PinType::Float:
                return "0.0f";
            case PinType::String:
                return "\"\"";
            default:
                return "0";
            }
        }

        if (std::holds_alternative<bool>(value))
        {
            return std::get<bool>(value) ? "true" : "false";
        }
        if (std::holds_alternative<int32_t>(value))
        {
            return std::to_string(std::get<int32_t>(value));
        }
        if (std::holds_alternative<float>(value))
        {
            std::ostringstream oss;
            oss << std::get<float>(value) << "f";
            return oss.str();
        }
        if (std::holds_alternative<std::string>(value))
        {
            return "\"" + std::get<std::string>(value) + "\"";
        }
        if (std::holds_alternative<uint64_t>(value))
        {
            return std::to_string(std::get<uint64_t>(value));
        }
        return "0";
    }

    std::string VisualScriptGraph::GenerateNodeCode(const ScriptNode& node,
                                                     const std::unordered_map<PinID, std::string>& pinVarNames) const
    {
        std::string code = node.GetCodeTemplate();
        if (code.empty())
        {
            return "";
        }

        // Replace pin placeholders: {PinName} -> variable name or literal
        for (const auto& pin : node.GetInputPins())
        {
            std::string placeholder = "{" + pin.name + "}";
            std::string replacement;

            // Check if this pin has an incoming link
            auto it = pinVarNames.find(pin.id);
            if (it != pinVarNames.end())
            {
                replacement = it->second;
            }
            else
            {
                replacement = PinValueToASLiteral(pin.defaultValue, pin.type);
            }

            size_t pos = 0;
            while ((pos = code.find(placeholder, pos)) != std::string::npos)
            {
                code.replace(pos, placeholder.length(), replacement);
                pos += replacement.length();
            }
        }

        for (const auto& pin : node.GetOutputPins())
        {
            std::string placeholder = "{" + pin.name + "}";
            auto it = pinVarNames.find(pin.id);
            if (it != pinVarNames.end())
            {
                size_t pos = 0;
                while ((pos = code.find(placeholder, pos)) != std::string::npos)
                {
                    code.replace(pos, placeholder.length(), it->second);
                    pos += it->second.length();
                }
            }
        }

        return code;
    }

    // -- Compilation to AngelScript ---------------------------------------------

    std::string VisualScriptGraph::CompileToAngelScript() const
    {
        std::ostringstream out;
        out << "// Auto-generated from visual script: " << m_name << "\n";
        out << "class " << m_name << "\n{\n";

        // Member variables
        for (const auto& var : m_variables)
        {
            out << "    " << PinTypeToASType(var.type) << " " << var.name;
            if (!std::holds_alternative<std::monostate>(var.value))
            {
                out << " = " << PinValueToASLiteral(var.value, var.type);
            }
            out << ";\n";
        }
        if (!m_variables.empty())
        {
            out << "\n";
        }

        // Find event nodes and generate a method for each
        for (const auto& [nodeId, node] : m_nodes)
        {
            if (node.GetCategory() != NodeCategory::Event)
            {
                continue;
            }

            // Determine method name and parameters
            std::string methodName = node.GetName();
            std::string params;
            if (methodName == "Tick")
            {
                params = "float deltaTime";
            }
            else if (methodName == "OnCollision")
            {
                params = "uint64 otherEntity";
            }

            out << "    void " << methodName << "(" << params << ")\n    {\n";

            // Walk execution flow from this event node
            // Build a map of pin -> variable names for data flow
            std::unordered_map<PinID, std::string> pinVarNames;
            int tempVarCounter = 0;

            // Gather data flow dependencies via topological order
            auto sorted = TopologicalSort();

            // For data nodes (non-execution flow), pre-compute variable names
            for (NodeID nid : sorted)
            {
                const auto* n = GetNode(nid);
                if (!n || n->GetCategory() == NodeCategory::Event)
                {
                    continue;
                }

                for (const auto& outPin : n->GetOutputPins())
                {
                    if (outPin.type != PinType::Execution)
                    {
                        std::string varName = "temp_" + std::to_string(tempVarCounter++);
                        pinVarNames[outPin.id] = varName;
                    }
                }
            }

            // Resolve input pin connections to variable names
            for (const auto& link : m_links)
            {
                auto srcIt = pinVarNames.find(link.sourcePin);
                if (srcIt != pinVarNames.end())
                {
                    pinVarNames[link.targetPin] = srcIt->second;
                }
            }

            // Walk execution flow starting from the event node
            std::set<NodeID> visited;
            std::function<void(NodeID)> emitExecChain = [&](NodeID currentId)
            {
                if (visited.count(currentId))
                {
                    return;
                }
                visited.insert(currentId);

                const auto* currentNode = GetNode(currentId);
                if (!currentNode)
                {
                    return;
                }

                // First, emit code for data dependencies of this node
                for (const auto& inPin : currentNode->GetInputPins())
                {
                    if (inPin.type == PinType::Execution)
                    {
                        continue;
                    }

                    for (const auto& link : m_links)
                    {
                        if (link.targetNode == currentId && link.targetPin == inPin.id)
                        {
                            const auto* dataNode = GetNode(link.sourceNode);
                            if (dataNode && dataNode->GetCategory() != NodeCategory::Event && !visited.count(link.sourceNode))
                            {
                                // Emit data node computation
                                visited.insert(link.sourceNode);
                                for (const auto& outPin : dataNode->GetOutputPins())
                                {
                                    if (outPin.type != PinType::Execution)
                                    {
                                        auto varIt = pinVarNames.find(outPin.id);
                                        if (varIt != pinVarNames.end())
                                        {
                                            std::string nodeCode =
                                                GenerateNodeCode(*dataNode, pinVarNames);
                                            if (!nodeCode.empty())
                                            {
                                                out << "        " << PinTypeToASType(outPin.type) << " "
                                                    << varIt->second << " = " << nodeCode << ";\n";
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // Emit this node's code (skip event nodes — they are the method itself)
                if (currentNode->GetCategory() != NodeCategory::Event)
                {
                    std::string code = GenerateNodeCode(*currentNode, pinVarNames);
                    if (!code.empty())
                    {
                        out << "        " << code << "\n";
                    }
                }

                // Follow execution output pins
                for (const auto& outPin : currentNode->GetOutputPins())
                {
                    if (outPin.type == PinType::Execution)
                    {
                        for (const auto& link : m_links)
                        {
                            if (link.sourceNode == currentId && link.sourcePin == outPin.id)
                            {
                                emitExecChain(link.targetNode);
                            }
                        }
                    }
                }
            };

            emitExecChain(nodeId);

            out << "    }\n\n";
        }

        out << "}\n";
        return out.str();
    }

    // -- Execution (interpreter mode) -------------------------------------------

    void VisualScriptGraph::Execute(NodeID eventNodeId)
    {
        const auto* eventNode = GetNode(eventNodeId);
        if (!eventNode || eventNode->GetCategory() != NodeCategory::Event)
        {
            return;
        }

        // Walk execution flow pins
        std::set<NodeID> visited;
        std::function<void(NodeID)> executeChain = [&](NodeID currentId)
        {
            if (visited.count(currentId))
            {
                return;
            }
            visited.insert(currentId);

            auto* currentNode = GetNode(currentId);
            if (!currentNode)
            {
                return;
            }

            // Resolve input data values from connected output pins
            std::vector<PinValue> inputValues;
            for (const auto& inPin : currentNode->GetInputPins())
            {
                if (inPin.type == PinType::Execution)
                {
                    continue;
                }

                PinValue resolvedValue = inPin.defaultValue;
                for (const auto& link : m_links)
                {
                    if (link.targetNode == currentId && link.targetPin == inPin.id)
                    {
                        // Recursively execute the source node to get its output
                        auto* srcNode = GetNode(link.sourceNode);
                        if (srcNode && srcNode->GetExecuteCallback())
                        {
                            // Gather source node's inputs recursively
                            std::vector<PinValue> srcInputs;
                            for (const auto& srcInPin : srcNode->GetInputPins())
                            {
                                if (srcInPin.type != PinType::Execution)
                                {
                                    srcInputs.push_back(srcInPin.defaultValue);
                                }
                            }
                            auto srcOutputs = srcNode->GetExecuteCallback()(srcInputs);

                            // Find which output index corresponds to the source pin
                            int outIdx = 0;
                            for (const auto& outPin : srcNode->GetOutputPins())
                            {
                                if (outPin.type == PinType::Execution)
                                {
                                    continue;
                                }
                                if (outPin.id == link.sourcePin && outIdx < static_cast<int>(srcOutputs.size()))
                                {
                                    resolvedValue = srcOutputs[outIdx];
                                    break;
                                }
                                outIdx++;
                            }
                        }
                        break;
                    }
                }
                inputValues.push_back(resolvedValue);
            }

            // Execute the node's callback
            if (currentNode->GetExecuteCallback())
            {
                currentNode->GetExecuteCallback()(inputValues);
            }

            // Follow execution output pins to the next node
            for (const auto& outPin : currentNode->GetOutputPins())
            {
                if (outPin.type == PinType::Execution)
                {
                    for (const auto& link : m_links)
                    {
                        if (link.sourceNode == currentId && link.sourcePin == outPin.id)
                        {
                            executeChain(link.targetNode);
                        }
                    }
                }
            }
        };

        executeChain(eventNodeId);
    }

    // -- Serialisation ----------------------------------------------------------

    static std::string EscapeJSON(const std::string& str)
    {
        std::ostringstream oss;
        for (char c : str)
        {
            switch (c)
            {
            case '"':
                oss << "\\\"";
                break;
            case '\\':
                oss << "\\\\";
                break;
            case '\n':
                oss << "\\n";
                break;
            case '\r':
                oss << "\\r";
                break;
            case '\t':
                oss << "\\t";
                break;
            default:
                oss << c;
                break;
            }
        }
        return oss.str();
    }

    static std::string PinTypeToString(PinType type)
    {
        switch (type)
        {
        case PinType::Execution:
            return "Execution";
        case PinType::Bool:
            return "Bool";
        case PinType::Int:
            return "Int";
        case PinType::Float:
            return "Float";
        case PinType::String:
            return "String";
        case PinType::Vector2:
            return "Vector2";
        case PinType::Vector3:
            return "Vector3";
        case PinType::Vector4:
            return "Vector4";
        case PinType::Entity:
            return "Entity";
        case PinType::Any:
            return "Any";
        default:
            return "Any";
        }
    }

    static PinType StringToPinType(const std::string& str)
    {
        if (str == "Execution")
            return PinType::Execution;
        if (str == "Bool")
            return PinType::Bool;
        if (str == "Int")
            return PinType::Int;
        if (str == "Float")
            return PinType::Float;
        if (str == "String")
            return PinType::String;
        if (str == "Vector2")
            return PinType::Vector2;
        if (str == "Vector3")
            return PinType::Vector3;
        if (str == "Vector4")
            return PinType::Vector4;
        if (str == "Entity")
            return PinType::Entity;
        return PinType::Any;
    }

    std::string VisualScriptGraph::SerializeToJSON() const
    {
        std::ostringstream json;
        json << "{\n";
        json << "  \"name\": \"" << EscapeJSON(m_name) << "\",\n";

        // Nodes
        json << "  \"nodes\": [\n";
        bool firstNode = true;
        for (const auto& [id, node] : m_nodes)
        {
            if (!firstNode)
            {
                json << ",\n";
            }
            firstNode = false;
            json << "    {\n";
            json << "      \"id\": " << id << ",\n";
            json << "      \"typeName\": \"" << EscapeJSON(node.GetName()) << "\",\n";
            json << "      \"posX\": " << node.GetPosX() << ",\n";
            json << "      \"posY\": " << node.GetPosY() << "\n";
            json << "    }";
        }
        json << "\n  ],\n";

        // Links
        json << "  \"links\": [\n";
        for (size_t i = 0; i < m_links.size(); ++i)
        {
            const auto& link = m_links[i];
            json << "    {\n";
            json << "      \"id\": " << link.id << ",\n";
            json << "      \"sourceNode\": " << link.sourceNode << ",\n";
            json << "      \"sourcePin\": " << link.sourcePin << ",\n";
            json << "      \"targetNode\": " << link.targetNode << ",\n";
            json << "      \"targetPin\": " << link.targetPin << "\n";
            json << "    }";
            if (i + 1 < m_links.size())
            {
                json << ",";
            }
            json << "\n";
        }
        json << "  ],\n";

        // Variables
        json << "  \"variables\": [\n";
        for (size_t i = 0; i < m_variables.size(); ++i)
        {
            const auto& var = m_variables[i];
            json << "    {\n";
            json << "      \"name\": \"" << EscapeJSON(var.name) << "\",\n";
            json << "      \"type\": \"" << PinTypeToString(var.type) << "\",\n";
            json << "      \"isPublic\": " << (var.isPublic ? "true" : "false") << "\n";
            json << "    }";
            if (i + 1 < m_variables.size())
            {
                json << ",";
            }
            json << "\n";
        }
        json << "  ]\n";

        json << "}\n";
        return json.str();
    }

    // Minimal JSON parser helpers (no external library)
    static std::string ExtractJSONString(const std::string& json, const std::string& key)
    {
        std::string searchKey = "\"" + key + "\"";
        size_t pos = json.find(searchKey);
        if (pos == std::string::npos)
        {
            return "";
        }
        pos = json.find(':', pos);
        if (pos == std::string::npos)
        {
            return "";
        }
        pos = json.find('"', pos + 1);
        if (pos == std::string::npos)
        {
            return "";
        }
        size_t start = pos + 1;
        size_t end = start;
        while (end < json.size())
        {
            if (json[end] == '\\')
            {
                end += 2;
                continue;
            }
            if (json[end] == '"')
            {
                break;
            }
            end++;
        }
        return json.substr(start, end - start);
    }

    static int ExtractJSONInt(const std::string& json, const std::string& key)
    {
        std::string searchKey = "\"" + key + "\"";
        size_t pos = json.find(searchKey);
        if (pos == std::string::npos)
        {
            return 0;
        }
        pos = json.find(':', pos);
        if (pos == std::string::npos)
        {
            return 0;
        }
        pos++;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
        {
            pos++;
        }
        return std::stoi(json.substr(pos));
    }

    static float ExtractJSONFloat(const std::string& json, const std::string& key)
    {
        std::string searchKey = "\"" + key + "\"";
        size_t pos = json.find(searchKey);
        if (pos == std::string::npos)
        {
            return 0.0f;
        }
        pos = json.find(':', pos);
        if (pos == std::string::npos)
        {
            return 0.0f;
        }
        pos++;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
        {
            pos++;
        }
        return std::stof(json.substr(pos));
    }

    static bool ExtractJSONBool(const std::string& json, const std::string& key)
    {
        std::string searchKey = "\"" + key + "\"";
        size_t pos = json.find(searchKey);
        if (pos == std::string::npos)
        {
            return false;
        }
        pos = json.find(':', pos);
        if (pos == std::string::npos)
        {
            return false;
        }
        return json.find("true", pos) < json.find("false", pos);
    }

    static std::vector<std::string> ExtractJSONArray(const std::string& json, const std::string& key)
    {
        std::vector<std::string> items;
        std::string searchKey = "\"" + key + "\"";
        size_t pos = json.find(searchKey);
        if (pos == std::string::npos)
        {
            return items;
        }
        pos = json.find('[', pos);
        if (pos == std::string::npos)
        {
            return items;
        }

        int depth = 0;
        size_t itemStart = 0;
        for (size_t i = pos; i < json.size(); ++i)
        {
            if (json[i] == '[')
            {
                depth++;
            }
            else if (json[i] == ']')
            {
                depth--;
                if (depth == 0)
                {
                    break;
                }
            }
            else if (json[i] == '{' && depth == 1)
            {
                itemStart = i;
            }
            else if (json[i] == '}' && depth == 1)
            {
                items.push_back(json.substr(itemStart, i - itemStart + 1));
            }
        }
        return items;
    }

    bool VisualScriptGraph::DeserializeFromJSON(const std::string& json)
    {
        m_nodes.clear();
        m_links.clear();
        m_variables.clear();
        m_nextNodeId = 1;
        m_nextLinkId = 1;

        m_name = ExtractJSONString(json, "name");
        if (m_name.empty())
        {
            m_name = "Untitled";
        }

        // Parse nodes
        auto nodeItems = ExtractJSONArray(json, "nodes");
        for (const auto& item : nodeItems)
        {
            std::string typeName = ExtractJSONString(item, "typeName");
            float posX = ExtractJSONFloat(item, "posX");
            float posY = ExtractJSONFloat(item, "posY");
            int serializedId = ExtractJSONInt(item, "id");

            NodeID createdId = AddNode(typeName, posX, posY);
            // Keep IDs in sync: if the serialised ID is higher, advance counter
            if (static_cast<NodeID>(serializedId) >= m_nextNodeId)
            {
                m_nextNodeId = static_cast<NodeID>(serializedId) + 1;
            }
            // Remap: the created node may have a different ID — for simplicity,
            // we rely on sequential creation matching serialised order.
            (void)createdId;
        }

        // Parse links
        auto linkItems = ExtractJSONArray(json, "links");
        for (const auto& item : linkItems)
        {
            NodeID srcNode = static_cast<NodeID>(ExtractJSONInt(item, "sourceNode"));
            PinID srcPin = static_cast<PinID>(ExtractJSONInt(item, "sourcePin"));
            NodeID dstNode = static_cast<NodeID>(ExtractJSONInt(item, "targetNode"));
            PinID dstPin = static_cast<PinID>(ExtractJSONInt(item, "targetPin"));

            // Direct insert (skip validation during deserialization for perf)
            LinkID linkId = m_nextLinkId++;
            LinkDesc link;
            link.id = linkId;
            link.sourceNode = srcNode;
            link.sourcePin = srcPin;
            link.targetNode = dstNode;
            link.targetPin = dstPin;
            m_links.push_back(link);
        }

        // Parse variables
        auto varItems = ExtractJSONArray(json, "variables");
        for (const auto& item : varItems)
        {
            std::string name = ExtractJSONString(item, "name");
            PinType type = StringToPinType(ExtractJSONString(item, "type"));
            bool isPublic = ExtractJSONBool(item, "isPublic");
            AddVariable(name, type, {}, isPublic);
        }

        return true;
    }

    // ============================================================================
    // NodeLibrary
    // ============================================================================

    NodeLibrary& NodeLibrary::GetInstance()
    {
        static NodeLibrary instance;
        return instance;
    }

    void NodeLibrary::RegisterTemplate(const NodeTemplate& tmpl)
    {
        m_templates[tmpl.typeName] = tmpl;
    }

    const NodeLibrary::NodeTemplate* NodeLibrary::FindTemplate(const std::string& typeName) const
    {
        auto it = m_templates.find(typeName);
        return (it != m_templates.end()) ? &it->second : nullptr;
    }

    std::vector<const NodeLibrary::NodeTemplate*> NodeLibrary::GetTemplatesByCategory(NodeCategory category) const
    {
        std::vector<const NodeTemplate*> result;
        for (const auto& [name, tmpl] : m_templates)
        {
            if (tmpl.category == category)
            {
                result.push_back(&tmpl);
            }
        }
        return result;
    }

    void NodeLibrary::RegisterBuiltinNodes()
    {
        if (m_builtinsRegistered)
        {
            return;
        }
        m_builtinsRegistered = true;

        // Helper lambda to build pin descriptors
        auto MakePin = [](const std::string& name, PinType type, PinDirection dir,
                          const PinValue& defVal = {}) -> PinDesc
        {
            PinDesc p;
            p.name = name;
            p.type = type;
            p.direction = dir;
            p.defaultValue = defVal;
            return p;
        };
        auto In = [&](const std::string& n, PinType t, const PinValue& d = {})
        { return MakePin(n, t, PinDirection::Input, d); };
        auto Out = [&](const std::string& n, PinType t)
        { return MakePin(n, t, PinDirection::Output); };
        auto ExecIn = [&](const std::string& n = "Exec")
        { return MakePin(n, PinType::Execution, PinDirection::Input); };
        auto ExecOut = [&](const std::string& n = "Then")
        { return MakePin(n, PinType::Execution, PinDirection::Output); };

        // ===================== Events =====================
        RegisterTemplate({"BeginPlay", "Begin Play", NodeCategory::Event, {},
                          {ExecOut()}, "", nullptr});

        RegisterTemplate({"Tick", "Tick", NodeCategory::Event, {},
                          {ExecOut(), Out("DeltaTime", PinType::Float)}, "", nullptr});

        RegisterTemplate({"OnCollision", "On Collision", NodeCategory::Event, {},
                          {ExecOut(), Out("OtherEntity", PinType::Entity)}, "", nullptr});

        // ===================== Flow Control =====================
        RegisterTemplate(
            {"Branch", "Branch", NodeCategory::FlowControl,
             {ExecIn(), In("Condition", PinType::Bool, false)},
             {ExecOut("True"), ExecOut("False")},
             "if ({Condition})",
             [](const std::vector<PinValue>& inputs) -> std::vector<PinValue>
             {
                 bool cond = inputs.size() > 0 && std::holds_alternative<bool>(inputs[0]) && std::get<bool>(inputs[0]);
                 return {cond};
             }});

        RegisterTemplate(
            {"ForLoop", "For Loop", NodeCategory::FlowControl,
             {ExecIn(), In("Start", PinType::Int, 0), In("End", PinType::Int, 10)},
             {ExecOut("LoopBody"), ExecOut("Completed"), Out("Index", PinType::Int)},
             "for (int i = {Start}; i < {End}; i++)",
             nullptr});

        RegisterTemplate(
            {"Sequence", "Sequence", NodeCategory::FlowControl,
             {ExecIn()},
             {ExecOut("Then0"), ExecOut("Then1"), ExecOut("Then2")},
             "", nullptr});

        RegisterTemplate(
            {"DoOnce", "Do Once", NodeCategory::FlowControl,
             {ExecIn()},
             {ExecOut()},
             "if (!_doOnce) { _doOnce = true;",
             nullptr});

        // ===================== Math =====================
        auto MakeBinaryMathNode = [&](const std::string& name, const std::string& op,
                                      const std::string& display)
        {
            RegisterTemplate(
                {name, display, NodeCategory::Math,
                 {In("A", PinType::Float, 0.0f), In("B", PinType::Float, 0.0f)},
                 {Out("Result", PinType::Float)},
                 "({A} " + op + " {B})",
                 [op](const std::vector<PinValue>& inputs) -> std::vector<PinValue>
                 {
                     float a = (inputs.size() > 0 && std::holds_alternative<float>(inputs[0]))
                                   ? std::get<float>(inputs[0])
                                   : 0.0f;
                     float b = (inputs.size() > 1 && std::holds_alternative<float>(inputs[1]))
                                   ? std::get<float>(inputs[1])
                                   : 0.0f;
                     float result = 0.0f;
                     if (op == "+")
                         result = a + b;
                     else if (op == "-")
                         result = a - b;
                     else if (op == "*")
                         result = a * b;
                     else if (op == "/")
                         result = (b != 0.0f) ? a / b : 0.0f;
                     return {result};
                 }});
        };

        MakeBinaryMathNode("Add", "+", "Add");
        MakeBinaryMathNode("Subtract", "-", "Subtract");
        MakeBinaryMathNode("Multiply", "*", "Multiply");
        MakeBinaryMathNode("Divide", "/", "Divide");

        RegisterTemplate(
            {"Clamp", "Clamp", NodeCategory::Math,
             {In("Value", PinType::Float, 0.0f), In("Min", PinType::Float, 0.0f),
              In("Max", PinType::Float, 1.0f)},
             {Out("Result", PinType::Float)},
             "Math::Clamp({Value}, {Min}, {Max})",
             [](const std::vector<PinValue>& inputs) -> std::vector<PinValue>
             {
                 float val = (inputs.size() > 0 && std::holds_alternative<float>(inputs[0]))
                                 ? std::get<float>(inputs[0])
                                 : 0.0f;
                 float lo = (inputs.size() > 1 && std::holds_alternative<float>(inputs[1]))
                                ? std::get<float>(inputs[1])
                                : 0.0f;
                 float hi = (inputs.size() > 2 && std::holds_alternative<float>(inputs[2]))
                                ? std::get<float>(inputs[2])
                                : 1.0f;
                 return {std::clamp(val, lo, hi)};
             }});

        RegisterTemplate(
            {"Lerp", "Lerp", NodeCategory::Math,
             {In("A", PinType::Float, 0.0f), In("B", PinType::Float, 1.0f),
              In("Alpha", PinType::Float, 0.5f)},
             {Out("Result", PinType::Float)},
             "Math::Lerp({A}, {B}, {Alpha})",
             [](const std::vector<PinValue>& inputs) -> std::vector<PinValue>
             {
                 float a = (inputs.size() > 0 && std::holds_alternative<float>(inputs[0]))
                               ? std::get<float>(inputs[0])
                               : 0.0f;
                 float b = (inputs.size() > 1 && std::holds_alternative<float>(inputs[1]))
                               ? std::get<float>(inputs[1])
                               : 1.0f;
                 float t = (inputs.size() > 2 && std::holds_alternative<float>(inputs[2]))
                               ? std::get<float>(inputs[2])
                               : 0.5f;
                 return {a + (b - a) * t};
             }});

        auto MakeUnaryMathNode = [&](const std::string& name, const std::string& display,
                                     const std::string& code, auto func)
        {
            RegisterTemplate(
                {name, display, NodeCategory::Math,
                 {In("Value", PinType::Float, 0.0f)},
                 {Out("Result", PinType::Float)},
                 code,
                 [func](const std::vector<PinValue>& inputs) -> std::vector<PinValue>
                 {
                     float val = (inputs.size() > 0 && std::holds_alternative<float>(inputs[0]))
                                     ? std::get<float>(inputs[0])
                                     : 0.0f;
                     return {func(val)};
                 }});
        };

        MakeUnaryMathNode("Abs", "Abs", "Math::Abs({Value})", [](float v) { return std::abs(v); });
        MakeUnaryMathNode("Sin", "Sin", "Math::Sin({Value})", [](float v) { return std::sin(v); });
        MakeUnaryMathNode("Cos", "Cos", "Math::Cos({Value})", [](float v) { return std::cos(v); });
        MakeUnaryMathNode("Sqrt", "Square Root", "Math::Sqrt({Value})", [](float v) { return std::sqrt(v); });

        RegisterTemplate(
            {"Min", "Min", NodeCategory::Math,
             {In("A", PinType::Float, 0.0f), In("B", PinType::Float, 0.0f)},
             {Out("Result", PinType::Float)},
             "Math::Min({A}, {B})",
             [](const std::vector<PinValue>& inputs) -> std::vector<PinValue>
             {
                 float a = (inputs.size() > 0 && std::holds_alternative<float>(inputs[0]))
                               ? std::get<float>(inputs[0])
                               : 0.0f;
                 float b = (inputs.size() > 1 && std::holds_alternative<float>(inputs[1]))
                               ? std::get<float>(inputs[1])
                               : 0.0f;
                 return {std::min(a, b)};
             }});

        RegisterTemplate(
            {"Max", "Max", NodeCategory::Math,
             {In("A", PinType::Float, 0.0f), In("B", PinType::Float, 0.0f)},
             {Out("Result", PinType::Float)},
             "Math::Max({A}, {B})",
             [](const std::vector<PinValue>& inputs) -> std::vector<PinValue>
             {
                 float a = (inputs.size() > 0 && std::holds_alternative<float>(inputs[0]))
                               ? std::get<float>(inputs[0])
                               : 0.0f;
                 float b = (inputs.size() > 1 && std::holds_alternative<float>(inputs[1]))
                               ? std::get<float>(inputs[1])
                               : 0.0f;
                 return {std::max(a, b)};
             }});

        // ===================== Logic =====================
        RegisterTemplate(
            {"AND", "AND", NodeCategory::Logic,
             {In("A", PinType::Bool, false), In("B", PinType::Bool, false)},
             {Out("Result", PinType::Bool)},
             "({A} && {B})",
             [](const std::vector<PinValue>& inputs) -> std::vector<PinValue>
             {
                 bool a = inputs.size() > 0 && std::holds_alternative<bool>(inputs[0]) && std::get<bool>(inputs[0]);
                 bool b = inputs.size() > 1 && std::holds_alternative<bool>(inputs[1]) && std::get<bool>(inputs[1]);
                 return {a && b};
             }});

        RegisterTemplate(
            {"OR", "OR", NodeCategory::Logic,
             {In("A", PinType::Bool, false), In("B", PinType::Bool, false)},
             {Out("Result", PinType::Bool)},
             "({A} || {B})",
             [](const std::vector<PinValue>& inputs) -> std::vector<PinValue>
             {
                 bool a = inputs.size() > 0 && std::holds_alternative<bool>(inputs[0]) && std::get<bool>(inputs[0]);
                 bool b = inputs.size() > 1 && std::holds_alternative<bool>(inputs[1]) && std::get<bool>(inputs[1]);
                 return {a || b};
             }});

        RegisterTemplate(
            {"NOT", "NOT", NodeCategory::Logic,
             {In("Value", PinType::Bool, false)},
             {Out("Result", PinType::Bool)},
             "(!{Value})",
             [](const std::vector<PinValue>& inputs) -> std::vector<PinValue>
             {
                 bool val = inputs.size() > 0 && std::holds_alternative<bool>(inputs[0]) && std::get<bool>(inputs[0]);
                 return {!val};
             }});

        auto MakeCompareNode = [&](const std::string& name, const std::string& display,
                                   const std::string& op)
        {
            RegisterTemplate(
                {name, display, NodeCategory::Logic,
                 {In("A", PinType::Float, 0.0f), In("B", PinType::Float, 0.0f)},
                 {Out("Result", PinType::Bool)},
                 "({A} " + op + " {B})",
                 [op](const std::vector<PinValue>& inputs) -> std::vector<PinValue>
                 {
                     float a = (inputs.size() > 0 && std::holds_alternative<float>(inputs[0]))
                                   ? std::get<float>(inputs[0])
                                   : 0.0f;
                     float b = (inputs.size() > 1 && std::holds_alternative<float>(inputs[1]))
                                   ? std::get<float>(inputs[1])
                                   : 0.0f;
                     bool result = false;
                     if (op == "==")
                         result = (a == b);
                     else if (op == "!=")
                         result = (a != b);
                     else if (op == ">")
                         result = (a > b);
                     else if (op == "<")
                         result = (a < b);
                     return {result};
                 }});
        };

        MakeCompareNode("Equal", "Equal", "==");
        MakeCompareNode("NotEqual", "Not Equal", "!=");
        MakeCompareNode("Greater", "Greater Than", ">");
        MakeCompareNode("Less", "Less Than", "<");

        // ===================== String =====================
        RegisterTemplate(
            {"Concat", "Concatenate", NodeCategory::String,
             {In("A", PinType::String, std::string("")), In("B", PinType::String, std::string(""))},
             {Out("Result", PinType::String)},
             "({A} + {B})",
             [](const std::vector<PinValue>& inputs) -> std::vector<PinValue>
             {
                 std::string a = (inputs.size() > 0 && std::holds_alternative<std::string>(inputs[0]))
                                     ? std::get<std::string>(inputs[0])
                                     : "";
                 std::string b = (inputs.size() > 1 && std::holds_alternative<std::string>(inputs[1]))
                                     ? std::get<std::string>(inputs[1])
                                     : "";
                 return {a + b};
             }});

        RegisterTemplate(
            {"Length", "String Length", NodeCategory::String,
             {In("Value", PinType::String, std::string(""))},
             {Out("Length", PinType::Int)},
             "{Value}.length()",
             [](const std::vector<PinValue>& inputs) -> std::vector<PinValue>
             {
                 std::string val = (inputs.size() > 0 && std::holds_alternative<std::string>(inputs[0]))
                                       ? std::get<std::string>(inputs[0])
                                       : "";
                 return {static_cast<int32_t>(val.length())};
             }});

        RegisterTemplate(
            {"Substring", "Substring", NodeCategory::String,
             {In("Value", PinType::String, std::string("")), In("Start", PinType::Int, 0),
              In("Count", PinType::Int, 1)},
             {Out("Result", PinType::String)},
             "{Value}.substr({Start}, {Count})",
             [](const std::vector<PinValue>& inputs) -> std::vector<PinValue>
             {
                 std::string val = (inputs.size() > 0 && std::holds_alternative<std::string>(inputs[0]))
                                       ? std::get<std::string>(inputs[0])
                                       : "";
                 int start = (inputs.size() > 1 && std::holds_alternative<int32_t>(inputs[1]))
                                 ? std::get<int32_t>(inputs[1])
                                 : 0;
                 int count = (inputs.size() > 2 && std::holds_alternative<int32_t>(inputs[2]))
                                 ? std::get<int32_t>(inputs[2])
                                 : 1;
                 if (start < 0 || start >= static_cast<int>(val.length()))
                 {
                     return {std::string("")};
                 }
                 return {val.substr(static_cast<size_t>(start), static_cast<size_t>(count))};
             }});

        RegisterTemplate(
            {"ToString", "To String", NodeCategory::String,
             {In("Value", PinType::Float, 0.0f)},
             {Out("Result", PinType::String)},
             "formatFloat({Value})",
             [](const std::vector<PinValue>& inputs) -> std::vector<PinValue>
             {
                 float val = (inputs.size() > 0 && std::holds_alternative<float>(inputs[0]))
                                 ? std::get<float>(inputs[0])
                                 : 0.0f;
                 return {std::to_string(val)};
             }});

        // ===================== Variable =====================
        RegisterTemplate(
            {"GetVariable", "Get Variable", NodeCategory::Variable,
             {In("Name", PinType::String, std::string(""))},
             {Out("Value", PinType::Any)},
             "{Name}",
             nullptr});

        RegisterTemplate(
            {"SetVariable", "Set Variable", NodeCategory::Variable,
             {ExecIn(), In("Name", PinType::String, std::string("")), In("Value", PinType::Any)},
             {ExecOut()},
             "{Name} = {Value};",
             nullptr});

        // ===================== Entity =====================
        RegisterTemplate(
            {"GetPosition", "Get Position", NodeCategory::Entity,
             {In("Entity", PinType::Entity)},
             {Out("Position", PinType::Vector3)},
             "GetEntityPosition({Entity})",
             nullptr});

        RegisterTemplate(
            {"SetPosition", "Set Position", NodeCategory::Entity,
             {ExecIn(), In("Entity", PinType::Entity), In("Position", PinType::Vector3)},
             {ExecOut()},
             "SetEntityPosition({Entity}, {Position});",
             nullptr});

        RegisterTemplate(
            {"SpawnEntity", "Spawn Entity", NodeCategory::Entity,
             {ExecIn(), In("Template", PinType::String, std::string(""))},
             {ExecOut(), Out("Entity", PinType::Entity)},
             "uint64 {Entity} = SpawnEntity({Template});",
             nullptr});

        RegisterTemplate(
            {"DestroyEntity", "Destroy Entity", NodeCategory::Entity,
             {ExecIn(), In("Entity", PinType::Entity)},
             {ExecOut()},
             "DestroyEntity({Entity});",
             nullptr});

        // ===================== Input =====================
        RegisterTemplate(
            {"IsKeyDown", "Is Key Down", NodeCategory::Input,
             {In("Key", PinType::String, std::string("Space"))},
             {Out("Pressed", PinType::Bool)},
             "Input::IsKeyDown({Key})",
             nullptr});

        RegisterTemplate(
            {"GetAxis", "Get Axis", NodeCategory::Input,
             {In("Axis", PinType::String, std::string("Horizontal"))},
             {Out("Value", PinType::Float)},
             "Input::GetAxis({Axis})",
             nullptr});

        // ===================== Debug =====================
        RegisterTemplate(
            {"PrintString", "Print String", NodeCategory::Debug,
             {ExecIn(), In("Message", PinType::String, std::string("Hello"))},
             {ExecOut()},
             "Print({Message});",
             [](const std::vector<PinValue>& inputs) -> std::vector<PinValue>
             {
                 std::string msg = (inputs.size() > 0 && std::holds_alternative<std::string>(inputs[0]))
                                       ? std::get<std::string>(inputs[0])
                                       : "";
                 std::cout << "[VisualScript] " << msg << std::endl;
                 return {};
             }});

        RegisterTemplate(
            {"DrawDebugLine", "Draw Debug Line", NodeCategory::Debug,
             {ExecIn(), In("Start", PinType::Vector3), In("End", PinType::Vector3),
              In("Duration", PinType::Float, 1.0f)},
             {ExecOut()},
             "Debug::DrawLine({Start}, {End}, {Duration});",
             nullptr});
    }

    // ============================================================================
    // VisualScriptSystem
    // ============================================================================

    VisualScriptSystem& VisualScriptSystem::GetInstance()
    {
        static VisualScriptSystem instance;
        return instance;
    }

    bool VisualScriptSystem::Initialize()
    {
        if (m_isInitialized)
        {
            return true;
        }

        NodeLibrary::GetInstance().RegisterBuiltinNodes();
        m_isInitialized = true;
        std::cout << "[VisualScriptSystem] Initialized with " << NodeLibrary::GetInstance().GetAllTemplates().size()
                  << " node templates." << std::endl;
        return true;
    }

    void VisualScriptSystem::Shutdown()
    {
        m_graphs.clear();
        m_isInitialized = false;
        std::cout << "[VisualScriptSystem] Shutdown." << std::endl;
    }

    VisualScriptGraph* VisualScriptSystem::CreateGraph(const std::string& name)
    {
        if (m_graphs.count(name))
        {
            return m_graphs[name].get();
        }

        auto graph = std::make_unique<VisualScriptGraph>(name);
        auto* ptr = graph.get();
        m_graphs[name] = std::move(graph);
        return ptr;
    }

    VisualScriptGraph* VisualScriptSystem::GetGraph(const std::string& name)
    {
        auto it = m_graphs.find(name);
        return (it != m_graphs.end()) ? it->second.get() : nullptr;
    }

    bool VisualScriptSystem::RemoveGraph(const std::string& name)
    {
        return m_graphs.erase(name) > 0;
    }

    bool VisualScriptSystem::CompileAndRegister(const std::string& graphName)
    {
        auto* graph = GetGraph(graphName);
        if (!graph)
        {
            std::cerr << "[VisualScriptSystem] Cannot compile — graph '" << graphName << "' not found." << std::endl;
            return false;
        }

        auto validation = graph->Validate();
        if (!validation.isValid)
        {
            std::cerr << "[VisualScriptSystem] Validation failed for '" << graphName << "':" << std::endl;
            for (const auto& err : validation.errors)
            {
                std::cerr << "  ERROR: " << err << std::endl;
            }
            return false;
        }

        for (const auto& warn : validation.warnings)
        {
            std::cout << "  WARNING: " << warn << std::endl;
        }

        std::string source = graph->CompileToAngelScript();
        if (source.empty())
        {
            std::cerr << "[VisualScriptSystem] Compilation produced empty output for '" << graphName << "'."
                      << std::endl;
            return false;
        }

        std::cout << "[VisualScriptSystem] Compiled '" << graphName << "' (" << source.size() << " bytes):\n"
                  << source << std::endl;

        // In a full implementation this would register with the AngelScript engine via
        // AngelScriptEngine::GetInstance().AddScriptSection(graphName, source);
        return true;
    }

    bool VisualScriptSystem::HotReload(const std::string& graphName)
    {
        std::cout << "[VisualScriptSystem] Hot-reloading '" << graphName << "'..." << std::endl;
        return CompileAndRegister(graphName);
    }

    bool VisualScriptSystem::SaveGraph(const std::string& graphName, const std::string& filePath) const
    {
        auto it = m_graphs.find(graphName);
        if (it == m_graphs.end())
        {
            std::cerr << "[VisualScriptSystem] Cannot save — graph '" << graphName << "' not found." << std::endl;
            return false;
        }

        std::string json = it->second->SerializeToJSON();
        std::ofstream file(filePath);
        if (!file.is_open())
        {
            std::cerr << "[VisualScriptSystem] Cannot open file for writing: " << filePath << std::endl;
            return false;
        }

        file << json;
        file.close();
        std::cout << "[VisualScriptSystem] Saved '" << graphName << "' to " << filePath << std::endl;
        return true;
    }

    bool VisualScriptSystem::LoadGraph(const std::string& filePath)
    {
        std::ifstream file(filePath);
        if (!file.is_open())
        {
            std::cerr << "[VisualScriptSystem] Cannot open file for reading: " << filePath << std::endl;
            return false;
        }

        std::ostringstream buffer;
        buffer << file.rdbuf();
        file.close();

        std::string json = buffer.str();
        auto graph = std::make_unique<VisualScriptGraph>();
        if (!graph->DeserializeFromJSON(json))
        {
            std::cerr << "[VisualScriptSystem] Failed to parse JSON from: " << filePath << std::endl;
            return false;
        }

        std::string name = graph->GetName();
        m_graphs[name] = std::move(graph);
        std::cout << "[VisualScriptSystem] Loaded graph '" << name << "' from " << filePath << std::endl;
        return true;
    }

    std::string VisualScriptSystem::Console_GetStatus() const
    {
        std::ostringstream oss;
        oss << "VisualScriptSystem: " << (m_isInitialized ? "Initialized" : "Not Initialized") << "\n";
        oss << "  Graphs loaded: " << m_graphs.size() << "\n";
        oss << "  Node templates: " << NodeLibrary::GetInstance().GetAllTemplates().size() << "\n";
        return oss.str();
    }

    std::string VisualScriptSystem::Console_ListGraphs() const
    {
        std::ostringstream oss;
        oss << "Visual Script Graphs (" << m_graphs.size() << "):\n";
        for (const auto& [name, graph] : m_graphs)
        {
            oss << "  - " << name << " (" << graph->GetNodes().size() << " nodes, " << graph->GetLinks().size()
                << " links)\n";
        }
        return oss.str();
    }

    std::string VisualScriptSystem::Console_ListNodes() const
    {
        std::ostringstream oss;
        const auto& templates = NodeLibrary::GetInstance().GetAllTemplates();
        oss << "Available Node Types (" << templates.size() << "):\n";

        // Group by category
        std::unordered_map<int, std::vector<const NodeLibrary::NodeTemplate*>> byCategory;
        for (const auto& [name, tmpl] : templates)
        {
            byCategory[static_cast<int>(tmpl.category)].push_back(&tmpl);
        }

        auto CategoryName = [](NodeCategory cat) -> std::string
        {
            switch (cat)
            {
            case NodeCategory::Event:
                return "Event";
            case NodeCategory::FlowControl:
                return "Flow Control";
            case NodeCategory::Math:
                return "Math";
            case NodeCategory::Logic:
                return "Logic";
            case NodeCategory::String:
                return "String";
            case NodeCategory::Variable:
                return "Variable";
            case NodeCategory::Entity:
                return "Entity";
            case NodeCategory::Physics:
                return "Physics";
            case NodeCategory::Input:
                return "Input";
            case NodeCategory::Audio:
                return "Audio";
            case NodeCategory::Debug:
                return "Debug";
            case NodeCategory::Custom:
                return "Custom";
            default:
                return "Unknown";
            }
        };

        for (const auto& [catInt, nodes] : byCategory)
        {
            oss << "  [" << CategoryName(static_cast<NodeCategory>(catInt)) << "]\n";
            for (const auto* node : nodes)
            {
                oss << "    " << node->typeName << " — " << node->displayName << "\n";
            }
        }

        return oss.str();
    }

} // namespace Spark::Scripting
