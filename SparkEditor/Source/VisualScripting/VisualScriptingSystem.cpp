/**
 * @file VisualScriptingSystem.cpp
 * @brief Implementation of VisualScriptingSystem, ScriptGraph, ScriptExecutionContext, ScriptExecutor
 */

#include "VisualScriptingSystem.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <chrono>

using namespace DirectX;
namespace SparkEditor {

// ============================================================================
// Concrete node implementations
// ============================================================================

namespace {

// Helper to get a float from a ScriptValue (handles int/float/bool)
static float ToFloat(const ScriptValue& v) {
    if (auto* f = std::get_if<float>(&v)) return *f;
    if (auto* i = std::get_if<int>(&v)) return static_cast<float>(*i);
    if (auto* b = std::get_if<bool>(&v)) return *b ? 1.0f : 0.0f;
    return 0.0f;
}

static int ToInt(const ScriptValue& v) {
    if (auto* i = std::get_if<int>(&v)) return *i;
    if (auto* f = std::get_if<float>(&v)) return static_cast<int>(*f);
    if (auto* b = std::get_if<bool>(&v)) return *b ? 1 : 0;
    return 0;
}

static bool ToBool(const ScriptValue& v) {
    if (auto* b = std::get_if<bool>(&v)) return *b;
    if (auto* i = std::get_if<int>(&v)) return *i != 0;
    if (auto* f = std::get_if<float>(&v)) return *f != 0.0f;
    if (auto* s = std::get_if<std::string>(&v)) return !s->empty();
    return false;
}

static std::string ToString(const ScriptValue& v) {
    if (auto* s = std::get_if<std::string>(&v)) return *s;
    if (auto* f = std::get_if<float>(&v)) return std::to_string(*f);
    if (auto* i = std::get_if<int>(&v)) return std::to_string(*i);
    if (auto* b = std::get_if<bool>(&v)) return *b ? "true" : "false";
    return "";
}

// --- Event nodes ---
struct EventStartNode : ScriptNode {
    EventStartNode() {
        type = ScriptNodeType::EVENT_START;
        category = ScriptNodeCategory::EVENT;
        name = "Event Start";
        description = "Called when script begins execution";
        outputSockets.push_back({"Exec", ScriptVariableType::EXECUTION, false, true, false, "Execution output"});
    }
    bool Execute(const std::vector<ScriptValue>&, std::vector<ScriptValue>& outputs, ScriptExecutionContext*) override {
        outputs.resize(1);
        outputs[0] = true; // signal execution flow
        return true;
    }
};

struct EventUpdateNode : ScriptNode {
    EventUpdateNode() {
        type = ScriptNodeType::EVENT_UPDATE;
        category = ScriptNodeCategory::EVENT;
        name = "Event Update";
        description = "Called every frame";
        outputSockets.push_back({"Exec", ScriptVariableType::EXECUTION, false, true, false, "Execution output"});
        outputSockets.push_back({"Delta Time", ScriptVariableType::FLOAT, false, false, 0.0f, "Frame delta time"});
    }
    bool Execute(const std::vector<ScriptValue>&, std::vector<ScriptValue>& outputs, ScriptExecutionContext* ctx) override {
        outputs.resize(2);
        outputs[0] = true;
        outputs[1] = ctx ? ctx->GetDeltaTime() : 0.016f;
        return true;
    }
};

// --- Math nodes ---
struct MathBinaryNode : ScriptNode {
    enum Op { ADD, SUB, MUL, DIV, POW };
    Op op;
    MathBinaryNode(Op o, const std::string& opName, ScriptNodeType t) : op(o) {
        type = t;
        category = ScriptNodeCategory::MATH;
        name = opName;
        inputSockets.push_back({"A", ScriptVariableType::FLOAT, true, false, 0.0f, "First operand"});
        inputSockets.push_back({"B", ScriptVariableType::FLOAT, true, false, 0.0f, "Second operand"});
        outputSockets.push_back({"Result", ScriptVariableType::FLOAT, false, false, 0.0f, "Result"});
    }
    bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs, ScriptExecutionContext*) override {
        float a = inputs.size() > 0 ? ToFloat(inputs[0]) : 0.0f;
        float b = inputs.size() > 1 ? ToFloat(inputs[1]) : 0.0f;
        float result = 0.0f;
        switch (op) {
            case ADD: result = a + b; break;
            case SUB: result = a - b; break;
            case MUL: result = a * b; break;
            case DIV: result = (b != 0.0f) ? a / b : 0.0f; break;
            case POW: result = std::pow(a, b); break;
        }
        outputs.resize(1);
        outputs[0] = result;
        return true;
    }
};

struct MathUnaryNode : ScriptNode {
    enum Fn { SQRT, SIN, COS, TAN };
    Fn fn;
    MathUnaryNode(Fn f, const std::string& fnName, ScriptNodeType t) : fn(f) {
        type = t;
        category = ScriptNodeCategory::MATH;
        name = fnName;
        inputSockets.push_back({"Value", ScriptVariableType::FLOAT, true, false, 0.0f, "Input value"});
        outputSockets.push_back({"Result", ScriptVariableType::FLOAT, false, false, 0.0f, "Result"});
    }
    bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs, ScriptExecutionContext*) override {
        float v = inputs.size() > 0 ? ToFloat(inputs[0]) : 0.0f;
        float result = 0.0f;
        switch (fn) {
            case SQRT: result = std::sqrt(std::abs(v)); break;
            case SIN:  result = std::sin(v); break;
            case COS:  result = std::cos(v); break;
            case TAN:  result = std::tan(v); break;
        }
        outputs.resize(1);
        outputs[0] = result;
        return true;
    }
};

