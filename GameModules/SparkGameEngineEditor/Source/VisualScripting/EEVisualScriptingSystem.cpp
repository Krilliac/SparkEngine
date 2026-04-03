/**
 * @file EEVisualScriptingSystem.cpp
 * @brief Node-based visual scripting for no-code gameplay logic
 *
 * Implements the visual scripting graph system with built-in node templates,
 * pin-type validation, and graph compilation.
 */

#include "EEVisualScriptingSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#include <algorithm>

namespace EngineEditor
{

    bool EEVisualScriptingSystem::Initialize(Spark::IEngineContext* context)
    {
        if (!context)
            return false;

        m_context = context;
        RegisterBuiltinNodes();

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Visual scripting system initialized (%zu node templates)",
                       m_nodeTemplates.size());
        return true;
    }

    void EEVisualScriptingSystem::Update([[maybe_unused]] float deltaTime)
    {
        if (!m_initialized)
            return;

        // Execute compiled graphs attached to active entities
        for (auto& graph : m_graphs)
        {
            if (!graph.isCompiled || graph.hasErrors)
                continue;
            // Runtime execution would dispatch through compiled bytecode here
        }
    }

    void EEVisualScriptingSystem::Shutdown()
    {
        m_graphs.clear();
        m_nodeTemplates.clear();
        m_initialized = false;
    }

    void EEVisualScriptingSystem::RenderDebugUI()
    {
        // ImGui rendering handled by editor panels
    }

    uint32_t EEVisualScriptingSystem::CreateGraph(const std::string& name)
    {
        ScriptGraph graph;
        graph.graphId = m_nextGraphId++;
        graph.name = name;

        // Add default event node (BeginPlay)
        ScriptNode beginPlay;
        beginPlay.nodeId = m_nextNodeId++;
        beginPlay.name = "BeginPlay";
        beginPlay.category = NodeCategory::Event;
        beginPlay.posX = 100.0f;
        beginPlay.posY = 200.0f;

        ScriptPin execOut;
        execOut.pinId = m_nextPinId++;
        execOut.name = "Exec";
        execOut.type = PinType::Exec;
        execOut.isInput = false;
        beginPlay.outputs.push_back(execOut);

        graph.nodes.push_back(beginPlay);

        // Add default Tick event node
        ScriptNode tick;
        tick.nodeId = m_nextNodeId++;
        tick.name = "Tick";
        tick.category = NodeCategory::Event;
        tick.posX = 100.0f;
        tick.posY = 400.0f;

        ScriptPin tickExecOut;
        tickExecOut.pinId = m_nextPinId++;
        tickExecOut.name = "Exec";
        tickExecOut.type = PinType::Exec;
        tickExecOut.isInput = false;
        tick.outputs.push_back(tickExecOut);

        ScriptPin deltaTimeOut;
        deltaTimeOut.pinId = m_nextPinId++;
        deltaTimeOut.name = "DeltaTime";
        deltaTimeOut.type = PinType::Float;
        deltaTimeOut.isInput = false;
        tick.outputs.push_back(deltaTimeOut);

        graph.nodes.push_back(tick);

        m_graphs.push_back(std::move(graph));
        return m_graphs.back().graphId;
    }

    bool EEVisualScriptingSystem::DeleteGraph(uint32_t graphId)
    {
        auto it = std::find_if(m_graphs.begin(), m_graphs.end(),
                               [graphId](const ScriptGraph& g) { return g.graphId == graphId; });
        if (it == m_graphs.end())
            return false;
        m_graphs.erase(it);
        return true;
    }

    bool EEVisualScriptingSystem::CompileGraph(uint32_t graphId)
    {
        auto it = std::find_if(m_graphs.begin(), m_graphs.end(),
                               [graphId](const ScriptGraph& g) { return g.graphId == graphId; });
        if (it == m_graphs.end())
            return false;

        it->hasErrors = false;

        // Validate all connections
        for (const auto& conn : it->connections)
        {
            if (!ValidateConnection(*it, conn.fromNodeId, conn.fromPinId, conn.toNodeId, conn.toPinId))
            {
                it->hasErrors = true;
                return false;
            }
        }

        it->isCompiled = true;
        return true;
    }

    bool EEVisualScriptingSystem::CompileAll()
    {
        bool allOk = true;
        for (auto& graph : m_graphs)
        {
            if (!CompileGraph(graph.graphId))
                allOk = false;
        }
        return allOk;
    }

    uint32_t EEVisualScriptingSystem::AddNode(uint32_t graphId, NodeCategory category, const std::string& name)
    {
        auto it = std::find_if(m_graphs.begin(), m_graphs.end(),
                               [graphId](const ScriptGraph& g) { return g.graphId == graphId; });
        if (it == m_graphs.end())
            return 0;

        // Find template
        const ScriptNode* tmpl = nullptr;
        for (const auto& t : m_nodeTemplates)
        {
            if (t.category == category && t.name == name)
            {
                tmpl = &t;
                break;
            }
        }

        ScriptNode node;
        node.nodeId = m_nextNodeId++;
        node.name = tmpl ? tmpl->name : name;
        node.category = category;
        node.posX = 300.0f;
        node.posY = 200.0f;

        if (tmpl)
        {
            node.description = tmpl->description;
            node.isPure = tmpl->isPure;
            // Copy pin templates with new IDs
            for (auto pin : tmpl->inputs)
            {
                pin.pinId = m_nextPinId++;
                node.inputs.push_back(pin);
            }
            for (auto pin : tmpl->outputs)
            {
                pin.pinId = m_nextPinId++;
                node.outputs.push_back(pin);
            }
        }

        it->nodes.push_back(std::move(node));
        it->isCompiled = false;
        return it->nodes.back().nodeId;
    }

    bool EEVisualScriptingSystem::RemoveNode(uint32_t graphId, uint32_t nodeId)
    {
        auto git = std::find_if(m_graphs.begin(), m_graphs.end(),
                                [graphId](const ScriptGraph& g) { return g.graphId == graphId; });
        if (git == m_graphs.end())
            return false;

        auto nit = std::find_if(git->nodes.begin(), git->nodes.end(),
                                [nodeId](const ScriptNode& n) { return n.nodeId == nodeId; });
        if (nit == git->nodes.end())
            return false;

        // Remove connections involving this node
        std::erase_if(git->connections,
                      [nodeId](const ScriptConnection& c) { return c.fromNodeId == nodeId || c.toNodeId == nodeId; });

        git->nodes.erase(nit);
        git->isCompiled = false;
        return true;
    }

    bool EEVisualScriptingSystem::ConnectPins(uint32_t graphId, uint32_t fromNode, uint32_t fromPin, uint32_t toNode,
                                              uint32_t toPin)
    {
        auto git = std::find_if(m_graphs.begin(), m_graphs.end(),
                                [graphId](const ScriptGraph& g) { return g.graphId == graphId; });
        if (git == m_graphs.end())
            return false;

        if (!ValidateConnection(*git, fromNode, fromPin, toNode, toPin))
            return false;

        ScriptConnection conn;
        conn.connectionId = m_nextConnectionId++;
        conn.fromNodeId = fromNode;
        conn.fromPinId = fromPin;
        conn.toNodeId = toNode;
        conn.toPinId = toPin;
        git->connections.push_back(conn);
        git->isCompiled = false;
        return true;
    }

    bool EEVisualScriptingSystem::DisconnectPin(uint32_t graphId, uint32_t connectionId)
    {
        auto git = std::find_if(m_graphs.begin(), m_graphs.end(),
                                [graphId](const ScriptGraph& g) { return g.graphId == graphId; });
        if (git == m_graphs.end())
            return false;

        auto cit = std::find_if(git->connections.begin(), git->connections.end(),
                                [connectionId](const ScriptConnection& c) { return c.connectionId == connectionId; });
        if (cit == git->connections.end())
            return false;

        git->connections.erase(cit);
        git->isCompiled = false;
        return true;
    }

    uint32_t EEVisualScriptingSystem::AddVariable(uint32_t graphId, const std::string& name, PinType type)
    {
        auto git = std::find_if(m_graphs.begin(), m_graphs.end(),
                                [graphId](const ScriptGraph& g) { return g.graphId == graphId; });
        if (git == m_graphs.end())
            return 0;

        ScriptVariable var;
        var.variableId = m_nextVariableId++;
        var.name = name;
        var.type = type;
        git->variables.push_back(var);
        return var.variableId;
    }

    const ScriptGraph* EEVisualScriptingSystem::GetGraph(uint32_t graphId) const
    {
        for (const auto& g : m_graphs)
        {
            if (g.graphId == graphId)
                return &g;
        }
        return nullptr;
    }

    std::string EEVisualScriptingSystem::GetGraphListString() const
    {
        std::string s = "=== Visual Script Graphs ===\n";
        for (const auto& g : m_graphs)
        {
            s += "  [" + std::to_string(g.graphId) + "] " + g.name;
            s += " (" + std::to_string(g.nodes.size()) + " nodes, " + std::to_string(g.connections.size()) +
                 " connections";
            if (g.isCompiled)
                s += ", compiled";
            if (g.hasErrors)
                s += ", ERRORS";
            s += ")\n";
        }
        if (m_graphs.empty())
            s += "  (none)\n";
        return s;
    }

    std::string EEVisualScriptingSystem::GetNodeCatalogString() const
    {
        std::string s = "=== Node Catalog ===\n";
        NodeCategory lastCat = NodeCategory::Count;
        for (const auto& n : m_nodeTemplates)
        {
            if (n.category != lastCat)
            {
                lastCat = n.category;
                s += " [" + std::to_string(static_cast<int>(n.category)) + "] ---\n";
            }
            s += "  " + n.name;
            if (n.isPure)
                s += " (pure)";
            s += "\n";
        }
        return s;
    }

    std::string EEVisualScriptingSystem::GetGraphDetailString(uint32_t graphId) const
    {
        const auto* g = GetGraph(graphId);
        if (!g)
            return "Graph not found";

        std::string s = "=== Graph: " + g->name + " ===\n";
        s += "Nodes: " + std::to_string(g->nodes.size()) + "\n";
        for (const auto& n : g->nodes)
        {
            s += "  [" + std::to_string(n.nodeId) + "] " + n.name;
            s += " (in:" + std::to_string(n.inputs.size()) + " out:" + std::to_string(n.outputs.size()) + ")\n";
        }
        s += "Connections: " + std::to_string(g->connections.size()) + "\n";
        s += "Variables: " + std::to_string(g->variables.size()) + "\n";
        for (const auto& v : g->variables)
            s += "  " + v.name + " (" + std::to_string(static_cast<int>(v.type)) + ")\n";
        return s;
    }

    // -------------------------------------------------------------------------
    // Built-in node registration
    // -------------------------------------------------------------------------

    void EEVisualScriptingSystem::RegisterBuiltinNodes()
    {
        RegisterEventNodes();
        RegisterFlowNodes();
        RegisterMathNodes();
        RegisterTransformNodes();
        RegisterPhysicsNodes();
    }

    void EEVisualScriptingSystem::RegisterEventNodes()
    {
        auto addEvent = [this](const std::string& name, const std::string& desc, std::vector<ScriptPin> outputs)
        {
            ScriptNode n;
            n.nodeId = 0; // template
            n.name = name;
            n.description = desc;
            n.category = NodeCategory::Event;
            n.outputs = std::move(outputs);
            m_nodeTemplates.push_back(std::move(n));
        };

        addEvent("BeginPlay", "Called once when entity spawns", {{0, "Exec", PinType::Exec, false, false}});
        addEvent("Tick", "Called every frame",
                 {{0, "Exec", PinType::Exec, false, false}, {0, "DeltaTime", PinType::Float, false, false}});
        addEvent("OnCollision", "Called on physics collision",
                 {{0, "Exec", PinType::Exec, false, false},
                  {0, "OtherEntity", PinType::Entity, false, false},
                  {0, "ImpactPoint", PinType::Vector3, false, false}});
        addEvent("OnOverlap", "Called when overlap begins",
                 {{0, "Exec", PinType::Exec, false, false}, {0, "OtherEntity", PinType::Entity, false, false}});
        addEvent("OnInput", "Called on input action",
                 {{0, "Exec", PinType::Exec, false, false},
                  {0, "ActionName", PinType::String, false, false},
                  {0, "Value", PinType::Float, false, false}});
        addEvent("OnDestroy", "Called when entity is destroyed", {{0, "Exec", PinType::Exec, false, false}});
    }

    void EEVisualScriptingSystem::RegisterFlowNodes()
    {
        auto addFlow = [this](const std::string& name, const std::string& desc, std::vector<ScriptPin> inputs,
                              std::vector<ScriptPin> outputs)
        {
            ScriptNode n;
            n.name = name;
            n.description = desc;
            n.category = NodeCategory::Flow;
            n.inputs = std::move(inputs);
            n.outputs = std::move(outputs);
            m_nodeTemplates.push_back(std::move(n));
        };

        addFlow("Branch", "If/else conditional",
                {{0, "Exec", PinType::Exec, true}, {0, "Condition", PinType::Bool, true}},
                {{0, "True", PinType::Exec, false}, {0, "False", PinType::Exec, false}});
        addFlow("Sequence", "Execute multiple outputs in order", {{0, "Exec", PinType::Exec, true}},
                {{0, "Then 0", PinType::Exec, false},
                 {0, "Then 1", PinType::Exec, false},
                 {0, "Then 2", PinType::Exec, false}});
        addFlow("ForLoop", "Loop from start to end index",
                {{0, "Exec", PinType::Exec, true}, {0, "Start", PinType::Int, true}, {0, "End", PinType::Int, true}},
                {{0, "LoopBody", PinType::Exec, false},
                 {0, "Index", PinType::Int, false},
                 {0, "Completed", PinType::Exec, false}});
        addFlow("WhileLoop", "Loop while condition is true",
                {{0, "Exec", PinType::Exec, true}, {0, "Condition", PinType::Bool, true}},
                {{0, "LoopBody", PinType::Exec, false}, {0, "Completed", PinType::Exec, false}});
        addFlow("Delay", "Wait for specified seconds",
                {{0, "Exec", PinType::Exec, true}, {0, "Duration", PinType::Float, true}},
                {{0, "Completed", PinType::Exec, false}});
    }

    void EEVisualScriptingSystem::RegisterMathNodes()
    {
        auto addPure = [this](const std::string& name, const std::string& desc, std::vector<ScriptPin> inputs,
                              std::vector<ScriptPin> outputs)
        {
            ScriptNode n;
            n.name = name;
            n.description = desc;
            n.category = NodeCategory::Math;
            n.isPure = true;
            n.inputs = std::move(inputs);
            n.outputs = std::move(outputs);
            m_nodeTemplates.push_back(std::move(n));
        };

        addPure("Add", "A + B", {{0, "A", PinType::Float, true}, {0, "B", PinType::Float, true}},
                {{0, "Result", PinType::Float, false}});
        addPure("Subtract", "A - B", {{0, "A", PinType::Float, true}, {0, "B", PinType::Float, true}},
                {{0, "Result", PinType::Float, false}});
        addPure("Multiply", "A * B", {{0, "A", PinType::Float, true}, {0, "B", PinType::Float, true}},
                {{0, "Result", PinType::Float, false}});
        addPure("Divide", "A / B", {{0, "A", PinType::Float, true}, {0, "B", PinType::Float, true}},
                {{0, "Result", PinType::Float, false}});
        addPure("Lerp", "Linear interpolation",
                {{0, "A", PinType::Float, true}, {0, "B", PinType::Float, true}, {0, "Alpha", PinType::Float, true}},
                {{0, "Result", PinType::Float, false}});
        addPure(
            "Clamp", "Clamp value to range",
            {{0, "Value", PinType::Float, true}, {0, "Min", PinType::Float, true}, {0, "Max", PinType::Float, true}},
            {{0, "Result", PinType::Float, false}});
        addPure("Abs", "Absolute value", {{0, "A", PinType::Float, true}}, {{0, "Result", PinType::Float, false}});
        addPure("Sin", "Sine (radians)", {{0, "A", PinType::Float, true}}, {{0, "Result", PinType::Float, false}});
        addPure("Cos", "Cosine (radians)", {{0, "A", PinType::Float, true}}, {{0, "Result", PinType::Float, false}});
        addPure("RandomFloat", "Random float in range",
                {{0, "Min", PinType::Float, true}, {0, "Max", PinType::Float, true}},
                {{0, "Result", PinType::Float, false}});
    }

    void EEVisualScriptingSystem::RegisterTransformNodes()
    {
        // GetPosition (pure)
        {
            ScriptNode n;
            n.name = "GetPosition";
            n.description = "Get entity world position";
            n.category = NodeCategory::Transform;
            n.isPure = true;
            n.inputs = {{0, "Entity", PinType::Entity, true}};
            n.outputs = {{0, "Position", PinType::Vector3, false}};
            m_nodeTemplates.push_back(std::move(n));
        }
        // SetPosition
        {
            ScriptNode n;
            n.name = "SetPosition";
            n.description = "Set entity world position";
            n.category = NodeCategory::Transform;
            n.inputs = {{0, "Exec", PinType::Exec, true},
                        {0, "Entity", PinType::Entity, true},
                        {0, "Position", PinType::Vector3, true}};
            n.outputs = {{0, "Exec", PinType::Exec, false}};
            m_nodeTemplates.push_back(std::move(n));
        }
        // GetRotation (pure)
        {
            ScriptNode n;
            n.name = "GetRotation";
            n.description = "Get entity world rotation";
            n.category = NodeCategory::Transform;
            n.isPure = true;
            n.inputs = {{0, "Entity", PinType::Entity, true}};
            n.outputs = {{0, "Rotation", PinType::Rotator, false}};
            m_nodeTemplates.push_back(std::move(n));
        }
        // LookAt
        {
            ScriptNode n;
            n.name = "LookAt";
            n.description = "Rotate entity to face target";
            n.category = NodeCategory::Transform;
            n.inputs = {{0, "Exec", PinType::Exec, true},
                        {0, "Entity", PinType::Entity, true},
                        {0, "Target", PinType::Vector3, true}};
            n.outputs = {{0, "Exec", PinType::Exec, false}};
            m_nodeTemplates.push_back(std::move(n));
        }
        // MoveToward
        {
            ScriptNode n;
            n.name = "MoveToward";
            n.description = "Move entity toward target at speed";
            n.category = NodeCategory::Transform;
            n.inputs = {{0, "Exec", PinType::Exec, true},
                        {0, "Entity", PinType::Entity, true},
                        {0, "Target", PinType::Vector3, true},
                        {0, "Speed", PinType::Float, true}};
            n.outputs = {{0, "Exec", PinType::Exec, false}, {0, "Reached", PinType::Bool, false}};
            m_nodeTemplates.push_back(std::move(n));
        }
    }

    void EEVisualScriptingSystem::RegisterPhysicsNodes()
    {
        // Raycast
        {
            ScriptNode n;
            n.name = "Raycast";
            n.description = "Cast ray and return hit info";
            n.category = NodeCategory::Physics;
            n.inputs = {{0, "Exec", PinType::Exec, true},
                        {0, "Origin", PinType::Vector3, true},
                        {0, "Direction", PinType::Vector3, true},
                        {0, "MaxDistance", PinType::Float, true}};
            n.outputs = {{0, "Exec", PinType::Exec, false},
                         {0, "DidHit", PinType::Bool, false},
                         {0, "HitEntity", PinType::Entity, false},
                         {0, "HitPoint", PinType::Vector3, false}};
            m_nodeTemplates.push_back(std::move(n));
        }
        // AddForce
        {
            ScriptNode n;
            n.name = "AddForce";
            n.description = "Apply physics force to entity";
            n.category = NodeCategory::Physics;
            n.inputs = {{0, "Exec", PinType::Exec, true},
                        {0, "Entity", PinType::Entity, true},
                        {0, "Force", PinType::Vector3, true}};
            n.outputs = {{0, "Exec", PinType::Exec, false}};
            m_nodeTemplates.push_back(std::move(n));
        }
        // SetVelocity
        {
            ScriptNode n;
            n.name = "SetVelocity";
            n.description = "Set entity linear velocity";
            n.category = NodeCategory::Physics;
            n.inputs = {{0, "Exec", PinType::Exec, true},
                        {0, "Entity", PinType::Entity, true},
                        {0, "Velocity", PinType::Vector3, true}};
            n.outputs = {{0, "Exec", PinType::Exec, false}};
            m_nodeTemplates.push_back(std::move(n));
        }
    }

    bool EEVisualScriptingSystem::ValidateConnection(const ScriptGraph& graph, uint32_t fromNode, uint32_t fromPin,
                                                     uint32_t toNode, uint32_t toPin) const
    {
        if (fromNode == toNode)
            return false;

        const ScriptNode* srcNode = nullptr;
        const ScriptNode* dstNode = nullptr;
        for (const auto& n : graph.nodes)
        {
            if (n.nodeId == fromNode)
                srcNode = &n;
            if (n.nodeId == toNode)
                dstNode = &n;
        }
        if (!srcNode || !dstNode)
            return false;

        const ScriptPin* srcPin = nullptr;
        const ScriptPin* dstPin = nullptr;
        for (const auto& p : srcNode->outputs)
        {
            if (p.pinId == fromPin)
                srcPin = &p;
        }
        for (const auto& p : dstNode->inputs)
        {
            if (p.pinId == toPin)
                dstPin = &p;
        }
        if (!srcPin || !dstPin)
            return false;

        // Type compatibility: exec-to-exec or matching data types (or wildcard)
        if (srcPin->type == PinType::Exec && dstPin->type == PinType::Exec)
            return true;
        if (srcPin->type == PinType::Exec || dstPin->type == PinType::Exec)
            return false;
        if (srcPin->type == PinType::Wildcard || dstPin->type == PinType::Wildcard)
            return true;
        return srcPin->type == dstPin->type;
    }

} // namespace EngineEditor
