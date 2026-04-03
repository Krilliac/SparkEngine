/**
 * @file VisualScriptCompiler.cpp
 * @brief Implementation of visual script graph → AngelScript compiler
 */

#include "VisualScriptCompiler.h"

#include <algorithm>
#include <queue>
#include <sstream>
#include <unordered_set>

namespace Spark::Scripting
{

    // ========================================================================
    // Helpers
    // ========================================================================

    std::string VisualScriptCompiler::VarName(uint32_t nodeID, uint32_t pinIndex)
    {
        return "n" + std::to_string(nodeID) + "_out" + std::to_string(pinIndex);
    }

    std::string VisualScriptCompiler::PinTypeString(PinKind kind)
    {
        switch (kind)
        {
        case PinKind::Bool:
            return "bool";
        case PinKind::Int:
            return "int";
        case PinKind::Float:
            return "float";
        case PinKind::String:
            return "string";
        case PinKind::Vector3:
            return "Vector3";
        case PinKind::Entity:
            return "uint";
        default:
            return "float";
        }
    }

    std::string VisualScriptCompiler::DefaultLiteral(const ScriptPin& pin)
    {
        switch (pin.kind)
        {
        case PinKind::Bool:
            return pin.defaultValue[0] != 0.0f ? "true" : "false";
        case PinKind::Int:
            return std::to_string(static_cast<int>(pin.defaultValue[0]));
        case PinKind::Float:
            return std::to_string(pin.defaultValue[0]) + "f";
        case PinKind::String:
            return "\"" + pin.defaultString + "\"";
        case PinKind::Vector3:
            return "Vector3(" + std::to_string(pin.defaultValue[0]) + "f, " + std::to_string(pin.defaultValue[1]) +
                   "f, " + std::to_string(pin.defaultValue[2]) + "f)";
        case PinKind::Entity:
            return std::to_string(static_cast<uint32_t>(pin.defaultValue[0]));
        default:
            return "0.0f";
        }
    }

    const ScriptNode* VisualScriptCompiler::FindNode(const VisualScriptGraph& graph, uint32_t nodeID)
    {
        for (const auto& node : graph.nodes)
        {
            if (node.id == nodeID)
            {
                return &node;
            }
        }
        return nullptr;
    }

    const ScriptConnection* VisualScriptCompiler::FindConnectionToInput(const VisualScriptGraph& graph, uint32_t nodeID,
                                                                        uint32_t pinIndex)
    {
        for (const auto& conn : graph.connections)
        {
            if (conn.toNode == nodeID && conn.toPin == pinIndex)
            {
                return &conn;
            }
        }
        return nullptr;
    }

    bool VisualScriptCompiler::IsEventNode(ScriptNodeType type)
    {
        return type == ScriptNodeType::OnStart || type == ScriptNodeType::OnUpdate ||
               type == ScriptNodeType::OnTriggerEnter || type == ScriptNodeType::OnTriggerExit ||
               type == ScriptNodeType::OnDamaged || type == ScriptNodeType::OnKeyPress ||
               type == ScriptNodeType::OnCollision || type == ScriptNodeType::OnCustomEvent ||
               type == ScriptNodeType::DefineCustomEvent;
    }

    bool VisualScriptCompiler::IsActionNode(ScriptNodeType type)
    {
        auto val = static_cast<uint32_t>(type);
        // Action nodes: flow control (50-53) and setters (150-159)
        return (val >= 50 && val <= 53) || (val >= 150 && val <= 159);
    }

    std::string VisualScriptCompiler::ResolveInput(const ScriptNode& node, uint32_t inputIndex,
                                                   const VisualScriptGraph& graph)
    {
        const auto* conn = FindConnectionToInput(graph, node.id, inputIndex);
        if (conn)
        {
            return VarName(conn->fromNode, conn->fromPin);
        }

        // Use default value from the pin
        if (inputIndex < node.inputs.size())
        {
            return DefaultLiteral(node.inputs[inputIndex]);
        }
        return "0.0f";
    }