struct ClampNode : ScriptNode {
    ClampNode() {
        type = ScriptNodeType::CLAMP;
        category = ScriptNodeCategory::MATH;
        name = "Clamp";
        inputSockets.push_back({"Value", ScriptVariableType::FLOAT, true, false, 0.0f, "Value to clamp"});
        inputSockets.push_back({"Min", ScriptVariableType::FLOAT, true, false, 0.0f, "Minimum"});
        inputSockets.push_back({"Max", ScriptVariableType::FLOAT, true, false, 1.0f, "Maximum"});
        outputSockets.push_back({"Result", ScriptVariableType::FLOAT, false, false, 0.0f, "Clamped value"});
    }
    bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs, ScriptExecutionContext*) override {
        float v = inputs.size() > 0 ? ToFloat(inputs[0]) : 0.0f;
        float mn = inputs.size() > 1 ? ToFloat(inputs[1]) : 0.0f;
        float mx = inputs.size() > 2 ? ToFloat(inputs[2]) : 1.0f;
        outputs.resize(1);
        outputs[0] = std::max(mn, std::min(mx, v));
        return true;
    }
};

struct LerpNode : ScriptNode {
    LerpNode() {
        type = ScriptNodeType::LERP;
        category = ScriptNodeCategory::MATH;
        name = "Lerp";
        inputSockets.push_back({"A", ScriptVariableType::FLOAT, true, false, 0.0f, "Start value"});
        inputSockets.push_back({"B", ScriptVariableType::FLOAT, true, false, 1.0f, "End value"});
        inputSockets.push_back({"Alpha", ScriptVariableType::FLOAT, true, false, 0.5f, "Interpolation factor"});
        outputSockets.push_back({"Result", ScriptVariableType::FLOAT, false, false, 0.0f, "Interpolated value"});
    }
    bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs, ScriptExecutionContext*) override {
        float a = inputs.size() > 0 ? ToFloat(inputs[0]) : 0.0f;
        float b = inputs.size() > 1 ? ToFloat(inputs[1]) : 1.0f;
        float t = inputs.size() > 2 ? ToFloat(inputs[2]) : 0.5f;
        outputs.resize(1);
        outputs[0] = a + (b - a) * t;
        return true;
    }
};

// --- Logic nodes ---
struct LogicBinaryNode : ScriptNode {
    enum Op { AND, OR, XOR };
    Op op;
    LogicBinaryNode(Op o, const std::string& opName, ScriptNodeType t) : op(o) {
        type = t;
        category = ScriptNodeCategory::LOGIC;
        name = opName;
        inputSockets.push_back({"A", ScriptVariableType::BOOLEAN, true, false, false, "First operand"});
        inputSockets.push_back({"B", ScriptVariableType::BOOLEAN, true, false, false, "Second operand"});
        outputSockets.push_back({"Result", ScriptVariableType::BOOLEAN, false, false, false, "Result"});
    }
    bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs, ScriptExecutionContext*) override {
        bool a = inputs.size() > 0 ? ToBool(inputs[0]) : false;
        bool b = inputs.size() > 1 ? ToBool(inputs[1]) : false;
        bool result = false;
        switch (op) {
            case AND: result = a && b; break;
            case OR:  result = a || b; break;
            case XOR: result = a != b; break;
        }
        outputs.resize(1);
        outputs[0] = result;
        return true;
    }
};

struct NotNode : ScriptNode {
    NotNode() {
        type = ScriptNodeType::NOT;
        category = ScriptNodeCategory::LOGIC;
        name = "NOT";
        inputSockets.push_back({"Value", ScriptVariableType::BOOLEAN, true, false, false, "Input"});
        outputSockets.push_back({"Result", ScriptVariableType::BOOLEAN, false, false, false, "Negated result"});
    }
    bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs, ScriptExecutionContext*) override {
        bool v = inputs.size() > 0 ? ToBool(inputs[0]) : false;
        outputs.resize(1);
        outputs[0] = !v;
        return true;
    }
};

// --- Comparison nodes ---
struct ComparisonNode : ScriptNode {
    enum Op { EQ, NE, LT, LE, GT, GE };
    Op op;
    ComparisonNode(Op o, const std::string& opName, ScriptNodeType t) : op(o) {
        type = t;
        category = ScriptNodeCategory::COMPARISON;
        name = opName;
        inputSockets.push_back({"A", ScriptVariableType::FLOAT, true, false, 0.0f, "First operand"});
        inputSockets.push_back({"B", ScriptVariableType::FLOAT, true, false, 0.0f, "Second operand"});
        outputSockets.push_back({"Result", ScriptVariableType::BOOLEAN, false, false, false, "Comparison result"});
    }
    bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs, ScriptExecutionContext*) override {
        float a = inputs.size() > 0 ? ToFloat(inputs[0]) : 0.0f;
        float b = inputs.size() > 1 ? ToFloat(inputs[1]) : 0.0f;
        bool result = false;
        switch (op) {
            case EQ: result = std::abs(a - b) < 0.0001f; break;
            case NE: result = std::abs(a - b) >= 0.0001f; break;
            case LT: result = a < b; break;
            case LE: result = a <= b; break;
            case GT: result = a > b; break;
            case GE: result = a >= b; break;
        }
        outputs.resize(1);
        outputs[0] = result;
        return true;
    }
};

// --- Flow control ---
struct BranchNode : ScriptNode {
    BranchNode() {
        type = ScriptNodeType::BRANCH;
        category = ScriptNodeCategory::FLOW_CONTROL;
        name = "Branch";
        description = "If/else branching";
        inputSockets.push_back({"Exec", ScriptVariableType::EXECUTION, true, true, false, "Execution input"});
        inputSockets.push_back({"Condition", ScriptVariableType::BOOLEAN, true, false, false, "Branch condition"});
        outputSockets.push_back({"True", ScriptVariableType::EXECUTION, false, true, false, "True branch"});
        outputSockets.push_back({"False", ScriptVariableType::EXECUTION, false, true, false, "False branch"});
    }
    bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs, ScriptExecutionContext*) override {
        bool condition = inputs.size() > 1 ? ToBool(inputs[1]) : false;
        outputs.resize(2);
        outputs[0] = condition;   // True branch active
        outputs[1] = !condition;  // False branch active
        return true;
    }
};

struct SequenceNode : ScriptNode {
    SequenceNode() {
        type = ScriptNodeType::SEQUENCE;
        category = ScriptNodeCategory::FLOW_CONTROL;
        name = "Sequence";
        description = "Execute outputs in order";
        inputSockets.push_back({"Exec", ScriptVariableType::EXECUTION, true, true, false, "Execution input"});
        outputSockets.push_back({"Then 0", ScriptVariableType::EXECUTION, false, true, false, "First output"});
        outputSockets.push_back({"Then 1", ScriptVariableType::EXECUTION, false, true, false, "Second output"});
    }
    bool Execute(const std::vector<ScriptValue>&, std::vector<ScriptValue>& outputs, ScriptExecutionContext*) override {
        outputs.resize(2);
        outputs[0] = true;
        outputs[1] = true;
        return true;
    }
};

// --- String nodes ---
struct StringConcatNode : ScriptNode {
    StringConcatNode() {
        type = ScriptNodeType::STRING_CONCAT;
        category = ScriptNodeCategory::STRING;
        name = "String Concat";
        inputSockets.push_back({"A", ScriptVariableType::STRING, true, false, std::string(""), "First string"});
        inputSockets.push_back({"B", ScriptVariableType::STRING, true, false, std::string(""), "Second string"});
        outputSockets.push_back({"Result", ScriptVariableType::STRING, false, false, std::string(""), "Concatenated string"});
    }
    bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs, ScriptExecutionContext*) override {
        std::string a = inputs.size() > 0 ? ToString(inputs[0]) : "";
        std::string b = inputs.size() > 1 ? ToString(inputs[1]) : "";
        outputs.resize(1);
        outputs[0] = a + b;
        return true;
    }
};

struct StringLengthNode : ScriptNode {
    StringLengthNode() {
        type = ScriptNodeType::STRING_LENGTH;
        category = ScriptNodeCategory::STRING;
        name = "String Length";
        inputSockets.push_back({"String", ScriptVariableType::STRING, true, false, std::string(""), "Input string"});
        outputSockets.push_back({"Length", ScriptVariableType::INTEGER, false, false, 0, "String length"});
    }
    bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs, ScriptExecutionContext*) override {
        std::string s = inputs.size() > 0 ? ToString(inputs[0]) : "";
        outputs.resize(1);
        outputs[0] = static_cast<int>(s.length());
        return true;
    }
};

// --- Variable nodes ---
struct GetVariableNode : ScriptNode {
    GetVariableNode() {
        type = ScriptNodeType::GET_VARIABLE;
        category = ScriptNodeCategory::VARIABLE;
        name = "Get Variable";
        inputSockets.push_back({"Name", ScriptVariableType::STRING, true, false, std::string(""), "Variable name"});
        outputSockets.push_back({"Value", ScriptVariableType::FLOAT, false, false, 0.0f, "Variable value"});
    }
    bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs, ScriptExecutionContext* ctx) override {
        std::string varName = inputs.size() > 0 ? ToString(inputs[0]) : "";
        outputs.resize(1);
        if (ctx && !varName.empty()) {
            outputs[0] = ctx->GetVariable(varName);
        } else {
            outputs[0] = 0.0f;
        }
        return true;
    }
};

struct SetVariableNode : ScriptNode {
    SetVariableNode() {
        type = ScriptNodeType::SET_VARIABLE;
        category = ScriptNodeCategory::VARIABLE;
        name = "Set Variable";
        inputSockets.push_back({"Exec", ScriptVariableType::EXECUTION, true, true, false, "Execution input"});
        inputSockets.push_back({"Name", ScriptVariableType::STRING, true, false, std::string(""), "Variable name"});
        inputSockets.push_back({"Value", ScriptVariableType::FLOAT, true, false, 0.0f, "Value to set"});
        outputSockets.push_back({"Exec", ScriptVariableType::EXECUTION, false, true, false, "Execution output"});
    }
    bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs, ScriptExecutionContext* ctx) override {
        std::string varName = inputs.size() > 1 ? ToString(inputs[1]) : "";
        if (ctx && !varName.empty() && inputs.size() > 2) {
            ctx->SetVariable(varName, inputs[2]);
        }
        outputs.resize(1);
        outputs[0] = true;
        return true;
    }
};

// --- For loop ---
struct ForLoopNode : ScriptNode {
    ForLoopNode() {
        type = ScriptNodeType::FOR_LOOP;
        category = ScriptNodeCategory::FLOW_CONTROL;
        name = "For Loop";
        inputSockets.push_back({"Exec", ScriptVariableType::EXECUTION, true, true, false, "Execution input"});
        inputSockets.push_back({"Start", ScriptVariableType::INTEGER, true, false, 0, "Start index"});
        inputSockets.push_back({"End", ScriptVariableType::INTEGER, true, false, 10, "End index"});
        outputSockets.push_back({"Loop Body", ScriptVariableType::EXECUTION, false, true, false, "Loop body"});
        outputSockets.push_back({"Index", ScriptVariableType::INTEGER, false, false, 0, "Current index"});
        outputSockets.push_back({"Completed", ScriptVariableType::EXECUTION, false, true, false, "After loop"});
    }
    bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs, ScriptExecutionContext*) override {
        // For loop execution is handled specially by the executor
        outputs.resize(3);
        outputs[0] = true;
        outputs[1] = inputs.size() > 1 ? inputs[1] : ScriptValue(0);
        outputs[2] = true;
        return true;
    }
};

} // anonymous namespace

// ============================================================================
// ScriptGraph
// ============================================================================

ScriptNode* ScriptGraph::FindNode(uint32_t nodeID) {
    for (auto& node : nodes) {
        if (node && node->id == nodeID) return node.get();
    }
    return nullptr;
}

uint32_t ScriptGraph::AddNode(std::unique_ptr<ScriptNode> node) {
    if (!node) return 0;
    node->id = nextNodeID++;
    uint32_t id = node->id;
    nodes.push_back(std::move(node));
    isCompiled = false;
    return id;
}

bool ScriptGraph::RemoveNode(uint32_t nodeID) {
    // Remove all connections involving this node
    connections.erase(
        std::remove_if(connections.begin(), connections.end(),
            [nodeID](const ScriptConnection& c) {
                return c.fromNodeID == nodeID || c.toNodeID == nodeID;
            }),
        connections.end());

    // Remove the node
    auto it = std::find_if(nodes.begin(), nodes.end(),
        [nodeID](const std::unique_ptr<ScriptNode>& n) { return n && n->id == nodeID; });
    if (it == nodes.end()) return false;
    nodes.erase(it);
    isCompiled = false;
    return true;
}