    // ========================================================================
    // Topological Sort (data dependencies)
    // ========================================================================

    std::vector<uint32_t> VisualScriptCompiler::TopologicalSortData(const VisualScriptGraph& graph, uint32_t startNode)
    {
        std::unordered_set<uint32_t> visited;
        std::vector<uint32_t> order;

        // BFS in both directions from the start node:
        // Forward (fromNode → toNode): follows execution flow from event to actions
        // Backward (toNode → fromNode): follows data dependencies from consumers to producers
        std::queue<uint32_t> queue;
        queue.push(startNode);
        visited.insert(startNode);

        while (!queue.empty())
        {
            uint32_t current = queue.front();
            queue.pop();

            for (const auto& conn : graph.connections)
            {
                // Forward: find nodes this node connects TO
                if (conn.fromNode == current && visited.find(conn.toNode) == visited.end())
                {
                    visited.insert(conn.toNode);
                    queue.push(conn.toNode);
                }
                // Backward: find nodes that connect TO this node
                if (conn.toNode == current && visited.find(conn.fromNode) == visited.end())
                {
                    visited.insert(conn.fromNode);
                    queue.push(conn.fromNode);
                }
            }
            order.push_back(current);
        }

        // Sort so dependencies come before dependents
        // A node that feeds into another (fromNode in a connection) should come first
        std::sort(order.begin(), order.end(),
                  [&graph](uint32_t a, uint32_t b)
                  {
                      for (const auto& conn : graph.connections)
                      {
                          if (conn.fromNode == a && conn.toNode == b)
                              return true;
                          if (conn.fromNode == b && conn.toNode == a)
                              return false;
                      }
                      return a < b;
                  });

        return order;
    }

    // ========================================================================
    // Code Emission for Individual Nodes
    // ========================================================================