bool ScriptGraph::CreateConnection(const ScriptConnection& connection) {
    // Validate nodes exist
    ScriptNode* fromNode = FindNode(connection.fromNodeID);
    ScriptNode* toNode = FindNode(connection.toNodeID);
    if (!fromNode || !toNode) return false;

    // Validate socket indices
    if (connection.fromSocketIndex >= fromNode->outputSockets.size()) return false;
    if (connection.toSocketIndex >= toNode->inputSockets.size()) return false;

    // Check type compatibility (execution sockets must match, data sockets are more flexible)
    auto& fromSocket = fromNode->outputSockets[connection.fromSocketIndex];
    auto& toSocket = toNode->inputSockets[connection.toSocketIndex];
    if (fromSocket.isExecution != toSocket.isExecution) return false;

    // Remove existing connection to the same input socket (inputs can only have one connection)
    connections.erase(
        std::remove_if(connections.begin(), connections.end(),
            [&](const ScriptConnection& c) {
                return c.toNodeID == connection.toNodeID && c.toSocketIndex == connection.toSocketIndex;
            }),
        connections.end());

    // Add connection
    connections.push_back(connection);
    fromSocket.isConnected = true;
    toSocket.isConnected = true;
    isCompiled = false;
    return true;
}

bool ScriptGraph::RemoveConnection(uint32_t fromNodeID, uint32_t fromSocket) {
    auto it = std::find_if(connections.begin(), connections.end(),
        [fromNodeID, fromSocket](const ScriptConnection& c) {
            return c.fromNodeID == fromNodeID && c.fromSocketIndex == fromSocket;
        });
    if (it == connections.end()) return false;

    // Update connected flags
    ScriptNode* toNode = FindNode(it->toNodeID);
    if (toNode && it->toSocketIndex < toNode->inputSockets.size()) {
        toNode->inputSockets[it->toSocketIndex].isConnected = false;
    }

    connections.erase(it);
    isCompiled = false;
    return true;
}

bool ScriptGraph::Validate(std::vector<std::string>& errors) const {
    errors.clear();

    if (nodes.empty()) {
        errors.push_back("Graph has no nodes");
        return false;
    }

    // Check for event nodes (entry points)
    bool hasEventNode = false;
    for (const auto& node : nodes) {
        if (!node) continue;
        if (node->category == ScriptNodeCategory::EVENT) {
            hasEventNode = true;
            break;
        }
    }
    if (!hasEventNode) {
        errors.push_back("Graph has no event nodes (entry points)");
    }

    // Check for required unconnected inputs
    for (const auto& node : nodes) {
        if (!node) continue;
        for (size_t i = 0; i < node->inputSockets.size(); ++i) {
            if (node->inputSockets[i].isRequired && !node->inputSockets[i].isConnected) {
                errors.push_back("Node '" + node->name + "' has unconnected required input '" +
                                 node->inputSockets[i].name + "'");
            }
        }
    }

    // Check for cycles in execution flow (simple DFS)
    // Build adjacency list for execution connections
    std::unordered_map<uint32_t, std::vector<uint32_t>> execAdj;
    for (const auto& conn : connections) {
        const ScriptNode* fromNode = nullptr;
        for (const auto& n : nodes) {
            if (n && n->id == conn.fromNodeID) { fromNode = n.get(); break; }
        }
        if (fromNode && conn.fromSocketIndex < fromNode->outputSockets.size() &&
            fromNode->outputSockets[conn.fromSocketIndex].isExecution) {
            execAdj[conn.fromNodeID].push_back(conn.toNodeID);
        }
    }

    // DFS cycle detection
    std::unordered_set<uint32_t> visited, inStack;
    std::function<bool(uint32_t)> hasCycle = [&](uint32_t nodeID) -> bool {
        visited.insert(nodeID);
        inStack.insert(nodeID);
        if (execAdj.count(nodeID)) {
            for (uint32_t next : execAdj[nodeID]) {
                if (inStack.count(next)) return true;
                if (!visited.count(next) && hasCycle(next)) return true;
            }
        }
        inStack.erase(nodeID);
        return false;
    };

    for (const auto& node : nodes) {
        if (node && !visited.count(node->id)) {
            if (hasCycle(node->id)) {
                errors.push_back("Graph contains a cycle in execution flow");
                break;
            }
        }
    }

    return errors.empty();
}

bool ScriptGraph::Compile() {
    compilationErrors.clear();
    bool valid = Validate(compilationErrors);
    isCompiled = valid;
    return valid;
}

// ============================================================================
// ScriptExecutionContext
// ============================================================================

ScriptExecutionContext::ScriptExecutionContext(ObjectID targetObject)
    : m_targetObject(targetObject) {
}

ScriptValue ScriptExecutionContext::GetVariable(const std::string& name) const {
    auto it = m_variables.find(name);
    if (it != m_variables.end()) return it->second;
    return 0.0f; // default to float 0
}

void ScriptExecutionContext::SetVariable(const std::string& name, const ScriptValue& value) {
    m_variables[name] = value;
}

void ScriptExecutionContext::Log(const std::string& message, const std::string& level) {
    // Store in execution log - actual output depends on integration
    (void)level;
    (void)message;
}

float ScriptExecutionContext::GetDeltaTime() const {
    // In a real integration this would come from the engine timer
    return 0.016f; // ~60fps default
}

// ============================================================================
// ScriptExecutor
// ============================================================================

ScriptExecutor::ScriptExecutor() {
}

ScriptExecutor::~ScriptExecutor() {
}

bool ScriptExecutor::ExecuteGraph(const ScriptGraph& graph, ScriptExecutionContext& context, uint32_t startNodeID) {
    // Find start node
    ScriptNode* startNode = nullptr;
    if (startNodeID != 0) {
        for (const auto& n : graph.nodes) {
            if (n && n->id == startNodeID) { startNode = n.get(); break; }
        }
    } else {
        // Find first EVENT_START node
        for (const auto& n : graph.nodes) {
            if (n && n->type == ScriptNodeType::EVENT_START) { startNode = n.get(); break; }
        }
    }

    if (!startNode) return false;

    // Execute starting from the start node, following execution flow
    return ExecuteNode(startNode, context);
}

bool ScriptExecutor::ExecuteNode(ScriptNode* node, ScriptExecutionContext& context) {
    if (!node || context.ShouldStop()) return false;

    m_currentNode = node->id;
    node->isExecuting = true;

    // Check breakpoint
    if (m_isDebugging && m_breakpoints.count(node->id) && m_breakpoints[node->id]) {
        m_stepMode = true;
        node->isExecuting = false;
        return true; // paused at breakpoint
    }

    // Gather input values - for connected inputs, evaluate the source
    std::vector<ScriptValue> inputs(node->inputSockets.size());
    for (size_t i = 0; i < node->inputSockets.size(); ++i) {
        if (node->inputSockets[i].isExecution) {
            inputs[i] = true; // execution flow is active
            continue;
        }
        inputs[i] = node->inputSockets[i].defaultValue;
    }

    // Execute the node
    std::vector<ScriptValue> outputs;
    bool success = node->Execute(inputs, outputs, &context);

    node->isExecuting = false;
    if (!success) {
        node->hasError = true;
        node->errorMessage = "Execution failed";
        return false;
    }
    node->hasError = false;

    return success;
}

void ScriptExecutor::StartDebugging(const ScriptGraph& graph, ScriptExecutionContext& context) {
    m_isDebugging = true;
    m_stepMode = false;
    m_currentNode = 0;

    // Find start node and begin
    for (const auto& n : graph.nodes) {
        if (n && n->type == ScriptNodeType::EVENT_START) {
            m_executionQueue.push(n->id);
            break;
        }
    }
    (void)context;
}

void ScriptExecutor::StopDebugging() {
    m_isDebugging = false;
    m_stepMode = false;
    m_currentNode = 0;
    while (!m_executionQueue.empty()) m_executionQueue.pop();
}

void ScriptExecutor::StepNext() {
    if (!m_isDebugging) return;
    m_stepMode = true;
}

void ScriptExecutor::Continue() {
    if (!m_isDebugging) return;
    m_stepMode = false;
}

void ScriptExecutor::SetBreakpoint(uint32_t nodeID, bool enabled) {
    m_breakpoints[nodeID] = enabled;
}

bool ScriptExecutor::ExecuteTopological(const ScriptGraph& graph, ScriptExecutionContext& context,
                                        const std::vector<uint32_t>& startNodes) {
    auto order = GetExecutionOrder(graph, startNodes);
    for (uint32_t nodeID : order) {
        if (context.ShouldStop()) return false;
        ScriptNode* node = nullptr;
        for (const auto& n : graph.nodes) {
            if (n && n->id == nodeID) { node = n.get(); break; }
        }
        if (node && !ExecuteNode(node, context)) return false;
    }
    return true;
}

std::vector<uint32_t> ScriptExecutor::GetExecutionOrder(const ScriptGraph& graph, const std::vector<uint32_t>& startNodes) {
    // Build execution adjacency list
    std::unordered_map<uint32_t, std::vector<uint32_t>> adj;
    std::unordered_map<uint32_t, int> inDegree;

    for (const auto& n : graph.nodes) {
        if (n) inDegree[n->id] = 0;
    }

    for (const auto& conn : graph.connections) {
        // Only follow execution connections
        for (const auto& n : graph.nodes) {
            if (n && n->id == conn.fromNodeID &&
                conn.fromSocketIndex < n->outputSockets.size() &&
                n->outputSockets[conn.fromSocketIndex].isExecution) {
                adj[conn.fromNodeID].push_back(conn.toNodeID);
                inDegree[conn.toNodeID]++;
                break;
            }
        }
    }

    // BFS topological sort starting from start nodes
    std::queue<uint32_t> q;
    for (uint32_t id : startNodes) {
        q.push(id);
    }
    // Also add nodes with no incoming execution edges
    if (startNodes.empty()) {
        for (const auto& [id, deg] : inDegree) {
            if (deg == 0) q.push(id);
        }
    }

    std::vector<uint32_t> order;
    std::unordered_set<uint32_t> visited;
    while (!q.empty()) {
        uint32_t curr = q.front(); q.pop();
        if (visited.count(curr)) continue;
        visited.insert(curr);
        order.push_back(curr);
        if (adj.count(curr)) {
            for (uint32_t next : adj[curr]) {
                if (!visited.count(next)) q.push(next);
            }
        }
    }

    return order;
}

ScriptValue ScriptExecutor::EvaluateSocket(const ScriptGraph& graph, uint32_t nodeID, uint32_t socketIndex,
                                           ScriptExecutionContext& context) {
    // Find what's connected to this input socket
    for (const auto& conn : graph.connections) {
        if (conn.toNodeID == nodeID && conn.toSocketIndex == socketIndex) {
            // Found the source - execute the source node and get its output
            ScriptNode* sourceNode = nullptr;
            for (const auto& n : graph.nodes) {
                if (n && n->id == conn.fromNodeID) { sourceNode = n.get(); break; }
            }
            if (sourceNode) {
                std::vector<ScriptValue> inputs(sourceNode->inputSockets.size());
                for (size_t i = 0; i < inputs.size(); ++i) {
                    inputs[i] = sourceNode->inputSockets[i].defaultValue;
                }
                std::vector<ScriptValue> outputs;
                sourceNode->Execute(inputs, outputs, &context);
                if (conn.fromSocketIndex < outputs.size()) {
                    return outputs[conn.fromSocketIndex];
                }
            }
        }
    }

    // No connection found, return default
    ScriptNode* node = nullptr;
    for (const auto& n : graph.nodes) {
        if (n && n->id == nodeID) { node = n.get(); break; }
    }
    if (node && socketIndex < node->inputSockets.size()) {
        return node->inputSockets[socketIndex].defaultValue;
    }
    return false;
}

// ============================================================================
// VisualScriptingSystem
// ============================================================================

VisualScriptingSystem::VisualScriptingSystem()
    : EditorPanel("Visual Scripting", "visual_scripting") {
}

VisualScriptingSystem::~VisualScriptingSystem() {
    Shutdown();
}