    void VisualScriptCompiler::EmitNode(const ScriptNode& node, const VisualScriptGraph& graph, std::string& code)
    {
        auto input = [&](uint32_t idx) { return ResolveInput(node, idx, graph); };
        auto out = [&](uint32_t idx) { return VarName(node.id, idx); };

        switch (node.type)
        {
        // -- Constants --
        case ScriptNodeType::ConstFloat:
            code += "    float " + out(0) + " = " + DefaultLiteral(node.outputs[0]) + ";\n";
            break;
        case ScriptNodeType::ConstInt:
            code += "    int " + out(0) + " = " + DefaultLiteral(node.outputs[0]) + ";\n";
            break;
        case ScriptNodeType::ConstBool:
            code += "    bool " + out(0) + " = " + DefaultLiteral(node.outputs[0]) + ";\n";
            break;
        case ScriptNodeType::ConstString:
            code += "    string " + out(0) + " = " + DefaultLiteral(node.outputs[0]) + ";\n";
            break;
        case ScriptNodeType::ConstVector3:
            code += "    Vector3 " + out(0) + " = " + DefaultLiteral(node.outputs[0]) + ";\n";
            break;

        // -- Math --
        case ScriptNodeType::Add:
            code += "    float " + out(0) + " = " + input(0) + " + " + input(1) + ";\n";
            break;
        case ScriptNodeType::Subtract:
            code += "    float " + out(0) + " = " + input(0) + " - " + input(1) + ";\n";
            break;
        case ScriptNodeType::Multiply:
            code += "    float " + out(0) + " = " + input(0) + " * " + input(1) + ";\n";
            break;
        case ScriptNodeType::Divide:
            code +=
                "    float " + out(0) + " = (" + input(1) + " != 0.0f) ? " + input(0) + " / " + input(1) + " : 0.0f;\n";
            break;
        case ScriptNodeType::Negate:
            code += "    float " + out(0) + " = -" + input(0) + ";\n";
            break;
        case ScriptNodeType::Abs:
            code += "    float " + out(0) + " = abs(" + input(0) + ");\n";
            break;
        case ScriptNodeType::Lerp:
            code += "    float " + out(0) + " = " + input(0) + " + (" + input(1) + " - " + input(0) + ") * " +
                    input(2) + ";\n";
            break;
        case ScriptNodeType::Clamp:
            code += "    float _v" + std::to_string(node.id) + " = " + input(0) + ";\n";
            code += "    float " + out(0) + " = (_v" + std::to_string(node.id) + " < " + input(1) + ") ? " + input(1) +
                    " : ((_v" + std::to_string(node.id) + " > " + input(2) + ") ? " + input(2) + " : _v" +
                    std::to_string(node.id) + ");\n";
            break;
        case ScriptNodeType::Random:
            code += "    float " + out(0) + " = float(rand()) / float(2147483647);\n";
            break;
        case ScriptNodeType::RandomRange:
            code += "    float " + out(0) + " = " + input(0) + " + float(rand()) / float(2147483647) * (" + input(1) +
                    " - " + input(0) + ");\n";
            break;

        // -- Logic --
        case ScriptNodeType::And:
            code += "    bool " + out(0) + " = " + input(0) + " && " + input(1) + ";\n";
            break;
        case ScriptNodeType::Or:
            code += "    bool " + out(0) + " = " + input(0) + " || " + input(1) + ";\n";
            break;
        case ScriptNodeType::Not:
            code += "    bool " + out(0) + " = !" + input(0) + ";\n";
            break;
        case ScriptNodeType::Equal:
            code += "    bool " + out(0) + " = (" + input(0) + " == " + input(1) + ");\n";
            break;
        case ScriptNodeType::NotEqual:
            code += "    bool " + out(0) + " = (" + input(0) + " != " + input(1) + ");\n";
            break;
        case ScriptNodeType::Greater:
            code += "    bool " + out(0) + " = (" + input(0) + " > " + input(1) + ");\n";
            break;
        case ScriptNodeType::Less:
            code += "    bool " + out(0) + " = (" + input(0) + " < " + input(1) + ");\n";
            break;
        case ScriptNodeType::GreaterEqual:
            code += "    bool " + out(0) + " = (" + input(0) + " >= " + input(1) + ");\n";
            break;
        case ScriptNodeType::LessEqual:
            code += "    bool " + out(0) + " = (" + input(0) + " <= " + input(1) + ");\n";
            break;

        // -- Getters --
        case ScriptNodeType::GetKeyDown:
        {
            std::string key = !node.properties.empty() ? node.properties.begin()->second : "Space";
            code += "    bool " + out(0) + " = getKeyDown(\"" + key + "\");\n";
            break;
        }
        case ScriptNodeType::GetKey:
        {
            std::string key = !node.properties.empty() ? node.properties.begin()->second : "Space";
            code += "    bool " + out(0) + " = getKey(\"" + key + "\");\n";
            break;
        }
        case ScriptNodeType::GetDeltaTime:
            code += "    float " + out(0) + " = dt;\n";
            break;
        case ScriptNodeType::GetSelf:
            code += "    uint " + out(0) + " = selfEntity;\n";
            break;

        // -- Getters --
        case ScriptNodeType::GetPosition:
            code += "    Vector3 " + out(0) + " = getPosition(" + input(0) + ");\n";
            break;
        case ScriptNodeType::GetRotation:
            code += "    Vector3 " + out(0) + " = getRotation(" + input(0) + ");\n";
            break;
        case ScriptNodeType::GetHealth:
            code += "    float " + out(0) + " = getHealth(" + input(0) + ");\n";
            break;
        case ScriptNodeType::GetSpeed:
            code += "    float " + out(0) + " = getSpeed(" + input(0) + ");\n";
            break;
        case ScriptNodeType::GetEntityByName:
        {
            auto it = node.properties.find("name");
            std::string name = (it != node.properties.end()) ? it->second : "Entity";
            code += "    uint " + out(0) + " = getEntityByName(\"" + name + "\");\n";
            break;
        }

        // -- Actions (input[0] is Exec pin, data starts at input[1]) --
        case ScriptNodeType::PrintMessage:
            code += "    print(" + input(1) + ");\n";
            break;
        case ScriptNodeType::SetPosition:
            code += "    setPosition(" + input(1) + ", " + input(2) + ");\n";
            break;
        case ScriptNodeType::SetRotation:
            code += "    setRotation(" + input(1) + ", " + input(2) + ");\n";
            break;
        case ScriptNodeType::SetHealth:
            code += "    setHealth(" + input(1) + ", " + input(2) + ");\n";
            break;
        case ScriptNodeType::ApplyForce:
            code += "    applyForce(" + input(1) + ", " + input(2) + ");\n";
            break;
        case ScriptNodeType::PlaySound:
        {
            auto it = node.properties.find("sound");
            std::string sound = (it != node.properties.end()) ? it->second : "";
            if (sound.empty() && !node.properties.empty())
                sound = node.properties.begin()->second;
            code += "    playSound(selfEntity, \"" + sound + "\");\n";
            break;
        }
        case ScriptNodeType::PlayAnimation:
        {
            auto it = node.properties.find("animation");
            std::string anim = (it != node.properties.end()) ? it->second : "";
            if (anim.empty() && !node.properties.empty())
                anim = node.properties.begin()->second;
            code += "    playAnimation(selfEntity, \"" + anim + "\");\n";
            break;
        }
        case ScriptNodeType::SpawnEntity:
        {
            auto it = node.properties.find("name");
            std::string name = (it != node.properties.end()) ? it->second : "Entity";
            if (name.empty() && !node.properties.empty())
                name = node.properties.begin()->second;
            code += "    uint " + out(0) + " = createEntity(\"" + name + "\");\n";
            break;
        }
        case ScriptNodeType::DestroyEntity:
            code += "    destroyEntity(" + input(1) + ");\n";
            break;
        case ScriptNodeType::FireEvent:
        {
            auto it = node.properties.find("event");
            std::string evt = (it != node.properties.end()) ? it->second : "";
            if (evt.empty() && !node.properties.empty())
                evt = node.properties.begin()->second;
            code += "    fireEvent(\"" + evt + "\");\n";
            break;
        }

        // -- Flow control --
        case ScriptNodeType::Branch:
            // Branch is handled in the main compile loop with true/false path routing
            break;
        case ScriptNodeType::ForLoop:
        {
            std::string start = input(1); // input[0] is Exec
            std::string end = input(2);
            std::string idx = out(1); // out[0] is LoopBody exec, out[1] is Index
            code += "    for (int " + idx + " = " + start + "; " + idx + " < " + end + "; " + idx + "++)\n";
            code += "    {\n";
            // Emit body nodes connected to output pin 0 (LoopBody exec)
            for (const auto& c : graph.connections)
            {
                if (c.fromNode == node.id && c.fromPin == 0)
                {
                    const auto* bodyNode = FindNode(graph, c.toNode);
                    if (bodyNode && !IsEventNode(bodyNode->type))
                    {
                        std::string bodyCode;
                        EmitNode(*bodyNode, graph, bodyCode);
                        code += "    " + bodyCode;
                    }
                }
            }
            code += "    }\n";
            break;
        }
        case ScriptNodeType::Sequence:
            // Emit connected nodes for each execution output in order
            for (uint32_t outIdx = 0; outIdx < static_cast<uint32_t>(node.outputs.size()); outIdx++)
            {
                for (const auto& c : graph.connections)
                {
                    if (c.fromNode == node.id && c.fromPin == outIdx)
                    {
                        const auto* seqNode = FindNode(graph, c.toNode);
                        if (seqNode && !IsEventNode(seqNode->type))
                            EmitNode(*seqNode, graph, code);
                    }
                }
            }
            break;
        case ScriptNodeType::DoNothing:
            break;

        // -- Variables --
        case ScriptNodeType::GetVariable:
        {
            auto it = node.properties.find("name");
            std::string varName = (it != node.properties.end()) ? it->second : "var";
            // Output pin type determines the declared type
            std::string typeStr = "float";
            if (!node.outputs.empty())
                typeStr = PinTypeString(node.outputs[0].kind);
            code += "    " + typeStr + " " + out(0) + " = " + varName + ";\n";
            break;
        }
        case ScriptNodeType::SetVariable:
        {
            auto it = node.properties.find("name");
            std::string varName = (it != node.properties.end()) ? it->second : "var";
            code += "    " + varName + " = " + input(1) + ";\n"; // input[0] is Exec
            break;
        }

        // -- Custom events & functions --
        case ScriptNodeType::CallFunction:
        {
            auto it = node.properties.find("function");
            std::string funcName = (it != node.properties.end()) ? it->second : "myFunction";
            // Pass all data inputs as arguments
            std::string args;
            for (size_t i = 0; i < node.inputs.size(); i++)
            {
                if (node.inputs[i].kind == PinKind::Execution)
                    continue;
                if (!args.empty())
                    args += ", ";
                args += input(static_cast<uint32_t>(i));
            }
            if (!node.outputs.empty() && node.outputs[0].kind != PinKind::Execution)
            {
                std::string retType = PinTypeString(node.outputs[0].kind);
                code += "    " + retType + " " + out(0) + " = " + funcName + "(" + args + ");\n";
            }
            else
            {
                code += "    " + funcName + "(" + args + ");\n";
            }
            break;
        }
        case ScriptNodeType::ReturnValue:
            code += "    return " + input(0) + ";\n";
            break;
        case ScriptNodeType::DefineCustomEvent:
            // DefineCustomEvent is handled as an event entry point in the main compile loop
            break;

        // -- Vector math --
        case ScriptNodeType::Normalize:
            code += "    Vector3 " + out(0) + " = normalize(" + input(0) + ");\n";
            break;
        case ScriptNodeType::DotProduct:
            code += "    float " + out(0) + " = dot(" + input(0) + ", " + input(1) + ");\n";
            break;
        case ScriptNodeType::Distance:
            code += "    float " + out(0) + " = distance(" + input(0) + ", " + input(1) + ");\n";
            break;

        default:
            code += "    // Unhandled node type " + std::to_string(static_cast<uint32_t>(node.type)) + "\n";
            break;
        }
    }