bool VisualScriptingSystem::Initialize() {
    m_executor = std::make_unique<ScriptExecutor>();
    InitializeBuiltInNodes();
    return true;
}

void VisualScriptingSystem::Update(float deltaTime) {
    if (m_isExecuting) {
        UpdateExecution();
    }
    (void)deltaTime;
}

void VisualScriptingSystem::Render() {
    // Main layout: palette | editor | properties
    // Not rendering ImGui here as this depends on the editor's ImGui context
    // In a full integration, this would be called from the editor's render loop
    RenderNodePalette();
    RenderScriptEditor();
    RenderScriptProperties();
    if (m_isDebugging) {
        RenderDebugInterface();
        RenderExecutionLog();
    }
}

void VisualScriptingSystem::Shutdown() {
    m_executor.reset();
    m_executionContext.reset();
    m_currentScript = ScriptGraph();
    m_nodeFactories.clear();
    m_nodeCategories.clear();
    m_customNodes.clear();
}

bool VisualScriptingSystem::HandleEvent(const std::string& eventType, void* eventData) {
    if (eventType == "delete_selected") {
        for (uint32_t id : m_selectedNodes) {
            m_currentScript.RemoveNode(id);
        }
        m_selectedNodes.clear();
        return true;
    }
    return false;
    (void)eventData;
}

void VisualScriptingSystem::CreateNewScript(const std::string& name, ObjectID targetObject) {
    m_currentScript = ScriptGraph();
    m_currentScript.name = name;
    m_currentScript.targetObjectID = targetObject;
    m_selectedNodes.clear();
    m_isExecuting = false;
    m_isDebugging = false;

    // Add default Event Start node
    auto startNode = CreateNode(ScriptNodeType::EVENT_START);
    if (startNode) {
        startNode->position = {100.0f, 200.0f};
        m_currentScript.AddNode(std::move(startNode));
    }
}

bool VisualScriptingSystem::LoadScript(const std::string& filePath) {
    std::ifstream file(filePath);
    if (!file.is_open()) return false;

    // Simple text-based format for script loading
    std::string line;
    ScriptGraph newGraph;

    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream iss(line);
        std::string token;
        iss >> token;

        if (token == "name") {
            std::getline(iss >> std::ws, newGraph.name);
        } else if (token == "node") {
            int typeInt;
            float x, y;
            if (iss >> typeInt >> x >> y) {
                auto node = CreateNode(static_cast<ScriptNodeType>(typeInt));
                if (node) {
                    node->position = {x, y};
                    newGraph.AddNode(std::move(node));
                }
            }
        } else if (token == "connection") {
            ScriptConnection conn;
            if (iss >> conn.fromNodeID >> conn.fromSocketIndex >> conn.toNodeID >> conn.toSocketIndex) {
                newGraph.CreateConnection(conn);
            }
        } else if (token == "variable") {
            ScriptVariable var;
            int typeInt;
            if (iss >> var.name >> typeInt) {
                var.type = static_cast<ScriptVariableType>(typeInt);
                newGraph.variables.push_back(var);
            }
        }
    }

    m_currentScript = std::move(newGraph);
    m_selectedNodes.clear();
    return true;
}

bool VisualScriptingSystem::SaveScript(const std::string& filePath) {
    std::ofstream file(filePath);
    if (!file.is_open()) return false;

    file << "# Spark Engine Visual Script\n";
    file << "name " << m_currentScript.name << "\n";

    // Save nodes
    for (const auto& node : m_currentScript.nodes) {
        if (!node) continue;
        file << "node " << static_cast<int>(node->type)
             << " " << node->position.x << " " << node->position.y << "\n";
    }

    // Save connections
    for (const auto& conn : m_currentScript.connections) {
        file << "connection " << conn.fromNodeID << " " << conn.fromSocketIndex
             << " " << conn.toNodeID << " " << conn.toSocketIndex << "\n";
    }

    // Save variables
    for (const auto& var : m_currentScript.variables) {
        file << "variable " << var.name << " " << static_cast<int>(var.type) << "\n";
    }

    return true;
}

bool VisualScriptingSystem::CompileScript() {
    return m_currentScript.Compile();
}

bool VisualScriptingSystem::ExecuteScript(ObjectID targetObject) {
    if (!m_executor) return false;
    if (!m_currentScript.isCompiled) {
        if (!CompileScript()) return false;
    }

    m_executionContext = std::make_unique<ScriptExecutionContext>(targetObject);

    // Initialize variables with defaults
    for (const auto& var : m_currentScript.variables) {
        m_executionContext->SetVariable(var.name, var.defaultValue);
    }

    auto start = std::chrono::high_resolution_clock::now();
    bool result = m_executor->ExecuteGraph(m_currentScript, *m_executionContext);
    auto end = std::chrono::high_resolution_clock::now();
    m_lastExecutionTime = std::chrono::duration<float, std::milli>(end - start).count();

    m_isExecuting = result;
    return result;
}

uint32_t VisualScriptingSystem::AddNode(ScriptNodeType nodeType, const XMFLOAT2& position) {
    auto node = CreateNode(nodeType);
    if (!node) return 0;
    node->position = position;
    return m_currentScript.AddNode(std::move(node));
}

bool VisualScriptingSystem::RemoveNode(uint32_t nodeID) {
    // Remove from selection
    m_selectedNodes.erase(
        std::remove(m_selectedNodes.begin(), m_selectedNodes.end(), nodeID),
        m_selectedNodes.end());
    return m_currentScript.RemoveNode(nodeID);
}

bool VisualScriptingSystem::ConnectSockets(uint32_t fromNodeID, uint32_t fromSocketIndex,
                                           uint32_t toNodeID, uint32_t toSocketIndex) {
    ScriptConnection conn;
    conn.fromNodeID = fromNodeID;
    conn.fromSocketIndex = fromSocketIndex;
    conn.toNodeID = toNodeID;
    conn.toSocketIndex = toSocketIndex;
    return m_currentScript.CreateConnection(conn);
}