    // ========================================================================
    // Main Compile Entry Point
    // ========================================================================

    ScriptCompileResult VisualScriptCompiler::Compile(const VisualScriptGraph& graph, bool debugMode)
    {
        ScriptCompileResult result;

        if (graph.nodes.empty())
        {
            result.errors.push_back("Empty graph — no nodes to compile");
            return result;
        }

        // Collect event entry-point nodes
        std::vector<const ScriptNode*> eventNodes;
        for (const auto& node : graph.nodes)
        {
            if (IsEventNode(node.type))
            {
                eventNodes.push_back(&node);
            }
        }

        if (eventNodes.empty())
        {
            result.errors.push_back("No event nodes found — add OnStart, OnUpdate, or another event node");
            return result;
        }

        std::ostringstream source;
        source << "// Auto-generated by SparkEngine Visual Script Compiler\n";
        source << "// Class: " << graph.className << "\n\n";
        source << "class " << graph.className << "\n{\n";
        source << "    uint selfEntity = 0; // Entity this script is attached to\n";

        // Emit member variables
        for (const auto& var : graph.variables)
        {
            source << "    " << PinTypeString(var.type) << " " << var.name;
            if (!var.defaultValue.empty())
            {
                source << " = " << var.defaultValue;
            }
            source << ";\n";
        }
        if (!graph.variables.empty())
        {
            source << "\n";
        }

        // Emit methods for each event node
        for (const auto* eventNode : eventNodes)
        {
            // Determine method signature from event type
            std::string methodName;
            std::string params;

            switch (eventNode->type)
            {
            case ScriptNodeType::OnStart:
                methodName = "Start";
                break;
            case ScriptNodeType::OnUpdate:
                methodName = "Update";
                params = "float dt";
                break;
            case ScriptNodeType::OnCollision:
                methodName = "OnCollision";
                params = "uint other";
                break;
            case ScriptNodeType::OnTriggerEnter:
                methodName = "OnTriggerEnter";
                params = "uint triggerId";
                break;
            case ScriptNodeType::OnTriggerExit:
                methodName = "OnTriggerExit";
                params = "uint triggerId";
                break;
            case ScriptNodeType::OnDamaged:
                methodName = "OnDamaged";
                params = "float amount";
                break;
            case ScriptNodeType::OnKeyPress:
            {
                auto it = eventNode->properties.find("key");
                std::string key = (it != eventNode->properties.end()) ? it->second : "Space";
                methodName = "Update"; // Key checks go in Update
                params = "float dt";
                break;
            }
            default:
                methodName = "CustomHandler";
                break;
            }

            source << "    void " << methodName << "(" << params << ")\n    {\n";

            // Collect all nodes reachable from this event via execution connections
            auto sortedNodes = TopologicalSortData(graph, eventNode->id);

            // For OnKeyPress, wrap in a key check
            if (eventNode->type == ScriptNodeType::OnKeyPress)
            {
                auto it = eventNode->properties.find("key");
                std::string key = (it != eventNode->properties.end()) ? it->second : "Space";
                source << "        if (getKeyDown(\"" << key << "\"))\n        {\n";
            }

            // Collect nodes that are emitted inside control flow blocks (Branch/ForLoop/Sequence)
            // to prevent double-emission in the main loop
            std::unordered_set<uint32_t> controlFlowChildren;
            for (const auto& conn : graph.connections)
            {
                const auto* fromNode = FindNode(graph, conn.fromNode);
                if (fromNode &&
                    (fromNode->type == ScriptNodeType::Branch || fromNode->type == ScriptNodeType::ForLoop ||
                     fromNode->type == ScriptNodeType::Sequence))
                {
                    controlFlowChildren.insert(conn.toNode);
                }
            }

            // Emit code for each node in dependency order
            std::string bodyCode;
            for (uint32_t nodeId : sortedNodes)
            {
                if (nodeId == eventNode->id)
                    continue;
                const auto* node = FindNode(graph, nodeId);
                if (!node || IsEventNode(node->type))
                    continue;
                // Skip nodes that are children of control flow — they get emitted inside the block
                if (controlFlowChildren.count(nodeId) && node->type != ScriptNodeType::Branch &&
                    node->type != ScriptNodeType::ForLoop && node->type != ScriptNodeType::Sequence)
                    continue;

                // Debug trace instrumentation
                if (debugMode)
                {
                    bodyCode += "    debugTrace(" + std::to_string(node->id) + ", \"" +
                                std::to_string(static_cast<uint32_t>(node->type)) + "\", \"executing\");\n";
                }

                // Handle Branch nodes specially — emit if/else with true/false paths
                if (node->type == ScriptNodeType::Branch)
                {
                    std::string condition = ResolveInput(*node, 1, graph); // input[0] is Exec, [1] is Bool
                    bodyCode += "    if (" + condition + ")\n    {\n";

                    // Walk an execution chain from a given output pin
                    auto emitExecChain = [&](uint32_t fromNodeId, uint32_t fromPinIdx)
                    {
                        std::string chainCode;
                        uint32_t currentNode = 0;

                        // Find first connected node
                        for (const auto& c : graph.connections)
                        {
                            if (c.fromNode == fromNodeId && c.fromPin == fromPinIdx)
                            {
                                currentNode = c.toNode;
                                break;
                            }
                        }
                        if (currentNode == 0)
                            return chainCode;

                        // Walk the chain following execution connections
                        std::unordered_set<uint32_t> emittedInChain;
                        while (currentNode != 0 && emittedInChain.find(currentNode) == emittedInChain.end())
                        {
                            const auto* chainNode = FindNode(graph, currentNode);
                            if (!chainNode || IsEventNode(chainNode->type))
                                break;

                            emittedInChain.insert(currentNode);
                            EmitNode(*chainNode, graph, chainCode);

                            // Find next node connected via first execution output
                            uint32_t nextNode = 0;
                            for (const auto& c : graph.connections)
                            {
                                if (c.fromNode == currentNode)
                                {
                                    // Check if this is an execution output
                                    if (c.fromPin < chainNode->outputs.size() &&
                                        chainNode->outputs[c.fromPin].kind == PinKind::Execution)
                                    {
                                        nextNode = c.toNode;
                                        break;
                                    }
                                }
                            }
                            currentNode = nextNode;
                        }
                        return chainCode;
                    };

                    bodyCode += emitExecChain(node->id, 0); // True branch (output pin 0)
                    bodyCode += "    }\n    else\n    {\n";
                    bodyCode += emitExecChain(node->id, 1); // False branch (output pin 1)
                    bodyCode += "    }\n";
                }
                else
                {
                    EmitNode(*node, graph, bodyCode);
                }
            }

            // Indent body code
            std::istringstream bodyStream(bodyCode);
            std::string line;
            while (std::getline(bodyStream, line))
            {
                if (eventNode->type == ScriptNodeType::OnKeyPress)
                {
                    source << "        " << line << "\n";
                }
                else
                {
                    source << "    " << line << "\n";
                }
            }

            if (eventNode->type == ScriptNodeType::OnKeyPress)
            {
                source << "        }\n";
            }

            source << "    }\n\n";
        }

        // Emit reusable function methods
        for (const auto& func : graph.functions)
        {
            std::string retTypeStr = PinTypeString(func.returnType);
            if (func.returnType == PinKind::Execution)
                retTypeStr = "void";

            std::string paramStr;
            for (size_t i = 0; i < func.parameters.size(); i++)
            {
                if (i > 0)
                    paramStr += ", ";
                paramStr += PinTypeString(func.parameters[i].type) + " " + func.parameters[i].name;
            }

            source << "    " << retTypeStr << " " << func.name << "(" << paramStr << ")\n    {\n";

            // Build a mini-graph for this function and emit its nodes
            VisualScriptGraph funcGraph;
            funcGraph.nodes = func.nodes;
            funcGraph.connections = func.connections;

            for (const auto& funcNode : func.nodes)
            {
                if (IsEventNode(funcNode.type))
                    continue;

                std::string funcCode;
                EmitNode(funcNode, funcGraph, funcCode);
                std::istringstream funcStream(funcCode);
                std::string funcLine;
                while (std::getline(funcStream, funcLine))
                    source << "    " << funcLine << "\n";
            }

            source << "    }\n\n";
        }

        // Emit custom event handler stubs
        for (const auto& evt : graph.customEvents)
        {
            std::string paramStr;
            for (size_t i = 0; i < evt.parameters.size(); i++)
            {
                if (i > 0)
                    paramStr += ", ";
                paramStr += PinTypeString(evt.parameters[i].type) + " " + evt.parameters[i].name;
            }
            source << "    void On" << evt.name << "(" << paramStr << ")\n    {\n";
            source << "        // Custom event handler — connected nodes execute here\n";
            source << "    }\n\n";
        }

        source << "}\n";

        result.angelScriptSource = source.str();
        result.success = true;
        return result;
    }

} // namespace Spark::Scripting