void VisualScriptingSystem::StartDebugging(ObjectID targetObject) {
    if (!m_executor) return;
    if (!m_currentScript.isCompiled) {
        if (!CompileScript()) return;
    }
    m_executionContext = std::make_unique<ScriptExecutionContext>(targetObject);
    m_executor->StartDebugging(m_currentScript, *m_executionContext);
    m_isDebugging = true;
    m_executionLog.clear();
    m_executionLog.push_back("Debugging started");
}

void VisualScriptingSystem::StopDebugging() {
    if (m_executor) m_executor->StopDebugging();
    m_isDebugging = false;
    m_isExecuting = false;
    m_executionLog.push_back("Debugging stopped");
}

void VisualScriptingSystem::RegisterCustomNode(const std::string& typeName,
                                               std::function<std::unique_ptr<ScriptNode>()> factory) {
    m_customNodes[typeName] = std::move(factory);
}

// ============================================================================
// Private methods
// ============================================================================

void VisualScriptingSystem::RenderScriptEditor() {
    // Graph canvas rendering would use ImGui draw lists in a real integration
}

void VisualScriptingSystem::RenderNodePalette() {
    // Node palette would render categorized list of available nodes
}

void VisualScriptingSystem::RenderScriptProperties() {
    // Properties panel for selected node/variable editing
}

void VisualScriptingSystem::RenderDebugInterface() {
    // Debug controls: step, continue, stop, breakpoints
}

void VisualScriptingSystem::RenderExecutionLog() {
    // Show execution log messages
}

void VisualScriptingSystem::RenderScriptNode(ScriptNode* node) {
    if (!node) return;
    // Would render node header, sockets, and body using ImGui
    (void)node;
}

void VisualScriptingSystem::RenderConnections() {
    // Would render bezier curves between connected sockets
}

void VisualScriptingSystem::HandleNodeCreation() {
    // Handle node creation from palette drag or right-click menu
}

void VisualScriptingSystem::HandleNodeDragging() {
    // Handle node position updates during drag
    if (!m_isDraggingNode) return;
    ScriptNode* node = m_currentScript.FindNode(m_draggedNodeID);
    if (!node) {
        m_isDraggingNode = false;
        return;
    }
    // In a real ImGui integration: node->position = mousePos - dragOffset
}

void VisualScriptingSystem::HandleConnectionCreation() {
    // Handle connection creation by dragging from socket to socket
}

void VisualScriptingSystem::HandleNodeSelection() {
    // Handle click-to-select and box selection
}

std::unique_ptr<ScriptNode> VisualScriptingSystem::CreateNode(ScriptNodeType nodeType) {
    // Check custom node factories first
    auto factoryIt = m_nodeFactories.find(nodeType);
    if (factoryIt != m_nodeFactories.end()) {
        return factoryIt->second();
    }

    // Built-in node types
    switch (nodeType) {
        // Events
        case ScriptNodeType::EVENT_START:  return std::make_unique<EventStartNode>();
        case ScriptNodeType::EVENT_UPDATE: return std::make_unique<EventUpdateNode>();

        // Flow control
        case ScriptNodeType::BRANCH:   return std::make_unique<BranchNode>();
        case ScriptNodeType::SEQUENCE: return std::make_unique<SequenceNode>();
        case ScriptNodeType::FOR_LOOP: return std::make_unique<ForLoopNode>();

        // Math
        case ScriptNodeType::ADD:      return std::make_unique<MathBinaryNode>(MathBinaryNode::ADD, "Add", nodeType);
        case ScriptNodeType::SUBTRACT: return std::make_unique<MathBinaryNode>(MathBinaryNode::SUB, "Subtract", nodeType);
        case ScriptNodeType::MULTIPLY: return std::make_unique<MathBinaryNode>(MathBinaryNode::MUL, "Multiply", nodeType);
        case ScriptNodeType::DIVIDE:   return std::make_unique<MathBinaryNode>(MathBinaryNode::DIV, "Divide", nodeType);
        case ScriptNodeType::POWER:    return std::make_unique<MathBinaryNode>(MathBinaryNode::POW, "Power", nodeType);
        case ScriptNodeType::SQRT:     return std::make_unique<MathUnaryNode>(MathUnaryNode::SQRT, "Square Root", nodeType);
        case ScriptNodeType::SIN:      return std::make_unique<MathUnaryNode>(MathUnaryNode::SIN, "Sine", nodeType);
        case ScriptNodeType::COS:      return std::make_unique<MathUnaryNode>(MathUnaryNode::COS, "Cosine", nodeType);
        case ScriptNodeType::TAN:      return std::make_unique<MathUnaryNode>(MathUnaryNode::TAN, "Tangent", nodeType);
        case ScriptNodeType::CLAMP:    return std::make_unique<ClampNode>();
        case ScriptNodeType::LERP:     return std::make_unique<LerpNode>();

        // Logic
        case ScriptNodeType::AND: return std::make_unique<LogicBinaryNode>(LogicBinaryNode::AND, "AND", nodeType);
        case ScriptNodeType::OR:  return std::make_unique<LogicBinaryNode>(LogicBinaryNode::OR, "OR", nodeType);
        case ScriptNodeType::XOR: return std::make_unique<LogicBinaryNode>(LogicBinaryNode::XOR, "XOR", nodeType);
        case ScriptNodeType::NOT: return std::make_unique<NotNode>();

        // Comparison
        case ScriptNodeType::EQUAL:         return std::make_unique<ComparisonNode>(ComparisonNode::EQ, "Equal", nodeType);
        case ScriptNodeType::NOT_EQUAL:     return std::make_unique<ComparisonNode>(ComparisonNode::NE, "Not Equal", nodeType);
        case ScriptNodeType::LESS:          return std::make_unique<ComparisonNode>(ComparisonNode::LT, "Less Than", nodeType);
        case ScriptNodeType::LESS_EQUAL:    return std::make_unique<ComparisonNode>(ComparisonNode::LE, "Less or Equal", nodeType);
        case ScriptNodeType::GREATER:       return std::make_unique<ComparisonNode>(ComparisonNode::GT, "Greater Than", nodeType);
        case ScriptNodeType::GREATER_EQUAL: return std::make_unique<ComparisonNode>(ComparisonNode::GE, "Greater or Equal", nodeType);

        // Strings
        case ScriptNodeType::STRING_CONCAT: return std::make_unique<StringConcatNode>();
        case ScriptNodeType::STRING_LENGTH: return std::make_unique<StringLengthNode>();

        // Variables
        case ScriptNodeType::GET_VARIABLE: return std::make_unique<GetVariableNode>();
        case ScriptNodeType::SET_VARIABLE: return std::make_unique<SetVariableNode>();

        default:
            return nullptr;
    }
}

XMFLOAT4 VisualScriptingSystem::GetNodeCategoryColor(ScriptNodeCategory category) const {
    switch (category) {
        case ScriptNodeCategory::EVENT:        return {0.7f, 0.2f, 0.2f, 1.0f}; // Red
        case ScriptNodeCategory::FLOW_CONTROL: return {0.8f, 0.8f, 0.8f, 1.0f}; // White/grey
        case ScriptNodeCategory::MATH:         return {0.2f, 0.6f, 0.2f, 1.0f}; // Green
        case ScriptNodeCategory::LOGIC:        return {0.3f, 0.3f, 0.7f, 1.0f}; // Blue
        case ScriptNodeCategory::COMPARISON:   return {0.2f, 0.5f, 0.7f, 1.0f}; // Cyan
        case ScriptNodeCategory::STRING:       return {0.8f, 0.4f, 0.6f, 1.0f}; // Pink
        case ScriptNodeCategory::ARRAY:        return {0.6f, 0.4f, 0.2f, 1.0f}; // Brown
        case ScriptNodeCategory::OBJECT:       return {0.3f, 0.5f, 0.8f, 1.0f}; // Light blue
        case ScriptNodeCategory::INPUT:        return {0.8f, 0.6f, 0.2f, 1.0f}; // Orange
        case ScriptNodeCategory::AUDIO:        return {0.6f, 0.2f, 0.6f, 1.0f}; // Purple
        case ScriptNodeCategory::GRAPHICS:     return {0.2f, 0.7f, 0.7f, 1.0f}; // Teal
        case ScriptNodeCategory::PHYSICS:      return {0.5f, 0.7f, 0.2f, 1.0f}; // Lime
        case ScriptNodeCategory::VARIABLE:     return {0.4f, 0.6f, 0.4f, 1.0f}; // Sage
        case ScriptNodeCategory::FUNCTION:     return {0.5f, 0.3f, 0.7f, 1.0f}; // Violet
        case ScriptNodeCategory::CUSTOM:       return {0.6f, 0.6f, 0.6f, 1.0f}; // Grey
        default:                               return {0.3f, 0.3f, 0.3f, 1.0f};
    }
}

XMFLOAT4 VisualScriptingSystem::GetSocketTypeColor(ScriptVariableType type) const {
    switch (type) {
        case ScriptVariableType::EXECUTION:          return {1.0f, 1.0f, 1.0f, 1.0f}; // White
        case ScriptVariableType::BOOLEAN:            return {0.7f, 0.1f, 0.1f, 1.0f}; // Red
        case ScriptVariableType::INTEGER:            return {0.1f, 0.8f, 0.8f, 1.0f}; // Cyan
        case ScriptVariableType::FLOAT:              return {0.1f, 0.8f, 0.1f, 1.0f}; // Green
        case ScriptVariableType::STRING:             return {0.9f, 0.1f, 0.9f, 1.0f}; // Magenta
        case ScriptVariableType::VECTOR2:            return {0.9f, 0.7f, 0.1f, 1.0f}; // Gold
        case ScriptVariableType::VECTOR3:            return {0.9f, 0.9f, 0.1f, 1.0f}; // Yellow
        case ScriptVariableType::VECTOR4:            return {1.0f, 0.5f, 0.1f, 1.0f}; // Orange
        case ScriptVariableType::COLOR:              return {1.0f, 1.0f, 1.0f, 1.0f}; // White
        case ScriptVariableType::OBJECT_REFERENCE:   return {0.1f, 0.4f, 0.9f, 1.0f}; // Blue
        case ScriptVariableType::ARRAY:              return {0.6f, 0.4f, 0.2f, 1.0f}; // Brown
        default:                                     return {0.7f, 0.7f, 0.7f, 1.0f}; // Grey
    }
}

void VisualScriptingSystem::InitializeBuiltInNodes() {
    // Register category listings for the node palette
    m_nodeCategories["Events"] = {
        ScriptNodeType::EVENT_START, ScriptNodeType::EVENT_UPDATE
    };
    m_nodeCategories["Flow Control"] = {
        ScriptNodeType::BRANCH, ScriptNodeType::SEQUENCE, ScriptNodeType::FOR_LOOP
    };
    m_nodeCategories["Math"] = {
        ScriptNodeType::ADD, ScriptNodeType::SUBTRACT, ScriptNodeType::MULTIPLY,
        ScriptNodeType::DIVIDE, ScriptNodeType::POWER, ScriptNodeType::SQRT,
        ScriptNodeType::SIN, ScriptNodeType::COS, ScriptNodeType::TAN,
        ScriptNodeType::CLAMP, ScriptNodeType::LERP
    };
    m_nodeCategories["Logic"] = {
        ScriptNodeType::AND, ScriptNodeType::OR, ScriptNodeType::NOT, ScriptNodeType::XOR
    };
    m_nodeCategories["Comparison"] = {
        ScriptNodeType::EQUAL, ScriptNodeType::NOT_EQUAL,
        ScriptNodeType::LESS, ScriptNodeType::LESS_EQUAL,
        ScriptNodeType::GREATER, ScriptNodeType::GREATER_EQUAL
    };
    m_nodeCategories["Strings"] = {
        ScriptNodeType::STRING_CONCAT, ScriptNodeType::STRING_LENGTH
    };
    m_nodeCategories["Variables"] = {
        ScriptNodeType::GET_VARIABLE, ScriptNodeType::SET_VARIABLE
    };
}

void VisualScriptingSystem::UpdateExecution() {
    // In continuous execution mode, step the executor
    if (m_isExecuting && m_executionContext && m_executionContext->ShouldStop()) {
        m_isExecuting = false;
    }
}

} // namespace SparkEditor
