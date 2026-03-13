/**
 * @file VisualScriptingSystem.cpp
 * @brief Implementation of VisualScriptingSystem, ScriptGraph, ScriptExecutionContext, ScriptExecutor
 */

#include "VisualScriptingSystem.h"
#include "Utils/Validate.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <chrono>

#include <imgui.h>

using namespace DirectX;
namespace SparkEditor
{

    // ============================================================================
    // Concrete node implementations
    // ============================================================================

    namespace
    {

        // Helper to get a float from a ScriptValue (handles int/float/bool)
        static float ToFloat(const ScriptValue& v)
        {
            if (auto* f = std::get_if<float>(&v))
                return *f;
            if (auto* i = std::get_if<int>(&v))
                return static_cast<float>(*i);
            if (auto* b = std::get_if<bool>(&v))
                return *b ? 1.0f : 0.0f;
            return 0.0f;
        }

        static int ToInt(const ScriptValue& v)
        {
            if (auto* i = std::get_if<int>(&v))
                return *i;
            if (auto* f = std::get_if<float>(&v))
                return static_cast<int>(*f);
            if (auto* b = std::get_if<bool>(&v))
                return *b ? 1 : 0;
            return 0;
        }

        static bool ToBool(const ScriptValue& v)
        {
            if (auto* b = std::get_if<bool>(&v))
                return *b;
            if (auto* i = std::get_if<int>(&v))
                return *i != 0;
            if (auto* f = std::get_if<float>(&v))
                return *f != 0.0f;
            if (auto* s = std::get_if<std::string>(&v))
                return !s->empty();
            return false;
        }

        static std::string ToString(const ScriptValue& v)
        {
            if (auto* s = std::get_if<std::string>(&v))
                return *s;
            if (auto* f = std::get_if<float>(&v))
                return std::to_string(*f);
            if (auto* i = std::get_if<int>(&v))
                return std::to_string(*i);
            if (auto* b = std::get_if<bool>(&v))
                return *b ? "true" : "false";
            return "";
        }

        // --- Event nodes ---
        struct EventStartNode : ScriptNode
        {
            EventStartNode()
            {
                type = ScriptNodeType::EVENT_START;
                category = ScriptNodeCategory::EVENT;
                name = "Event Start";
                description = "Called when script begins execution";
                outputSockets.push_back(
                    {"Exec", ScriptVariableType::EXECUTION, false, true, false, "Execution output"});
            }
            bool Execute(const std::vector<ScriptValue>&, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                outputs.resize(1);
                outputs[0] = true; // signal execution flow
                return true;
            }
        };

        struct EventUpdateNode : ScriptNode
        {
            EventUpdateNode()
            {
                type = ScriptNodeType::EVENT_UPDATE;
                category = ScriptNodeCategory::EVENT;
                name = "Event Update";
                description = "Called every frame";
                outputSockets.push_back(
                    {"Exec", ScriptVariableType::EXECUTION, false, true, false, "Execution output"});
                outputSockets.push_back(
                    {"Delta Time", ScriptVariableType::FLOAT, false, false, 0.0f, "Frame delta time"});
            }
            bool Execute(const std::vector<ScriptValue>&, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext* ctx) override
            {
                outputs.resize(2);
                outputs[0] = true;
                outputs[1] = ctx ? ctx->GetDeltaTime() : 0.016f;
                return true;
            }
        };

        // --- Math nodes ---
        struct MathBinaryNode : ScriptNode
        {
            enum Op
            {
                ADD,
                SUB,
                MUL,
                DIV,
                POW
            };
            Op op;
            MathBinaryNode(Op o, const std::string& opName, ScriptNodeType t) : op(o)
            {
                type = t;
                category = ScriptNodeCategory::MATH;
                name = opName;
                inputSockets.push_back({"A", ScriptVariableType::FLOAT, true, false, 0.0f, "First operand"});
                inputSockets.push_back({"B", ScriptVariableType::FLOAT, true, false, 0.0f, "Second operand"});
                outputSockets.push_back({"Result", ScriptVariableType::FLOAT, false, false, 0.0f, "Result"});
            }
            bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                float a = inputs.size() > 0 ? ToFloat(inputs[0]) : 0.0f;
                float b = inputs.size() > 1 ? ToFloat(inputs[1]) : 0.0f;
                float result = 0.0f;
                switch (op)
                {
                case ADD:
                    result = a + b;
                    break;
                case SUB:
                    result = a - b;
                    break;
                case MUL:
                    result = a * b;
                    break;
                case DIV:
                    result = (b != 0.0f) ? a / b : 0.0f;
                    break;
                case POW:
                    result = std::pow(a, b);
                    break;
                }
                outputs.resize(1);
                outputs[0] = result;
                return true;
            }
        };

        struct MathUnaryNode : ScriptNode
        {
            enum Fn
            {
                SQRT,
                SIN,
                COS,
                TAN
            };
            Fn fn;
            MathUnaryNode(Fn f, const std::string& fnName, ScriptNodeType t) : fn(f)
            {
                type = t;
                category = ScriptNodeCategory::MATH;
                name = fnName;
                inputSockets.push_back({"Value", ScriptVariableType::FLOAT, true, false, 0.0f, "Input value"});
                outputSockets.push_back({"Result", ScriptVariableType::FLOAT, false, false, 0.0f, "Result"});
            }
            bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                float v = inputs.size() > 0 ? ToFloat(inputs[0]) : 0.0f;
                float result = 0.0f;
                switch (fn)
                {
                case SQRT:
                    result = std::sqrt(std::abs(v));
                    break;
                case SIN:
                    result = std::sin(v);
                    break;
                case COS:
                    result = std::cos(v);
                    break;
                case TAN:
                    result = std::tan(v);
                    break;
                }
                outputs.resize(1);
                outputs[0] = result;
                return true;
            }
        };

        struct ClampNode : ScriptNode
        {
            ClampNode()
            {
                type = ScriptNodeType::CLAMP;
                category = ScriptNodeCategory::MATH;
                name = "Clamp";
                inputSockets.push_back({"Value", ScriptVariableType::FLOAT, true, false, 0.0f, "Value to clamp"});
                inputSockets.push_back({"Min", ScriptVariableType::FLOAT, true, false, 0.0f, "Minimum"});
                inputSockets.push_back({"Max", ScriptVariableType::FLOAT, true, false, 1.0f, "Maximum"});
                outputSockets.push_back({"Result", ScriptVariableType::FLOAT, false, false, 0.0f, "Clamped value"});
            }
            bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                float v = inputs.size() > 0 ? ToFloat(inputs[0]) : 0.0f;
                float mn = inputs.size() > 1 ? ToFloat(inputs[1]) : 0.0f;
                float mx = inputs.size() > 2 ? ToFloat(inputs[2]) : 1.0f;
                outputs.resize(1);
                outputs[0] = std::max(mn, std::min(mx, v));
                return true;
            }
        };

        struct LerpNode : ScriptNode
        {
            LerpNode()
            {
                type = ScriptNodeType::LERP;
                category = ScriptNodeCategory::MATH;
                name = "Lerp";
                inputSockets.push_back({"A", ScriptVariableType::FLOAT, true, false, 0.0f, "Start value"});
                inputSockets.push_back({"B", ScriptVariableType::FLOAT, true, false, 1.0f, "End value"});
                inputSockets.push_back({"Alpha", ScriptVariableType::FLOAT, true, false, 0.5f, "Interpolation factor"});
                outputSockets.push_back(
                    {"Result", ScriptVariableType::FLOAT, false, false, 0.0f, "Interpolated value"});
            }
            bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                float a = inputs.size() > 0 ? ToFloat(inputs[0]) : 0.0f;
                float b = inputs.size() > 1 ? ToFloat(inputs[1]) : 1.0f;
                float t = inputs.size() > 2 ? ToFloat(inputs[2]) : 0.5f;
                outputs.resize(1);
                outputs[0] = a + (b - a) * t;
                return true;
            }
        };

        // --- Logic nodes ---
        struct LogicBinaryNode : ScriptNode
        {
            enum Op
            {
                AND,
                OR,
                XOR
            };
            Op op;
            LogicBinaryNode(Op o, const std::string& opName, ScriptNodeType t) : op(o)
            {
                type = t;
                category = ScriptNodeCategory::LOGIC;
                name = opName;
                inputSockets.push_back({"A", ScriptVariableType::BOOLEAN, true, false, false, "First operand"});
                inputSockets.push_back({"B", ScriptVariableType::BOOLEAN, true, false, false, "Second operand"});
                outputSockets.push_back({"Result", ScriptVariableType::BOOLEAN, false, false, false, "Result"});
            }
            bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                bool a = inputs.size() > 0 ? ToBool(inputs[0]) : false;
                bool b = inputs.size() > 1 ? ToBool(inputs[1]) : false;
                bool result = false;
                switch (op)
                {
                case AND:
                    result = a && b;
                    break;
                case OR:
                    result = a || b;
                    break;
                case XOR:
                    result = a != b;
                    break;
                }
                outputs.resize(1);
                outputs[0] = result;
                return true;
            }
        };

        struct NotNode : ScriptNode
        {
            NotNode()
            {
                type = ScriptNodeType::NOT;
                category = ScriptNodeCategory::LOGIC;
                name = "NOT";
                inputSockets.push_back({"Value", ScriptVariableType::BOOLEAN, true, false, false, "Input"});
                outputSockets.push_back({"Result", ScriptVariableType::BOOLEAN, false, false, false, "Negated result"});
            }
            bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                bool v = inputs.size() > 0 ? ToBool(inputs[0]) : false;
                outputs.resize(1);
                outputs[0] = !v;
                return true;
            }
        };

        // --- Comparison nodes ---
        struct ComparisonNode : ScriptNode
        {
            enum Op
            {
                EQ,
                NE,
                LT,
                LE,
                GT,
                GE
            };
            Op op;
            ComparisonNode(Op o, const std::string& opName, ScriptNodeType t) : op(o)
            {
                type = t;
                category = ScriptNodeCategory::COMPARISON;
                name = opName;
                inputSockets.push_back({"A", ScriptVariableType::FLOAT, true, false, 0.0f, "First operand"});
                inputSockets.push_back({"B", ScriptVariableType::FLOAT, true, false, 0.0f, "Second operand"});
                outputSockets.push_back(
                    {"Result", ScriptVariableType::BOOLEAN, false, false, false, "Comparison result"});
            }
            bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                float a = inputs.size() > 0 ? ToFloat(inputs[0]) : 0.0f;
                float b = inputs.size() > 1 ? ToFloat(inputs[1]) : 0.0f;
                bool result = false;
                switch (op)
                {
                case EQ:
                    result = std::abs(a - b) < 0.0001f;
                    break;
                case NE:
                    result = std::abs(a - b) >= 0.0001f;
                    break;
                case LT:
                    result = a < b;
                    break;
                case LE:
                    result = a <= b;
                    break;
                case GT:
                    result = a > b;
                    break;
                case GE:
                    result = a >= b;
                    break;
                }
                outputs.resize(1);
                outputs[0] = result;
                return true;
            }
        };

        // --- Flow control ---
        struct BranchNode : ScriptNode
        {
            BranchNode()
            {
                type = ScriptNodeType::BRANCH;
                category = ScriptNodeCategory::FLOW_CONTROL;
                name = "Branch";
                description = "If/else branching";
                inputSockets.push_back({"Exec", ScriptVariableType::EXECUTION, true, true, false, "Execution input"});
                inputSockets.push_back(
                    {"Condition", ScriptVariableType::BOOLEAN, true, false, false, "Branch condition"});
                outputSockets.push_back({"True", ScriptVariableType::EXECUTION, false, true, false, "True branch"});
                outputSockets.push_back({"False", ScriptVariableType::EXECUTION, false, true, false, "False branch"});
            }
            bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                bool condition = inputs.size() > 1 ? ToBool(inputs[1]) : false;
                outputs.resize(2);
                outputs[0] = condition;  // True branch active
                outputs[1] = !condition; // False branch active
                return true;
            }
        };

        struct SequenceNode : ScriptNode
        {
            SequenceNode()
            {
                type = ScriptNodeType::SEQUENCE;
                category = ScriptNodeCategory::FLOW_CONTROL;
                name = "Sequence";
                description = "Execute outputs in order";
                inputSockets.push_back({"Exec", ScriptVariableType::EXECUTION, true, true, false, "Execution input"});
                outputSockets.push_back({"Then 0", ScriptVariableType::EXECUTION, false, true, false, "First output"});
                outputSockets.push_back({"Then 1", ScriptVariableType::EXECUTION, false, true, false, "Second output"});
            }
            bool Execute(const std::vector<ScriptValue>&, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                outputs.resize(2);
                outputs[0] = true;
                outputs[1] = true;
                return true;
            }
        };

        // --- String nodes ---
        struct StringConcatNode : ScriptNode
        {
            StringConcatNode()
            {
                type = ScriptNodeType::STRING_CONCAT;
                category = ScriptNodeCategory::STRING;
                name = "String Concat";
                inputSockets.push_back({"A", ScriptVariableType::STRING, true, false, std::string(""), "First string"});
                inputSockets.push_back(
                    {"B", ScriptVariableType::STRING, true, false, std::string(""), "Second string"});
                outputSockets.push_back(
                    {"Result", ScriptVariableType::STRING, false, false, std::string(""), "Concatenated string"});
            }
            bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                std::string a = inputs.size() > 0 ? ToString(inputs[0]) : "";
                std::string b = inputs.size() > 1 ? ToString(inputs[1]) : "";
                outputs.resize(1);
                outputs[0] = a + b;
                return true;
            }
        };

        struct StringLengthNode : ScriptNode
        {
            StringLengthNode()
            {
                type = ScriptNodeType::STRING_LENGTH;
                category = ScriptNodeCategory::STRING;
                name = "String Length";
                inputSockets.push_back(
                    {"String", ScriptVariableType::STRING, true, false, std::string(""), "Input string"});
                outputSockets.push_back({"Length", ScriptVariableType::INTEGER, false, false, 0, "String length"});
            }
            bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                std::string s = inputs.size() > 0 ? ToString(inputs[0]) : "";
                outputs.resize(1);
                outputs[0] = static_cast<int>(s.length());
                return true;
            }
        };

        // --- Variable nodes ---
        struct GetVariableNode : ScriptNode
        {
            GetVariableNode()
            {
                type = ScriptNodeType::GET_VARIABLE;
                category = ScriptNodeCategory::VARIABLE;
                name = "Get Variable";
                inputSockets.push_back(
                    {"Name", ScriptVariableType::STRING, true, false, std::string(""), "Variable name"});
                outputSockets.push_back({"Value", ScriptVariableType::FLOAT, false, false, 0.0f, "Variable value"});
            }
            bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext* ctx) override
            {
                std::string varName = inputs.size() > 0 ? ToString(inputs[0]) : "";
                outputs.resize(1);
                if (ctx && !varName.empty())
                {
                    outputs[0] = ctx->GetVariable(varName);
                }
                else
                {
                    outputs[0] = 0.0f;
                }
                return true;
            }
        };

        struct SetVariableNode : ScriptNode
        {
            SetVariableNode()
            {
                type = ScriptNodeType::SET_VARIABLE;
                category = ScriptNodeCategory::VARIABLE;
                name = "Set Variable";
                inputSockets.push_back({"Exec", ScriptVariableType::EXECUTION, true, true, false, "Execution input"});
                inputSockets.push_back(
                    {"Name", ScriptVariableType::STRING, true, false, std::string(""), "Variable name"});
                inputSockets.push_back({"Value", ScriptVariableType::FLOAT, true, false, 0.0f, "Value to set"});
                outputSockets.push_back(
                    {"Exec", ScriptVariableType::EXECUTION, false, true, false, "Execution output"});
            }
            bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext* ctx) override
            {
                std::string varName = inputs.size() > 1 ? ToString(inputs[1]) : "";
                if (ctx && !varName.empty() && inputs.size() > 2)
                {
                    ctx->SetVariable(varName, inputs[2]);
                }
                outputs.resize(1);
                outputs[0] = true;
                return true;
            }
        };

        // --- For loop ---
        struct ForLoopNode : ScriptNode
        {
            ForLoopNode()
            {
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
            bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                // For loop execution is handled specially by the executor
                outputs.resize(3);
                outputs[0] = true;
                outputs[1] = inputs.size() > 1 ? inputs[1] : ScriptValue(0);
                outputs[2] = true;
                return true;
            }
        };

        // --- While loop ---
        struct WhileLoopNode : ScriptNode
        {
            WhileLoopNode()
            {
                type = ScriptNodeType::WHILE_LOOP;
                category = ScriptNodeCategory::FLOW_CONTROL;
                name = "While Loop";
                description = "Loops while condition is true";
                inputSockets.push_back({"Exec", ScriptVariableType::EXECUTION, true, true, false, "Execution input"});
                inputSockets.push_back({"Condition", ScriptVariableType::BOOLEAN, true, false, true, "Loop condition"});
                outputSockets.push_back({"Loop Body", ScriptVariableType::EXECUTION, false, true, false, "Loop body"});
                outputSockets.push_back({"Completed", ScriptVariableType::EXECUTION, false, true, false, "After loop"});
            }
            bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                bool condition = inputs.size() > 1 ? ToBool(inputs[1]) : false;
                outputs.resize(2);
                outputs[0] = condition;  // Loop body active while condition true
                outputs[1] = !condition; // Completed when condition false
                return true;
            }
        };

        // --- Delay node ---
        struct DelayNode : ScriptNode
        {
            float m_elapsed = 0.0f;
            DelayNode()
            {
                type = ScriptNodeType::DELAY;
                category = ScriptNodeCategory::FLOW_CONTROL;
                name = "Delay";
                description = "Outputs after delay timer";
                inputSockets.push_back({"Exec", ScriptVariableType::EXECUTION, true, true, false, "Execution input"});
                inputSockets.push_back({"Duration", ScriptVariableType::FLOAT, true, false, 1.0f, "Delay in seconds"});
                outputSockets.push_back(
                    {"Completed", ScriptVariableType::EXECUTION, false, true, false, "After delay"});
            }
            bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext* ctx) override
            {
                float duration = inputs.size() > 1 ? ToFloat(inputs[1]) : 1.0f;
                float dt = ctx ? ctx->GetDeltaTime() : 0.016f;
                m_elapsed += dt;
                outputs.resize(1);
                outputs[0] = (m_elapsed >= duration);
                if (m_elapsed >= duration)
                {
                    m_elapsed = 0.0f;
                }
                return true;
            }
        };

        // --- Gate node ---
        struct GateNode : ScriptNode
        {
            bool m_isOpen = true;
            GateNode()
            {
                type = ScriptNodeType::GATE;
                category = ScriptNodeCategory::FLOW_CONTROL;
                name = "Gate";
                description = "Pass-through gate with open/close";
                inputSockets.push_back({"Exec", ScriptVariableType::EXECUTION, true, true, false, "Execution input"});
                inputSockets.push_back({"Open", ScriptVariableType::BOOLEAN, true, false, true, "Open the gate"});
                inputSockets.push_back({"Close", ScriptVariableType::BOOLEAN, true, false, false, "Close the gate"});
                inputSockets.push_back(
                    {"Toggle", ScriptVariableType::BOOLEAN, true, false, false, "Toggle gate state"});
                outputSockets.push_back(
                    {"Exec", ScriptVariableType::EXECUTION, false, true, false, "Output (when open)"});
            }
            bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                bool open = inputs.size() > 1 ? ToBool(inputs[1]) : false;
                bool close = inputs.size() > 2 ? ToBool(inputs[2]) : false;
                bool toggle = inputs.size() > 3 ? ToBool(inputs[3]) : false;
                if (open)
                    m_isOpen = true;
                if (close)
                    m_isOpen = false;
                if (toggle)
                    m_isOpen = !m_isOpen;
                outputs.resize(1);
                outputs[0] = m_isOpen;
                return true;
            }
        };

        // --- FlipFlop node ---
        struct FlipFlopNode : ScriptNode
        {
            bool m_isA = true;
            FlipFlopNode()
            {
                type = ScriptNodeType::FLIP_FLOP;
                category = ScriptNodeCategory::FLOW_CONTROL;
                name = "Flip Flop";
                description = "Alternates between A and B outputs";
                inputSockets.push_back({"Exec", ScriptVariableType::EXECUTION, true, true, false, "Execution input"});
                outputSockets.push_back({"A", ScriptVariableType::EXECUTION, false, true, false, "First output"});
                outputSockets.push_back({"B", ScriptVariableType::EXECUTION, false, true, false, "Second output"});
                outputSockets.push_back(
                    {"Is A", ScriptVariableType::BOOLEAN, false, false, true, "True when A is active"});
            }
            bool Execute(const std::vector<ScriptValue>&, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                outputs.resize(3);
                outputs[0] = m_isA;
                outputs[1] = !m_isA;
                outputs[2] = m_isA;
                m_isA = !m_isA;
                return true;
            }
        };

        // --- Switch node ---
        struct SwitchNode : ScriptNode
        {
            SwitchNode()
            {
                type = ScriptNodeType::SWITCH;
                category = ScriptNodeCategory::FLOW_CONTROL;
                name = "Switch";
                description = "Multi-way branching on integer value";
                inputSockets.push_back({"Exec", ScriptVariableType::EXECUTION, true, true, false, "Execution input"});
                inputSockets.push_back({"Selection", ScriptVariableType::INTEGER, true, false, 0, "Switch value"});
                outputSockets.push_back({"Case 0", ScriptVariableType::EXECUTION, false, true, false, "Case 0"});
                outputSockets.push_back({"Case 1", ScriptVariableType::EXECUTION, false, true, false, "Case 1"});
                outputSockets.push_back({"Case 2", ScriptVariableType::EXECUTION, false, true, false, "Case 2"});
                outputSockets.push_back({"Case 3", ScriptVariableType::EXECUTION, false, true, false, "Case 3"});
                outputSockets.push_back({"Default", ScriptVariableType::EXECUTION, false, true, false, "Default case"});
            }
            bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                int selection = inputs.size() > 1 ? ToInt(inputs[1]) : 0;
                outputs.resize(5);
                for (int i = 0; i < 5; ++i)
                {
                    outputs[i] = false;
                }
                if (selection >= 0 && selection < 4)
                {
                    outputs[selection] = true;
                }
                else
                {
                    outputs[4] = true; // Default
                }
                return true;
            }
        };

        // --- Additional String nodes ---
        struct SubstringNode : ScriptNode
        {
            SubstringNode()
            {
                type = ScriptNodeType::STRING_SUBSTRING;
                category = ScriptNodeCategory::STRING;
                name = "Substring";
                inputSockets.push_back(
                    {"String", ScriptVariableType::STRING, true, false, std::string(""), "Input string"});
                inputSockets.push_back({"Start", ScriptVariableType::INTEGER, true, false, 0, "Start index"});
                inputSockets.push_back(
                    {"Length", ScriptVariableType::INTEGER, true, false, -1, "Length (-1 for rest)"});
                outputSockets.push_back(
                    {"Result", ScriptVariableType::STRING, false, false, std::string(""), "Substring result"});
            }
            bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                std::string s = inputs.size() > 0 ? ToString(inputs[0]) : "";
                int start = inputs.size() > 1 ? ToInt(inputs[1]) : 0;
                int length = inputs.size() > 2 ? ToInt(inputs[2]) : -1;
                outputs.resize(1);
                if (start < 0 || start >= static_cast<int>(s.length()))
                {
                    outputs[0] = std::string("");
                }
                else if (length < 0)
                {
                    outputs[0] = s.substr(static_cast<size_t>(start));
                }
                else
                {
                    outputs[0] = s.substr(static_cast<size_t>(start), static_cast<size_t>(length));
                }
                return true;
            }
        };

        struct StringContainsNode : ScriptNode
        {
            StringContainsNode()
            {
                type = ScriptNodeType::STRING_CONTAINS;
                category = ScriptNodeCategory::STRING;
                name = "String Contains";
                inputSockets.push_back(
                    {"String", ScriptVariableType::STRING, true, false, std::string(""), "Input string"});
                inputSockets.push_back(
                    {"Search", ScriptVariableType::STRING, true, false, std::string(""), "String to search for"});
                outputSockets.push_back(
                    {"Result", ScriptVariableType::BOOLEAN, false, false, false, "Contains result"});
            }
            bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                std::string s = inputs.size() > 0 ? ToString(inputs[0]) : "";
                std::string search = inputs.size() > 1 ? ToString(inputs[1]) : "";
                outputs.resize(1);
                outputs[0] = (s.find(search) != std::string::npos);
                return true;
            }
        };

        struct StringReplaceNode : ScriptNode
        {
            StringReplaceNode()
            {
                type = ScriptNodeType::STRING_REPLACE;
                category = ScriptNodeCategory::STRING;
                name = "String Replace";
                inputSockets.push_back(
                    {"String", ScriptVariableType::STRING, true, false, std::string(""), "Input string"});
                inputSockets.push_back(
                    {"Find", ScriptVariableType::STRING, true, false, std::string(""), "String to find"});
                inputSockets.push_back(
                    {"Replace", ScriptVariableType::STRING, true, false, std::string(""), "Replacement string"});
                outputSockets.push_back(
                    {"Result", ScriptVariableType::STRING, false, false, std::string(""), "Replaced string"});
            }
            bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                std::string s = inputs.size() > 0 ? ToString(inputs[0]) : "";
                std::string find = inputs.size() > 1 ? ToString(inputs[1]) : "";
                std::string replace = inputs.size() > 2 ? ToString(inputs[2]) : "";
                outputs.resize(1);
                if (!find.empty())
                {
                    std::string result = s;
                    size_t pos = 0;
                    while ((pos = result.find(find, pos)) != std::string::npos)
                    {
                        result.replace(pos, find.length(), replace);
                        pos += replace.length();
                    }
                    outputs[0] = result;
                }
                else
                {
                    outputs[0] = s;
                }
                return true;
            }
        };

        struct StringToUpperNode : ScriptNode
        {
            StringToUpperNode()
            {
                type = ScriptNodeType::STRING_TO_UPPER;
                category = ScriptNodeCategory::STRING;
                name = "To Upper";
                inputSockets.push_back(
                    {"String", ScriptVariableType::STRING, true, false, std::string(""), "Input string"});
                outputSockets.push_back(
                    {"Result", ScriptVariableType::STRING, false, false, std::string(""), "Uppercase string"});
            }
            bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                std::string s = inputs.size() > 0 ? ToString(inputs[0]) : "";
                std::transform(s.begin(), s.end(), s.begin(),
                               [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
                outputs.resize(1);
                outputs[0] = s;
                return true;
            }
        };

        struct StringToLowerNode : ScriptNode
        {
            StringToLowerNode()
            {
                type = ScriptNodeType::STRING_TO_LOWER;
                category = ScriptNodeCategory::STRING;
                name = "To Lower";
                inputSockets.push_back(
                    {"String", ScriptVariableType::STRING, true, false, std::string(""), "Input string"});
                outputSockets.push_back(
                    {"Result", ScriptVariableType::STRING, false, false, std::string(""), "Lowercase string"});
            }
            bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                std::string s = inputs.size() > 0 ? ToString(inputs[0]) : "";
                std::transform(s.begin(), s.end(), s.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                outputs.resize(1);
                outputs[0] = s;
                return true;
            }
        };

        // --- Array nodes ---
        static std::shared_ptr<ScriptValueArray> ToArray(const ScriptValue& v)
        {
            if (auto* arr = std::get_if<std::shared_ptr<ScriptValueArray>>(&v))
                return *arr;
            return std::make_shared<ScriptValueArray>();
        }

        struct ArrayGetNode : ScriptNode
        {
            ArrayGetNode()
            {
                type = ScriptNodeType::ARRAY_GET;
                category = ScriptNodeCategory::ARRAY;
                name = "Array Get";
                inputSockets.push_back({"Array", ScriptVariableType::ARRAY, true, false,
                                        std::make_shared<ScriptValueArray>(), "Input array"});
                inputSockets.push_back({"Index", ScriptVariableType::INTEGER, true, false, 0, "Element index"});
                outputSockets.push_back({"Value", ScriptVariableType::FLOAT, false, false, 0.0f, "Element value"});
                outputSockets.push_back(
                    {"Valid", ScriptVariableType::BOOLEAN, false, false, false, "Whether index is valid"});
            }
            bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                auto arr = inputs.size() > 0 ? ToArray(inputs[0]) : std::make_shared<ScriptValueArray>();
                int idx = inputs.size() > 1 ? ToInt(inputs[1]) : 0;
                outputs.resize(2);
                if (arr && idx >= 0 && idx < static_cast<int>(arr->values.size()))
                {
                    outputs[0] = arr->values[static_cast<size_t>(idx)];
                    outputs[1] = true;
                }
                else
                {
                    outputs[0] = 0.0f;
                    outputs[1] = false;
                }
                return true;
            }
        };

        struct ArraySetNode : ScriptNode
        {
            ArraySetNode()
            {
                type = ScriptNodeType::ARRAY_SET;
                category = ScriptNodeCategory::ARRAY;
                name = "Array Set";
                inputSockets.push_back({"Exec", ScriptVariableType::EXECUTION, true, true, false, "Execution input"});
                inputSockets.push_back({"Array", ScriptVariableType::ARRAY, true, false,
                                        std::make_shared<ScriptValueArray>(), "Input array"});
                inputSockets.push_back({"Index", ScriptVariableType::INTEGER, true, false, 0, "Element index"});
                inputSockets.push_back({"Value", ScriptVariableType::FLOAT, true, false, 0.0f, "Value to set"});
                outputSockets.push_back(
                    {"Exec", ScriptVariableType::EXECUTION, false, true, false, "Execution output"});
                outputSockets.push_back({"Array", ScriptVariableType::ARRAY, false, false,
                                         std::make_shared<ScriptValueArray>(), "Modified array"});
            }
            bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                auto arr = inputs.size() > 1 ? ToArray(inputs[1]) : std::make_shared<ScriptValueArray>();
                int idx = inputs.size() > 2 ? ToInt(inputs[2]) : 0;
                auto result = std::make_shared<ScriptValueArray>(*arr);
                if (idx >= 0 && idx < static_cast<int>(result->values.size()) && inputs.size() > 3)
                {
                    result->values[static_cast<size_t>(idx)] = inputs[3];
                }
                outputs.resize(2);
                outputs[0] = true;
                outputs[1] = ScriptValue(result);
                return true;
            }
        };

        struct ArrayAddNode : ScriptNode
        {
            ArrayAddNode()
            {
                type = ScriptNodeType::ARRAY_ADD;
                category = ScriptNodeCategory::ARRAY;
                name = "Array Add";
                inputSockets.push_back({"Exec", ScriptVariableType::EXECUTION, true, true, false, "Execution input"});
                inputSockets.push_back({"Array", ScriptVariableType::ARRAY, true, false,
                                        std::make_shared<ScriptValueArray>(), "Input array"});
                inputSockets.push_back({"Value", ScriptVariableType::FLOAT, true, false, 0.0f, "Value to add"});
                outputSockets.push_back(
                    {"Exec", ScriptVariableType::EXECUTION, false, true, false, "Execution output"});
                outputSockets.push_back({"Array", ScriptVariableType::ARRAY, false, false,
                                         std::make_shared<ScriptValueArray>(), "Modified array"});
                outputSockets.push_back(
                    {"New Length", ScriptVariableType::INTEGER, false, false, 0, "Array length after add"});
            }
            bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                auto arr = inputs.size() > 1 ? ToArray(inputs[1]) : std::make_shared<ScriptValueArray>();
                auto result = std::make_shared<ScriptValueArray>(*arr);
                if (inputs.size() > 2)
                {
                    result->values.push_back(inputs[2]);
                }
                outputs.resize(3);
                outputs[0] = true;
                outputs[1] = ScriptValue(result);
                outputs[2] = static_cast<int>(result->values.size());
                return true;
            }
        };

        struct ArrayRemoveNode : ScriptNode
        {
            ArrayRemoveNode()
            {
                type = ScriptNodeType::ARRAY_REMOVE;
                category = ScriptNodeCategory::ARRAY;
                name = "Array Remove";
                inputSockets.push_back({"Exec", ScriptVariableType::EXECUTION, true, true, false, "Execution input"});
                inputSockets.push_back({"Array", ScriptVariableType::ARRAY, true, false,
                                        std::make_shared<ScriptValueArray>(), "Input array"});
                inputSockets.push_back({"Index", ScriptVariableType::INTEGER, true, false, 0, "Index to remove"});
                outputSockets.push_back(
                    {"Exec", ScriptVariableType::EXECUTION, false, true, false, "Execution output"});
                outputSockets.push_back({"Array", ScriptVariableType::ARRAY, false, false,
                                         std::make_shared<ScriptValueArray>(), "Modified array"});
                outputSockets.push_back(
                    {"Removed", ScriptVariableType::BOOLEAN, false, false, false, "Whether removal succeeded"});
            }
            bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                auto arr = inputs.size() > 1 ? ToArray(inputs[1]) : std::make_shared<ScriptValueArray>();
                int idx = inputs.size() > 2 ? ToInt(inputs[2]) : 0;
                auto result = std::make_shared<ScriptValueArray>(*arr);
                bool removed = false;
                if (idx >= 0 && idx < static_cast<int>(result->values.size()))
                {
                    result->values.erase(result->values.begin() + idx);
                    removed = true;
                }
                outputs.resize(3);
                outputs[0] = true;
                outputs[1] = ScriptValue(result);
                outputs[2] = removed;
                return true;
            }
        };

        struct ArrayLengthNode : ScriptNode
        {
            ArrayLengthNode()
            {
                type = ScriptNodeType::ARRAY_LENGTH;
                category = ScriptNodeCategory::ARRAY;
                name = "Array Length";
                inputSockets.push_back({"Array", ScriptVariableType::ARRAY, true, false,
                                        std::make_shared<ScriptValueArray>(), "Input array"});
                outputSockets.push_back({"Length", ScriptVariableType::INTEGER, false, false, 0, "Array length"});
            }
            bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                auto arr = inputs.size() > 0 ? ToArray(inputs[0]) : std::make_shared<ScriptValueArray>();
                outputs.resize(1);
                outputs[0] = static_cast<int>(arr->values.size());
                return true;
            }
        };

        struct ArrayContainsNode : ScriptNode
        {
            ArrayContainsNode()
            {
                type = ScriptNodeType::ARRAY_CONTAINS;
                category = ScriptNodeCategory::ARRAY;
                name = "Array Contains";
                inputSockets.push_back({"Array", ScriptVariableType::ARRAY, true, false,
                                        std::make_shared<ScriptValueArray>(), "Input array"});
                inputSockets.push_back({"Value", ScriptVariableType::FLOAT, true, false, 0.0f, "Value to find"});
                outputSockets.push_back(
                    {"Contains", ScriptVariableType::BOOLEAN, false, false, false, "Whether array contains value"});
            }
            bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                auto arr = inputs.size() > 0 ? ToArray(inputs[0]) : std::make_shared<ScriptValueArray>();
                float target = inputs.size() > 1 ? ToFloat(inputs[1]) : 0.0f;
                bool found = false;
                for (const auto& val : arr->values)
                {
                    if (std::abs(ToFloat(val) - target) < 0.0001f)
                    {
                        found = true;
                        break;
                    }
                }
                outputs.resize(1);
                outputs[0] = found;
                return true;
            }
        };

        struct ArrayFindNode : ScriptNode
        {
            ArrayFindNode()
            {
                type = ScriptNodeType::ARRAY_FIND;
                category = ScriptNodeCategory::ARRAY;
                name = "Array Find";
                inputSockets.push_back({"Array", ScriptVariableType::ARRAY, true, false,
                                        std::make_shared<ScriptValueArray>(), "Input array"});
                inputSockets.push_back({"Value", ScriptVariableType::FLOAT, true, false, 0.0f, "Value to find"});
                outputSockets.push_back(
                    {"Index", ScriptVariableType::INTEGER, false, false, -1, "Index of value (-1 if not found)"});
                outputSockets.push_back(
                    {"Found", ScriptVariableType::BOOLEAN, false, false, false, "Whether value was found"});
            }
            bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                auto arr = inputs.size() > 0 ? ToArray(inputs[0]) : std::make_shared<ScriptValueArray>();
                float target = inputs.size() > 1 ? ToFloat(inputs[1]) : 0.0f;
                int foundIdx = -1;
                for (int i = 0; i < static_cast<int>(arr->values.size()); ++i)
                {
                    if (std::abs(ToFloat(arr->values[static_cast<size_t>(i)]) - target) < 0.0001f)
                    {
                        foundIdx = i;
                        break;
                    }
                }
                outputs.resize(2);
                outputs[0] = foundIdx;
                outputs[1] = (foundIdx >= 0);
                return true;
            }
        };

        // --- Object nodes ---
        struct GetComponentNode : ScriptNode
        {
            GetComponentNode()
            {
                type = ScriptNodeType::GET_COMPONENT;
                category = ScriptNodeCategory::OBJECT;
                name = "Get Component";
                description = "Get a component from an object";
                inputSockets.push_back(
                    {"Object", ScriptVariableType::OBJECT_REFERENCE, true, false, ObjectID{0}, "Target object"});
                inputSockets.push_back(
                    {"Type", ScriptVariableType::STRING, true, false, std::string(""), "Component type name"});
                outputSockets.push_back({"Component", ScriptVariableType::COMPONENT_REFERENCE, true, false, ObjectID{0},
                                         "Component reference"});
                outputSockets.push_back(
                    {"Valid", ScriptVariableType::BOOLEAN, false, false, false, "Whether component exists"});
            }
            bool Execute(const std::vector<ScriptValue>&, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                // Component access requires engine integration
                outputs.resize(2);
                outputs[0] = ObjectID{0};
                outputs[1] = false;
                return true;
            }
        };

        struct SetTransformNode : ScriptNode
        {
            SetTransformNode()
            {
                type = ScriptNodeType::SET_TRANSFORM;
                category = ScriptNodeCategory::OBJECT;
                name = "Set Transform";
                description = "Set object position, rotation, scale";
                inputSockets.push_back({"Exec", ScriptVariableType::EXECUTION, true, true, false, "Execution input"});
                inputSockets.push_back(
                    {"Object", ScriptVariableType::OBJECT_REFERENCE, true, false, ObjectID{0}, "Target object"});
                inputSockets.push_back(
                    {"Position", ScriptVariableType::VECTOR3, true, false, XMFLOAT3(0, 0, 0), "World position"});
                inputSockets.push_back(
                    {"Rotation", ScriptVariableType::VECTOR3, true, false, XMFLOAT3(0, 0, 0), "Euler rotation"});
                inputSockets.push_back({"Scale", ScriptVariableType::VECTOR3, true, false, XMFLOAT3(1, 1, 1), "Scale"});
                outputSockets.push_back(
                    {"Exec", ScriptVariableType::EXECUTION, false, true, false, "Execution output"});
            }
            bool Execute(const std::vector<ScriptValue>&, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                // Transform setting requires engine integration
                outputs.resize(1);
                outputs[0] = true;
                return true;
            }
        };

        struct GetTransformNode : ScriptNode
        {
            GetTransformNode()
            {
                type = ScriptNodeType::GET_TRANSFORM;
                category = ScriptNodeCategory::OBJECT;
                name = "Get Transform";
                description = "Get object position, rotation, scale";
                inputSockets.push_back(
                    {"Object", ScriptVariableType::OBJECT_REFERENCE, true, false, ObjectID{0}, "Target object"});
                outputSockets.push_back(
                    {"Position", ScriptVariableType::VECTOR3, false, false, XMFLOAT3(0, 0, 0), "World position"});
                outputSockets.push_back(
                    {"Rotation", ScriptVariableType::VECTOR3, false, false, XMFLOAT3(0, 0, 0), "Euler rotation"});
                outputSockets.push_back(
                    {"Scale", ScriptVariableType::VECTOR3, false, false, XMFLOAT3(1, 1, 1), "Scale"});
            }
            bool Execute(const std::vector<ScriptValue>&, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                // Transform retrieval requires engine integration
                outputs.resize(3);
                outputs[0] = XMFLOAT3(0, 0, 0);
                outputs[1] = XMFLOAT3(0, 0, 0);
                outputs[2] = XMFLOAT3(1, 1, 1);
                return true;
            }
        };

        struct DestroyObjectNode : ScriptNode
        {
            DestroyObjectNode()
            {
                type = ScriptNodeType::DESTROY_OBJECT;
                category = ScriptNodeCategory::OBJECT;
                name = "Destroy Object";
                description = "Destroy a game object";
                inputSockets.push_back({"Exec", ScriptVariableType::EXECUTION, true, true, false, "Execution input"});
                inputSockets.push_back(
                    {"Object", ScriptVariableType::OBJECT_REFERENCE, true, false, ObjectID{0}, "Object to destroy"});
                outputSockets.push_back(
                    {"Exec", ScriptVariableType::EXECUTION, false, true, false, "Execution output"});
            }
            bool Execute(const std::vector<ScriptValue>&, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                // Object destruction requires engine integration
                outputs.resize(1);
                outputs[0] = true;
                return true;
            }
        };

        struct InstantiateNode : ScriptNode
        {
            InstantiateNode()
            {
                type = ScriptNodeType::INSTANTIATE;
                category = ScriptNodeCategory::OBJECT;
                name = "Instantiate";
                description = "Create a new object from a template";
                inputSockets.push_back({"Exec", ScriptVariableType::EXECUTION, true, true, false, "Execution input"});
                inputSockets.push_back(
                    {"Template", ScriptVariableType::STRING, true, false, std::string(""), "Template/prefab name"});
                inputSockets.push_back(
                    {"Position", ScriptVariableType::VECTOR3, true, false, XMFLOAT3(0, 0, 0), "Spawn position"});
                inputSockets.push_back(
                    {"Rotation", ScriptVariableType::VECTOR3, true, false, XMFLOAT3(0, 0, 0), "Spawn rotation"});
                outputSockets.push_back(
                    {"Exec", ScriptVariableType::EXECUTION, false, true, false, "Execution output"});
                outputSockets.push_back(
                    {"Object", ScriptVariableType::OBJECT_REFERENCE, false, false, ObjectID{0}, "Created object"});
            }
            bool Execute(const std::vector<ScriptValue>&, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                // Instantiation requires engine integration
                outputs.resize(2);
                outputs[0] = true;
                outputs[1] = ObjectID{0};
                return true;
            }
        };

        struct FindObjectNode : ScriptNode
        {
            FindObjectNode()
            {
                type = ScriptNodeType::FIND_OBJECT;
                category = ScriptNodeCategory::OBJECT;
                name = "Find Object";
                description = "Find an object by name or tag";
                inputSockets.push_back(
                    {"Name", ScriptVariableType::STRING, true, false, std::string(""), "Object name to find"});
                outputSockets.push_back(
                    {"Object", ScriptVariableType::OBJECT_REFERENCE, false, false, ObjectID{0}, "Found object"});
                outputSockets.push_back(
                    {"Found", ScriptVariableType::BOOLEAN, false, false, false, "Whether object was found"});
            }
            bool Execute(const std::vector<ScriptValue>&, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                // Object search requires engine integration
                outputs.resize(2);
                outputs[0] = ObjectID{0};
                outputs[1] = false;
                return true;
            }
        };

        // --- Input nodes ---
        struct InputKeyDownNode : ScriptNode
        {
            InputKeyDownNode()
            {
                type = ScriptNodeType::INPUT_KEY_DOWN;
                category = ScriptNodeCategory::INPUT;
                name = "Key Down";
                description = "Check if a key is currently pressed";
                inputSockets.push_back(
                    {"Key", ScriptVariableType::STRING, true, false, std::string("Space"), "Key name"});
                outputSockets.push_back(
                    {"Is Down", ScriptVariableType::BOOLEAN, false, false, false, "Whether key is pressed"});
            }
            bool Execute(const std::vector<ScriptValue>&, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                // Input polling requires engine integration
                outputs.resize(1);
                outputs[0] = false;
                return true;
            }
        };

        struct InputKeyUpNode : ScriptNode
        {
            InputKeyUpNode()
            {
                type = ScriptNodeType::INPUT_KEY_UP;
                category = ScriptNodeCategory::INPUT;
                name = "Key Up";
                description = "Check if a key was released this frame";
                inputSockets.push_back(
                    {"Key", ScriptVariableType::STRING, true, false, std::string("Space"), "Key name"});
                outputSockets.push_back(
                    {"Is Up", ScriptVariableType::BOOLEAN, false, false, false, "Whether key was released"});
            }
            bool Execute(const std::vector<ScriptValue>&, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                outputs.resize(1);
                outputs[0] = false;
                return true;
            }
        };

        struct InputMouseButtonNode : ScriptNode
        {
            InputMouseButtonNode()
            {
                type = ScriptNodeType::INPUT_MOUSE_BUTTON;
                category = ScriptNodeCategory::INPUT;
                name = "Mouse Button";
                description = "Check if a mouse button is pressed";
                inputSockets.push_back({"Button", ScriptVariableType::INTEGER, true, false, 0,
                                        "Mouse button (0=left, 1=right, 2=middle)"});
                outputSockets.push_back(
                    {"Is Down", ScriptVariableType::BOOLEAN, false, false, false, "Whether button is pressed"});
            }
            bool Execute(const std::vector<ScriptValue>&, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                outputs.resize(1);
                outputs[0] = false;
                return true;
            }
        };

        struct InputMousePositionNode : ScriptNode
        {
            InputMousePositionNode()
            {
                type = ScriptNodeType::INPUT_MOUSE_POSITION;
                category = ScriptNodeCategory::INPUT;
                name = "Mouse Position";
                description = "Get current mouse cursor position";
                outputSockets.push_back(
                    {"Position", ScriptVariableType::VECTOR2, false, false, XMFLOAT2(0, 0), "Mouse position"});
                outputSockets.push_back({"X", ScriptVariableType::FLOAT, false, false, 0.0f, "Mouse X"});
                outputSockets.push_back({"Y", ScriptVariableType::FLOAT, false, false, 0.0f, "Mouse Y"});
            }
            bool Execute(const std::vector<ScriptValue>&, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                outputs.resize(3);
                outputs[0] = XMFLOAT2(0.0f, 0.0f);
                outputs[1] = 0.0f;
                outputs[2] = 0.0f;
                return true;
            }
        };

        struct InputAxisNode : ScriptNode
        {
            InputAxisNode()
            {
                type = ScriptNodeType::INPUT_AXIS;
                category = ScriptNodeCategory::INPUT;
                name = "Input Axis";
                description = "Get input axis value (-1 to 1)";
                inputSockets.push_back(
                    {"Axis", ScriptVariableType::STRING, true, false, std::string("Horizontal"), "Axis name"});
                outputSockets.push_back({"Value", ScriptVariableType::FLOAT, false, false, 0.0f, "Axis value"});
            }
            bool Execute(const std::vector<ScriptValue>&, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                outputs.resize(1);
                outputs[0] = 0.0f;
                return true;
            }
        };

        // --- Audio nodes ---
        struct PlaySoundNode : ScriptNode
        {
            PlaySoundNode()
            {
                type = ScriptNodeType::PLAY_SOUND;
                category = ScriptNodeCategory::AUDIO;
                name = "Play Sound";
                description = "Play an audio clip";
                inputSockets.push_back({"Exec", ScriptVariableType::EXECUTION, true, true, false, "Execution input"});
                inputSockets.push_back(
                    {"Sound", ScriptVariableType::STRING, true, false, std::string(""), "Sound asset name"});
                inputSockets.push_back(
                    {"Volume", ScriptVariableType::FLOAT, true, false, 1.0f, "Playback volume (0-1)"});
                inputSockets.push_back({"Loop", ScriptVariableType::BOOLEAN, true, false, false, "Whether to loop"});
                outputSockets.push_back(
                    {"Exec", ScriptVariableType::EXECUTION, false, true, false, "Execution output"});
            }
            bool Execute(const std::vector<ScriptValue>&, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                // Audio playback requires engine integration
                outputs.resize(1);
                outputs[0] = true;
                return true;
            }
        };

        struct StopSoundNode : ScriptNode
        {
            StopSoundNode()
            {
                type = ScriptNodeType::STOP_SOUND;
                category = ScriptNodeCategory::AUDIO;
                name = "Stop Sound";
                description = "Stop a playing sound";
                inputSockets.push_back({"Exec", ScriptVariableType::EXECUTION, true, true, false, "Execution input"});
                inputSockets.push_back(
                    {"Sound", ScriptVariableType::STRING, true, false, std::string(""), "Sound asset name"});
                outputSockets.push_back(
                    {"Exec", ScriptVariableType::EXECUTION, false, true, false, "Execution output"});
            }
            bool Execute(const std::vector<ScriptValue>&, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                outputs.resize(1);
                outputs[0] = true;
                return true;
            }
        };

        struct SetVolumeNode : ScriptNode
        {
            SetVolumeNode()
            {
                type = ScriptNodeType::SET_VOLUME;
                category = ScriptNodeCategory::AUDIO;
                name = "Set Volume";
                description = "Set volume for a sound";
                inputSockets.push_back({"Exec", ScriptVariableType::EXECUTION, true, true, false, "Execution input"});
                inputSockets.push_back(
                    {"Sound", ScriptVariableType::STRING, true, false, std::string(""), "Sound asset name"});
                inputSockets.push_back({"Volume", ScriptVariableType::FLOAT, true, false, 1.0f, "Volume (0-1)"});
                outputSockets.push_back(
                    {"Exec", ScriptVariableType::EXECUTION, false, true, false, "Execution output"});
            }
            bool Execute(const std::vector<ScriptValue>&, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                outputs.resize(1);
                outputs[0] = true;
                return true;
            }
        };

        // --- Graphics nodes ---
        struct SetMaterialNode : ScriptNode
        {
            SetMaterialNode()
            {
                type = ScriptNodeType::SET_MATERIAL;
                category = ScriptNodeCategory::GRAPHICS;
                name = "Set Material";
                description = "Set material on an object";
                inputSockets.push_back({"Exec", ScriptVariableType::EXECUTION, true, true, false, "Execution input"});
                inputSockets.push_back(
                    {"Object", ScriptVariableType::OBJECT_REFERENCE, true, false, ObjectID{0}, "Target object"});
                inputSockets.push_back(
                    {"Material", ScriptVariableType::STRING, true, false, std::string(""), "Material asset name"});
                outputSockets.push_back(
                    {"Exec", ScriptVariableType::EXECUTION, false, true, false, "Execution output"});
            }
            bool Execute(const std::vector<ScriptValue>&, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                outputs.resize(1);
                outputs[0] = true;
                return true;
            }
        };

        struct SetColorNode : ScriptNode
        {
            SetColorNode()
            {
                type = ScriptNodeType::SET_COLOR;
                category = ScriptNodeCategory::GRAPHICS;
                name = "Set Color";
                description = "Set color on an object's material";
                inputSockets.push_back({"Exec", ScriptVariableType::EXECUTION, true, true, false, "Execution input"});
                inputSockets.push_back(
                    {"Object", ScriptVariableType::OBJECT_REFERENCE, true, false, ObjectID{0}, "Target object"});
                inputSockets.push_back(
                    {"Color", ScriptVariableType::VECTOR4, true, false, XMFLOAT4(1, 1, 1, 1), "RGBA color"});
                outputSockets.push_back(
                    {"Exec", ScriptVariableType::EXECUTION, false, true, false, "Execution output"});
            }
            bool Execute(const std::vector<ScriptValue>&, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                outputs.resize(1);
                outputs[0] = true;
                return true;
            }
        };

        struct SetVisibilityNode : ScriptNode
        {
            SetVisibilityNode()
            {
                type = ScriptNodeType::SET_VISIBILITY;
                category = ScriptNodeCategory::GRAPHICS;
                name = "Set Visibility";
                description = "Show or hide an object";
                inputSockets.push_back({"Exec", ScriptVariableType::EXECUTION, true, true, false, "Execution input"});
                inputSockets.push_back(
                    {"Object", ScriptVariableType::OBJECT_REFERENCE, true, false, ObjectID{0}, "Target object"});
                inputSockets.push_back({"Visible", ScriptVariableType::BOOLEAN, true, false, true, "Visibility state"});
                outputSockets.push_back(
                    {"Exec", ScriptVariableType::EXECUTION, false, true, false, "Execution output"});
            }
            bool Execute(const std::vector<ScriptValue>&, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                outputs.resize(1);
                outputs[0] = true;
                return true;
            }
        };

        // --- Physics nodes ---
        struct AddForceNode : ScriptNode
        {
            AddForceNode()
            {
                type = ScriptNodeType::ADD_FORCE;
                category = ScriptNodeCategory::PHYSICS;
                name = "Add Force";
                description = "Apply a force to a physics body";
                inputSockets.push_back({"Exec", ScriptVariableType::EXECUTION, true, true, false, "Execution input"});
                inputSockets.push_back(
                    {"Object", ScriptVariableType::OBJECT_REFERENCE, true, false, ObjectID{0}, "Target object"});
                inputSockets.push_back(
                    {"Force", ScriptVariableType::VECTOR3, true, false, XMFLOAT3(0, 0, 0), "Force vector"});
                outputSockets.push_back(
                    {"Exec", ScriptVariableType::EXECUTION, false, true, false, "Execution output"});
            }
            bool Execute(const std::vector<ScriptValue>&, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                outputs.resize(1);
                outputs[0] = true;
                return true;
            }
        };

        struct SetVelocityNode : ScriptNode
        {
            SetVelocityNode()
            {
                type = ScriptNodeType::SET_VELOCITY;
                category = ScriptNodeCategory::PHYSICS;
                name = "Set Velocity";
                description = "Set velocity on a physics body";
                inputSockets.push_back({"Exec", ScriptVariableType::EXECUTION, true, true, false, "Execution input"});
                inputSockets.push_back(
                    {"Object", ScriptVariableType::OBJECT_REFERENCE, true, false, ObjectID{0}, "Target object"});
                inputSockets.push_back(
                    {"Velocity", ScriptVariableType::VECTOR3, true, false, XMFLOAT3(0, 0, 0), "Velocity vector"});
                outputSockets.push_back(
                    {"Exec", ScriptVariableType::EXECUTION, false, true, false, "Execution output"});
            }
            bool Execute(const std::vector<ScriptValue>&, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                outputs.resize(1);
                outputs[0] = true;
                return true;
            }
        };

        struct RaycastNode : ScriptNode
        {
            RaycastNode()
            {
                type = ScriptNodeType::RAYCAST;
                category = ScriptNodeCategory::PHYSICS;
                name = "Raycast";
                description = "Cast a ray and check for hits";
                inputSockets.push_back(
                    {"Origin", ScriptVariableType::VECTOR3, true, false, XMFLOAT3(0, 0, 0), "Ray origin"});
                inputSockets.push_back(
                    {"Direction", ScriptVariableType::VECTOR3, true, false, XMFLOAT3(0, 0, -1), "Ray direction"});
                inputSockets.push_back(
                    {"Max Distance", ScriptVariableType::FLOAT, true, false, 1000.0f, "Maximum ray distance"});
                outputSockets.push_back(
                    {"Hit", ScriptVariableType::BOOLEAN, false, false, false, "Whether ray hit something"});
                outputSockets.push_back(
                    {"Hit Point", ScriptVariableType::VECTOR3, false, false, XMFLOAT3(0, 0, 0), "Hit point"});
                outputSockets.push_back(
                    {"Hit Normal", ScriptVariableType::VECTOR3, false, false, XMFLOAT3(0, 1, 0), "Hit surface normal"});
                outputSockets.push_back(
                    {"Hit Object", ScriptVariableType::OBJECT_REFERENCE, false, false, ObjectID{0}, "Hit object"});
                outputSockets.push_back({"Distance", ScriptVariableType::FLOAT, false, false, 0.0f, "Hit distance"});
            }
            bool Execute(const std::vector<ScriptValue>&, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                // Raycasting requires physics engine integration
                outputs.resize(5);
                outputs[0] = false;
                outputs[1] = XMFLOAT3(0, 0, 0);
                outputs[2] = XMFLOAT3(0, 1, 0);
                outputs[3] = ObjectID{0};
                outputs[4] = 0.0f;
                return true;
            }
        };

        // --- Function nodes ---
        struct FunctionCallNode : ScriptNode
        {
            FunctionCallNode()
            {
                type = ScriptNodeType::FUNCTION_CALL;
                category = ScriptNodeCategory::FUNCTION;
                name = "Function Call";
                description = "Call a script function";
                inputSockets.push_back({"Exec", ScriptVariableType::EXECUTION, true, true, false, "Execution input"});
                inputSockets.push_back(
                    {"Function", ScriptVariableType::STRING, true, false, std::string(""), "Function name"});
                outputSockets.push_back(
                    {"Exec", ScriptVariableType::EXECUTION, false, true, false, "Execution output"});
                outputSockets.push_back(
                    {"Return Value", ScriptVariableType::FLOAT, false, false, 0.0f, "Function return value"});
            }
            bool Execute(const std::vector<ScriptValue>&, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                // Function dispatch requires script graph lookup
                outputs.resize(2);
                outputs[0] = true;
                outputs[1] = 0.0f;
                return true;
            }
        };

        struct FunctionReturnNode : ScriptNode
        {
            FunctionReturnNode()
            {
                type = ScriptNodeType::FUNCTION_RETURN;
                category = ScriptNodeCategory::FUNCTION;
                name = "Function Return";
                description = "Return from a script function";
                inputSockets.push_back({"Exec", ScriptVariableType::EXECUTION, true, true, false, "Execution input"});
                inputSockets.push_back(
                    {"Return Value", ScriptVariableType::FLOAT, true, false, 0.0f, "Value to return"});
            }
            bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext* ctx) override
            {
                // Store return value in context
                if (ctx && inputs.size() > 1)
                {
                    ctx->SetVariable("__return", inputs[1]);
                }
                outputs.clear();
                return true;
            }
        };

        // --- Event nodes ---
        struct EventInputKeyNode : ScriptNode
        {
            EventInputKeyNode()
            {
                type = ScriptNodeType::EVENT_INPUT_KEY;
                category = ScriptNodeCategory::EVENT;
                name = "Event Input Key";
                description = "Fired when a key is pressed or released";
                inputSockets.push_back(
                    {"Key", ScriptVariableType::STRING, true, false, std::string("Space"), "Key to listen for"});
                outputSockets.push_back({"Pressed", ScriptVariableType::EXECUTION, false, true, false, "Key pressed"});
                outputSockets.push_back(
                    {"Released", ScriptVariableType::EXECUTION, false, true, false, "Key released"});
                outputSockets.push_back(
                    {"Key Name", ScriptVariableType::STRING, false, false, std::string(""), "Triggered key name"});
            }
            bool Execute(const std::vector<ScriptValue>&, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                outputs.resize(3);
                outputs[0] = false;
                outputs[1] = false;
                outputs[2] = std::string("");
                return true;
            }
        };

        struct EventInputMouseNode : ScriptNode
        {
            EventInputMouseNode()
            {
                type = ScriptNodeType::EVENT_INPUT_MOUSE;
                category = ScriptNodeCategory::EVENT;
                name = "Event Input Mouse";
                description = "Fired on mouse button events";
                outputSockets.push_back(
                    {"Pressed", ScriptVariableType::EXECUTION, false, true, false, "Mouse button pressed"});
                outputSockets.push_back(
                    {"Released", ScriptVariableType::EXECUTION, false, true, false, "Mouse button released"});
                outputSockets.push_back({"Button", ScriptVariableType::INTEGER, false, false, 0, "Mouse button index"});
                outputSockets.push_back(
                    {"Position", ScriptVariableType::VECTOR2, false, false, XMFLOAT2(0, 0), "Mouse position"});
            }
            bool Execute(const std::vector<ScriptValue>&, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                outputs.resize(4);
                outputs[0] = false;
                outputs[1] = false;
                outputs[2] = 0;
                outputs[3] = XMFLOAT2(0, 0);
                return true;
            }
        };

        struct EventCollisionNode : ScriptNode
        {
            EventCollisionNode()
            {
                type = ScriptNodeType::EVENT_COLLISION;
                category = ScriptNodeCategory::EVENT;
                name = "Event Collision";
                description = "Fired when a collision occurs";
                outputSockets.push_back(
                    {"On Enter", ScriptVariableType::EXECUTION, false, true, false, "Collision started"});
                outputSockets.push_back(
                    {"On Stay", ScriptVariableType::EXECUTION, false, true, false, "Still colliding"});
                outputSockets.push_back(
                    {"On Exit", ScriptVariableType::EXECUTION, false, true, false, "Collision ended"});
                outputSockets.push_back({"Other Object", ScriptVariableType::OBJECT_REFERENCE, false, false,
                                         ObjectID{0}, "Other collider"});
                outputSockets.push_back(
                    {"Contact Point", ScriptVariableType::VECTOR3, false, false, XMFLOAT3(0, 0, 0), "Contact point"});
                outputSockets.push_back(
                    {"Normal", ScriptVariableType::VECTOR3, false, false, XMFLOAT3(0, 1, 0), "Contact normal"});
            }
            bool Execute(const std::vector<ScriptValue>&, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                outputs.resize(6);
                outputs[0] = false;
                outputs[1] = false;
                outputs[2] = false;
                outputs[3] = ObjectID{0};
                outputs[4] = XMFLOAT3(0, 0, 0);
                outputs[5] = XMFLOAT3(0, 1, 0);
                return true;
            }
        };

        struct EventTriggerNode : ScriptNode
        {
            EventTriggerNode()
            {
                type = ScriptNodeType::EVENT_TRIGGER;
                category = ScriptNodeCategory::EVENT;
                name = "Event Trigger";
                description = "Fired when entering/exiting a trigger volume";
                outputSockets.push_back(
                    {"On Enter", ScriptVariableType::EXECUTION, false, true, false, "Entered trigger"});
                outputSockets.push_back(
                    {"On Exit", ScriptVariableType::EXECUTION, false, true, false, "Exited trigger"});
                outputSockets.push_back(
                    {"Other Object", ScriptVariableType::OBJECT_REFERENCE, false, false, ObjectID{0}, "Other object"});
            }
            bool Execute(const std::vector<ScriptValue>&, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                outputs.resize(3);
                outputs[0] = false;
                outputs[1] = false;
                outputs[2] = ObjectID{0};
                return true;
            }
        };

        struct EventTimerNode : ScriptNode
        {
            float m_elapsed = 0.0f;
            EventTimerNode()
            {
                type = ScriptNodeType::EVENT_TIMER;
                category = ScriptNodeCategory::EVENT;
                name = "Event Timer";
                description = "Fires periodically at a set interval";
                inputSockets.push_back(
                    {"Interval", ScriptVariableType::FLOAT, true, false, 1.0f, "Timer interval in seconds"});
                inputSockets.push_back(
                    {"Looping", ScriptVariableType::BOOLEAN, true, false, true, "Whether timer repeats"});
                outputSockets.push_back({"Exec", ScriptVariableType::EXECUTION, false, true, false, "Timer fired"});
                outputSockets.push_back(
                    {"Elapsed", ScriptVariableType::FLOAT, false, false, 0.0f, "Total elapsed time"});
            }
            bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext* ctx) override
            {
                float interval = inputs.size() > 0 ? ToFloat(inputs[0]) : 1.0f;
                float dt = ctx ? ctx->GetDeltaTime() : 0.016f;
                m_elapsed += dt;
                bool fired = (m_elapsed >= interval);
                if (fired)
                {
                    bool looping = inputs.size() > 1 ? ToBool(inputs[1]) : true;
                    if (looping)
                    {
                        m_elapsed -= interval;
                    }
                }
                outputs.resize(2);
                outputs[0] = fired;
                outputs[1] = m_elapsed;
                return true;
            }
        };

        struct EventCustomNode : ScriptNode
        {
            EventCustomNode()
            {
                type = ScriptNodeType::EVENT_CUSTOM;
                category = ScriptNodeCategory::EVENT;
                name = "Event Custom";
                description = "Listen for a named custom event";
                inputSockets.push_back(
                    {"Event Name", ScriptVariableType::STRING, true, false, std::string(""), "Custom event name"});
                outputSockets.push_back({"Exec", ScriptVariableType::EXECUTION, false, true, false, "Event fired"});
                outputSockets.push_back({"Event Data", ScriptVariableType::FLOAT, false, false, 0.0f, "Event payload"});
            }
            bool Execute(const std::vector<ScriptValue>&, std::vector<ScriptValue>& outputs,
                         ScriptExecutionContext*) override
            {
                outputs.resize(2);
                outputs[0] = false;
                outputs[1] = 0.0f;
                return true;
            }
        };

    } // anonymous namespace

    // ============================================================================
    // ScriptGraph
    // ============================================================================

    ScriptNode* ScriptGraph::FindNode(uint32_t nodeID)
    {
        for (auto& node : nodes)
        {
            if (node && node->id == nodeID)
                return node.get();
        }
        return nullptr;
    }

    uint32_t ScriptGraph::AddNode(std::unique_ptr<ScriptNode> node)
    {
        if (!node)
            return 0;
        node->id = nextNodeID++;
        uint32_t id = node->id;
        nodes.push_back(std::move(node));
        isCompiled = false;
        return id;
    }

    bool ScriptGraph::RemoveNode(uint32_t nodeID)
    {
        // Remove all connections involving this node
        connections.erase(std::remove_if(connections.begin(), connections.end(), [nodeID](const ScriptConnection& c)
                                         { return c.fromNodeID == nodeID || c.toNodeID == nodeID; }),
                          connections.end());

        // Remove the node
        auto it = std::find_if(nodes.begin(), nodes.end(),
                               [nodeID](const std::unique_ptr<ScriptNode>& n) { return n && n->id == nodeID; });
        if (it == nodes.end())
            return false;
        nodes.erase(it);
        isCompiled = false;
        return true;
    }

    bool ScriptGraph::CreateConnection(const ScriptConnection& connection)
    {
        // Validate nodes exist
        ScriptNode* fromNode = FindNode(connection.fromNodeID);
        ScriptNode* toNode = FindNode(connection.toNodeID);
        if (!fromNode || !toNode)
            return false;

        // Validate socket indices
        if (connection.fromSocketIndex >= fromNode->outputSockets.size())
            return false;
        if (connection.toSocketIndex >= toNode->inputSockets.size())
            return false;

        // Check type compatibility (execution sockets must match, data sockets are more flexible)
        auto& fromSocket = fromNode->outputSockets[connection.fromSocketIndex];
        auto& toSocket = toNode->inputSockets[connection.toSocketIndex];
        if (fromSocket.isExecution != toSocket.isExecution)
            return false;

        // Remove existing connection to the same input socket (inputs can only have one connection)
        connections.erase(std::remove_if(connections.begin(), connections.end(),
                                         [&](const ScriptConnection& c) {
                                             return c.toNodeID == connection.toNodeID &&
                                                    c.toSocketIndex == connection.toSocketIndex;
                                         }),
                          connections.end());

        // Add connection
        connections.push_back(connection);
        fromSocket.isConnected = true;
        toSocket.isConnected = true;
        isCompiled = false;
        return true;
    }

    bool ScriptGraph::RemoveConnection(uint32_t fromNodeID, uint32_t fromSocket)
    {
        auto it =
            std::find_if(connections.begin(), connections.end(), [fromNodeID, fromSocket](const ScriptConnection& c)
                         { return c.fromNodeID == fromNodeID && c.fromSocketIndex == fromSocket; });
        if (it == connections.end())
            return false;

        // Update connected flags
        ScriptNode* toNode = FindNode(it->toNodeID);
        if (toNode && it->toSocketIndex < toNode->inputSockets.size())
        {
            toNode->inputSockets[it->toSocketIndex].isConnected = false;
        }

        connections.erase(it);
        isCompiled = false;
        return true;
    }

    bool ScriptGraph::Validate(std::vector<std::string>& errors) const
    {
        errors.clear();

        if (nodes.empty())
        {
            errors.push_back("Graph has no nodes");
            return false;
        }

        // Check for event nodes (entry points)
        bool hasEventNode = false;
        for (const auto& node : nodes)
        {
            if (!node)
                continue;
            if (node->category == ScriptNodeCategory::EVENT)
            {
                hasEventNode = true;
                break;
            }
        }
        if (!hasEventNode)
        {
            errors.push_back("Graph has no event nodes (entry points)");
        }

        // Check for required unconnected inputs
        for (const auto& node : nodes)
        {
            if (!node)
                continue;
            for (size_t i = 0; i < node->inputSockets.size(); ++i)
            {
                if (node->inputSockets[i].isRequired && !node->inputSockets[i].isConnected)
                {
                    errors.push_back("Node '" + node->name + "' has unconnected required input '" +
                                     node->inputSockets[i].name + "'");
                }
            }
        }

        // Check for cycles in execution flow (simple DFS)
        // Build adjacency list for execution connections
        std::unordered_map<uint32_t, std::vector<uint32_t>> execAdj;
        for (const auto& conn : connections)
        {
            const ScriptNode* fromNode = nullptr;
            for (const auto& n : nodes)
            {
                if (n && n->id == conn.fromNodeID)
                {
                    fromNode = n.get();
                    break;
                }
            }
            if (fromNode && conn.fromSocketIndex < fromNode->outputSockets.size() &&
                fromNode->outputSockets[conn.fromSocketIndex].isExecution)
            {
                execAdj[conn.fromNodeID].push_back(conn.toNodeID);
            }
        }

        // DFS cycle detection
        std::unordered_set<uint32_t> visited, inStack;
        std::function<bool(uint32_t)> hasCycle = [&](uint32_t nodeID) -> bool
        {
            visited.insert(nodeID);
            inStack.insert(nodeID);
            if (execAdj.count(nodeID))
            {
                for (uint32_t next : execAdj[nodeID])
                {
                    if (inStack.count(next))
                        return true;
                    if (!visited.count(next) && hasCycle(next))
                        return true;
                }
            }
            inStack.erase(nodeID);
            return false;
        };

        for (const auto& node : nodes)
        {
            if (node && !visited.count(node->id))
            {
                if (hasCycle(node->id))
                {
                    errors.push_back("Graph contains a cycle in execution flow");
                    break;
                }
            }
        }

        return errors.empty();
    }

    bool ScriptGraph::Compile()
    {
        compilationErrors.clear();
        bool valid = Validate(compilationErrors);
        isCompiled = valid;
        return valid;
    }

    // ============================================================================
    // ScriptExecutionContext
    // ============================================================================

    ScriptExecutionContext::ScriptExecutionContext(ObjectID targetObject) : m_targetObject(targetObject) {}

    ScriptValue ScriptExecutionContext::GetVariable(const std::string& name) const
    {
        auto it = m_variables.find(name);
        if (it != m_variables.end())
            return it->second;
        return 0.0f; // default to float 0
    }

    void ScriptExecutionContext::SetVariable(const std::string& name, const ScriptValue& value)
    {
        m_variables[name] = value;
    }

    void ScriptExecutionContext::Log(const std::string& message, const std::string& level)
    {
        // Store in execution log - actual output depends on integration
        (void)level;
        (void)message;
    }

    float ScriptExecutionContext::GetDeltaTime() const
    {
        // In a real integration this would come from the engine timer
        return 0.016f; // ~60fps default
    }

    // ============================================================================
    // ScriptExecutor
    // ============================================================================

    ScriptExecutor::ScriptExecutor() {}

    ScriptExecutor::~ScriptExecutor() {}

    bool ScriptExecutor::ExecuteGraph(const ScriptGraph& graph, ScriptExecutionContext& context, uint32_t startNodeID)
    {
        // Find start node
        ScriptNode* startNode = nullptr;
        if (startNodeID != 0)
        {
            for (const auto& n : graph.nodes)
            {
                if (n && n->id == startNodeID)
                {
                    startNode = n.get();
                    break;
                }
            }
        }
        else
        {
            // Find first EVENT_START node
            for (const auto& n : graph.nodes)
            {
                if (n && n->type == ScriptNodeType::EVENT_START)
                {
                    startNode = n.get();
                    break;
                }
            }
        }

        if (!startNode)
            return false;

        // Execute starting from the start node, following execution flow
        return ExecuteNode(startNode, context);
    }

    bool ScriptExecutor::ExecuteNode(ScriptNode* node, ScriptExecutionContext& context)
    {
        if (!node || context.ShouldStop())
            return false;

        m_currentNode = node->id;
        node->isExecuting = true;

        // Check breakpoint
        if (m_isDebugging && m_breakpoints.count(node->id) && m_breakpoints[node->id])
        {
            m_stepMode = true;
            node->isExecuting = false;
            return true; // paused at breakpoint
        }

        // Gather input values - for connected inputs, evaluate the source
        std::vector<ScriptValue> inputs(node->inputSockets.size());
        for (size_t i = 0; i < node->inputSockets.size(); ++i)
        {
            if (node->inputSockets[i].isExecution)
            {
                inputs[i] = true; // execution flow is active
                continue;
            }
            inputs[i] = node->inputSockets[i].defaultValue;
        }

        // Execute the node
        std::vector<ScriptValue> outputs;
        bool success = node->Execute(inputs, outputs, &context);

        node->isExecuting = false;
        if (!success)
        {
            node->hasError = true;
            node->errorMessage = "Execution failed";
            return false;
        }
        node->hasError = false;

        return success;
    }

    void ScriptExecutor::StartDebugging(const ScriptGraph& graph, ScriptExecutionContext& context)
    {
        m_isDebugging = true;
        m_stepMode = false;
        m_currentNode = 0;

        // Find start node and begin
        for (const auto& n : graph.nodes)
        {
            if (n && n->type == ScriptNodeType::EVENT_START)
            {
                m_executionQueue.push(n->id);
                break;
            }
        }
        (void)context;
    }

    void ScriptExecutor::StopDebugging()
    {
        m_isDebugging = false;
        m_stepMode = false;
        m_currentNode = 0;
        while (!m_executionQueue.empty())
            m_executionQueue.pop();
    }

    void ScriptExecutor::StepNext()
    {
        if (!m_isDebugging)
            return;
        m_stepMode = true;
    }

    void ScriptExecutor::Continue()
    {
        if (!m_isDebugging)
            return;
        m_stepMode = false;
    }

    void ScriptExecutor::SetBreakpoint(uint32_t nodeID, bool enabled)
    {
        m_breakpoints[nodeID] = enabled;
    }

    bool ScriptExecutor::ExecuteTopological(const ScriptGraph& graph, ScriptExecutionContext& context,
                                            const std::vector<uint32_t>& startNodes)
    {
        auto order = GetExecutionOrder(graph, startNodes);
        for (uint32_t nodeID : order)
        {
            if (context.ShouldStop())
                return false;
            ScriptNode* node = nullptr;
            for (const auto& n : graph.nodes)
            {
                if (n && n->id == nodeID)
                {
                    node = n.get();
                    break;
                }
            }
            if (node && !ExecuteNode(node, context))
                return false;
        }
        return true;
    }

    std::vector<uint32_t> ScriptExecutor::GetExecutionOrder(const ScriptGraph& graph,
                                                            const std::vector<uint32_t>& startNodes)
    {
        // Build execution adjacency list
        std::unordered_map<uint32_t, std::vector<uint32_t>> adj;
        std::unordered_map<uint32_t, int> inDegree;

        for (const auto& n : graph.nodes)
        {
            if (n)
                inDegree[n->id] = 0;
        }

        for (const auto& conn : graph.connections)
        {
            // Only follow execution connections
            for (const auto& n : graph.nodes)
            {
                if (n && n->id == conn.fromNodeID && conn.fromSocketIndex < n->outputSockets.size() &&
                    n->outputSockets[conn.fromSocketIndex].isExecution)
                {
                    adj[conn.fromNodeID].push_back(conn.toNodeID);
                    inDegree[conn.toNodeID]++;
                    break;
                }
            }
        }

        // BFS topological sort starting from start nodes
        std::queue<uint32_t> q;
        for (uint32_t id : startNodes)
        {
            q.push(id);
        }
        // Also add nodes with no incoming execution edges
        if (startNodes.empty())
        {
            for (const auto& [id, deg] : inDegree)
            {
                if (deg == 0)
                    q.push(id);
            }
        }

        std::vector<uint32_t> order;
        std::unordered_set<uint32_t> visited;
        while (!q.empty())
        {
            uint32_t curr = q.front();
            q.pop();
            if (visited.count(curr))
                continue;
            visited.insert(curr);
            order.push_back(curr);
            if (adj.count(curr))
            {
                for (uint32_t next : adj[curr])
                {
                    if (!visited.count(next))
                        q.push(next);
                }
            }
        }

        return order;
    }

    ScriptValue ScriptExecutor::EvaluateSocket(const ScriptGraph& graph, uint32_t nodeID, uint32_t socketIndex,
                                               ScriptExecutionContext& context)
    {
        // Find what's connected to this input socket
        for (const auto& conn : graph.connections)
        {
            if (conn.toNodeID == nodeID && conn.toSocketIndex == socketIndex)
            {
                // Found the source - execute the source node and get its output
                ScriptNode* sourceNode = nullptr;
                for (const auto& n : graph.nodes)
                {
                    if (n && n->id == conn.fromNodeID)
                    {
                        sourceNode = n.get();
                        break;
                    }
                }
                if (sourceNode)
                {
                    std::vector<ScriptValue> inputs(sourceNode->inputSockets.size());
                    for (size_t i = 0; i < inputs.size(); ++i)
                    {
                        inputs[i] = sourceNode->inputSockets[i].defaultValue;
                    }
                    std::vector<ScriptValue> outputs;
                    sourceNode->Execute(inputs, outputs, &context);
                    if (conn.fromSocketIndex < outputs.size())
                    {
                        return outputs[conn.fromSocketIndex];
                    }
                }
            }
        }

        // No connection found, return default
        ScriptNode* node = nullptr;
        for (const auto& n : graph.nodes)
        {
            if (n && n->id == nodeID)
            {
                node = n.get();
                break;
            }
        }
        if (node && socketIndex < node->inputSockets.size())
        {
            return node->inputSockets[socketIndex].defaultValue;
        }
        return false;
    }

    // ============================================================================
    // VisualScriptingSystem
    // ============================================================================

    VisualScriptingSystem::VisualScriptingSystem() : EditorPanel("Visual Scripting", "visual_scripting") {}

    VisualScriptingSystem::~VisualScriptingSystem()
    {
        Shutdown();
    }

    bool VisualScriptingSystem::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Scripting);
        m_executor = std::make_unique<ScriptExecutor>();
        InitializeBuiltInNodes();
        return true;
    }

    void VisualScriptingSystem::Update(float deltaTime)
    {
        if (m_isExecuting)
        {
            UpdateExecution();
        }
        (void)deltaTime;
    }

    void VisualScriptingSystem::Render()
    {
        // Main layout: palette | editor | properties
        // Not rendering ImGui here as this depends on the editor's ImGui context
        // In a full integration, this would be called from the editor's render loop
        RenderNodePalette();
        RenderScriptEditor();
        RenderScriptProperties();
        if (m_isDebugging)
        {
            RenderDebugInterface();
            RenderExecutionLog();
        }
    }

    void VisualScriptingSystem::Shutdown()
    {
        m_executor.reset();
        m_executionContext.reset();
        m_currentScript = ScriptGraph();
        m_nodeFactories.clear();
        m_nodeCategories.clear();
        m_customNodes.clear();
    }

    bool VisualScriptingSystem::HandleEvent(const std::string& eventType, void* eventData)
    {
        if (eventType == "delete_selected")
        {
            for (uint32_t id : m_selectedNodes)
            {
                m_currentScript.RemoveNode(id);
            }
            m_selectedNodes.clear();
            return true;
        }
        return false;
        (void)eventData;
    }

    void VisualScriptingSystem::CreateNewScript(const std::string& name, ObjectID targetObject)
    {
        m_currentScript = ScriptGraph();
        m_currentScript.name = name;
        m_currentScript.targetObjectID = targetObject;
        m_selectedNodes.clear();
        m_isExecuting = false;
        m_isDebugging = false;

        // Add default Event Start node
        auto startNode = CreateNode(ScriptNodeType::EVENT_START);
        if (startNode)
        {
            startNode->position = {100.0f, 200.0f};
            m_currentScript.AddNode(std::move(startNode));
        }
    }

    bool VisualScriptingSystem::LoadScript(const std::string& filePath)
    {
        std::ifstream file(filePath);
        if (!file.is_open())
            return false;

        // Simple text-based format for script loading
        std::string line;
        ScriptGraph newGraph;

        while (std::getline(file, line))
        {
            if (line.empty() || line[0] == '#')
                continue;
            std::istringstream iss(line);
            std::string token;
            iss >> token;

            if (token == "name")
            {
                std::getline(iss >> std::ws, newGraph.name);
            }
            else if (token == "node")
            {
                int typeInt;
                float x, y;
                if (iss >> typeInt >> x >> y)
                {
                    auto node = CreateNode(static_cast<ScriptNodeType>(typeInt));
                    if (node)
                    {
                        node->position = {x, y};
                        newGraph.AddNode(std::move(node));
                    }
                }
            }
            else if (token == "connection")
            {
                ScriptConnection conn;
                if (iss >> conn.fromNodeID >> conn.fromSocketIndex >> conn.toNodeID >> conn.toSocketIndex)
                {
                    newGraph.CreateConnection(conn);
                }
            }
            else if (token == "variable")
            {
                ScriptVariable var;
                int typeInt;
                if (iss >> var.name >> typeInt)
                {
                    var.type = static_cast<ScriptVariableType>(typeInt);
                    newGraph.variables.push_back(var);
                }
            }
        }

        m_currentScript = std::move(newGraph);
        m_selectedNodes.clear();
        return true;
    }

    bool VisualScriptingSystem::SaveScript(const std::string& filePath)
    {
        std::ofstream file(filePath);
        if (!file.is_open())
            return false;

        file << "# Spark Engine Visual Script\n";
        file << "name " << m_currentScript.name << "\n";

        // Save nodes
        for (const auto& node : m_currentScript.nodes)
        {
            if (!node)
                continue;
            file << "node " << static_cast<int>(node->type) << " " << node->position.x << " " << node->position.y
                 << "\n";
        }

        // Save connections
        for (const auto& conn : m_currentScript.connections)
        {
            file << "connection " << conn.fromNodeID << " " << conn.fromSocketIndex << " " << conn.toNodeID << " "
                 << conn.toSocketIndex << "\n";
        }

        // Save variables
        for (const auto& var : m_currentScript.variables)
        {
            file << "variable " << var.name << " " << static_cast<int>(var.type) << "\n";
        }

        return true;
    }

    bool VisualScriptingSystem::CompileScript()
    {
        return m_currentScript.Compile();
    }

    bool VisualScriptingSystem::ExecuteScript(ObjectID targetObject)
    {
        if (!m_executor)
            return false;
        if (!m_currentScript.isCompiled)
        {
            if (!CompileScript())
                return false;
        }

        m_executionContext = std::make_unique<ScriptExecutionContext>(targetObject);

        // Initialize variables with defaults
        for (const auto& var : m_currentScript.variables)
        {
            m_executionContext->SetVariable(var.name, var.defaultValue);
        }

        auto start = std::chrono::high_resolution_clock::now();
        bool result = m_executor->ExecuteGraph(m_currentScript, *m_executionContext);
        auto end = std::chrono::high_resolution_clock::now();
        m_lastExecutionTime = std::chrono::duration<float, std::milli>(end - start).count();

        m_isExecuting = result;
        return result;
    }

    uint32_t VisualScriptingSystem::AddNode(ScriptNodeType nodeType, const XMFLOAT2& position)
    {
        auto node = CreateNode(nodeType);
        if (!node)
            return 0;
        node->position = position;
        return m_currentScript.AddNode(std::move(node));
    }

    bool VisualScriptingSystem::RemoveNode(uint32_t nodeID)
    {
        // Remove from selection
        m_selectedNodes.erase(std::remove(m_selectedNodes.begin(), m_selectedNodes.end(), nodeID),
                              m_selectedNodes.end());
        return m_currentScript.RemoveNode(nodeID);
    }

    bool VisualScriptingSystem::ConnectSockets(uint32_t fromNodeID, uint32_t fromSocketIndex, uint32_t toNodeID,
                                               uint32_t toSocketIndex)
    {
        ScriptConnection conn;
        conn.fromNodeID = fromNodeID;
        conn.fromSocketIndex = fromSocketIndex;
        conn.toNodeID = toNodeID;
        conn.toSocketIndex = toSocketIndex;
        return m_currentScript.CreateConnection(conn);
    }

    void VisualScriptingSystem::StartDebugging(ObjectID targetObject)
    {
        if (!m_executor)
            return;
        if (!m_currentScript.isCompiled)
        {
            if (!CompileScript())
                return;
        }
        m_executionContext = std::make_unique<ScriptExecutionContext>(targetObject);
        m_executor->StartDebugging(m_currentScript, *m_executionContext);
        m_isDebugging = true;
        m_executionLog.clear();
        m_executionLog.push_back("Debugging started");
    }

    void VisualScriptingSystem::StopDebugging()
    {
        if (m_executor)
            m_executor->StopDebugging();
        m_isDebugging = false;
        m_isExecuting = false;
        m_executionLog.push_back("Debugging stopped");
    }

    void VisualScriptingSystem::RegisterCustomNode(const std::string& typeName,
                                                   std::function<std::unique_ptr<ScriptNode>()> factory)
    {
        m_customNodes[typeName] = std::move(factory);
    }

    // ============================================================================
    // Private methods
    // ============================================================================

    void VisualScriptingSystem::RenderScriptEditor()
    {
        ImGui::BeginChild("ScriptEditor", ImVec2(0, -m_debugHeight), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        ImVec2 canvasSize = ImGui::GetContentRegionAvail();
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Clip rect for canvas area
        drawList->PushClipRect(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y), true);

        // Draw grid background
        constexpr float GRID_STEP = 32.0f;
        ImU32 gridColor = IM_COL32(50, 50, 50, 200);
        ImU32 gridColorMajor = IM_COL32(70, 70, 70, 200);
        float scaledStep = GRID_STEP * m_graphViewScale;

        float gridOffsetX = std::fmod(m_graphViewOffset.x * m_graphViewScale, scaledStep);
        float gridOffsetY = std::fmod(m_graphViewOffset.y * m_graphViewScale, scaledStep);

        for (float x = gridOffsetX; x < canvasSize.x; x += scaledStep)
        {
            int lineIdx = static_cast<int>((x - gridOffsetX) / scaledStep);
            ImU32 col = (lineIdx % 4 == 0) ? gridColorMajor : gridColor;
            drawList->AddLine(ImVec2(canvasPos.x + x, canvasPos.y), ImVec2(canvasPos.x + x, canvasPos.y + canvasSize.y),
                              col);
        }
        for (float y = gridOffsetY; y < canvasSize.y; y += scaledStep)
        {
            int lineIdx = static_cast<int>((y - gridOffsetY) / scaledStep);
            ImU32 col = (lineIdx % 4 == 0) ? gridColorMajor : gridColor;
            drawList->AddLine(ImVec2(canvasPos.x, canvasPos.y + y), ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + y),
                              col);
        }

        // Draw connections
        RenderConnections();

        // Draw nodes
        for (auto& node : m_currentScript.nodes)
        {
            if (node)
            {
                RenderScriptNode(node.get());
            }
        }

        drawList->PopClipRect();

        // Handle interactions on canvas
        ImGui::InvisibleButton("canvas", canvasSize,
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);

        // Canvas navigation (pan/zoom)
        HandleCanvasNavigation();

        // Right-click context menu for node creation
        HandleNodeCreation();

        // Handle node dragging and selection
        HandleNodeDragging();
        HandleNodeSelection();
        HandleConnectionCreation();

        // Graph overview minimap
        RenderGraphOverview();

        ImGui::EndChild();
    }

    void VisualScriptingSystem::RenderNodePalette()
    {
        ImGui::BeginChild("NodePalette", ImVec2(m_nodeListWidth, 0), true);
        ImGui::Text("Node Palette");
        ImGui::Separator();

        // Search filter
        static char searchBuffer[256] = "";
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##NodeSearch", "Search nodes...", searchBuffer, sizeof(searchBuffer));
        ImGui::Separator();

        std::string searchStr(searchBuffer);
        // Convert search to lowercase for case-insensitive matching
        std::string searchLower = searchStr;
        std::transform(searchLower.begin(), searchLower.end(), searchLower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        for (const auto& [categoryName, nodeTypes] : m_nodeCategories)
        {
            // If searching, skip categories with no matches
            bool hasMatch = searchLower.empty();
            if (!hasMatch)
            {
                for (const auto& nodeType : nodeTypes)
                {
                    auto tempNode = CreateNode(nodeType);
                    if (tempNode)
                    {
                        std::string nameLower = tempNode->name;
                        std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(),
                                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        if (nameLower.find(searchLower) != std::string::npos)
                        {
                            hasMatch = true;
                            break;
                        }
                    }
                }
            }
            if (!hasMatch)
                continue;

            if (ImGui::TreeNodeEx(categoryName.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            {
                for (const auto& nodeType : nodeTypes)
                {
                    auto tempNode = CreateNode(nodeType);
                    if (!tempNode)
                        continue;

                    // Apply search filter
                    if (!searchLower.empty())
                    {
                        std::string nameLower = tempNode->name;
                        std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(),
                                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        if (nameLower.find(searchLower) == std::string::npos)
                            continue;
                    }

                    // Color indicator for category
                    XMFLOAT4 catColor = GetNodeCategoryColor(tempNode->category);
                    ImVec4 color(catColor.x, catColor.y, catColor.z, catColor.w);
                    ImGui::PushStyleColor(ImGuiCol_Button, color);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                          ImVec4(color.x * 1.2f, color.y * 1.2f, color.z * 1.2f, 1.0f));

                    if (ImGui::Button(tempNode->name.c_str(), ImVec2(-1, 0)))
                    {
                        // Add node at center of current view
                        XMFLOAT2 spawnPos = {-m_graphViewOffset.x + 300.0f, -m_graphViewOffset.y + 200.0f};
                        AddNode(nodeType, spawnPos);
                    }

                    ImGui::PopStyleColor(2);

                    // Tooltip with description
                    if (ImGui::IsItemHovered() && !tempNode->description.empty())
                    {
                        ImGui::SetTooltip("%s", tempNode->description.c_str());
                    }
                }
                ImGui::TreePop();
            }
        }

        ImGui::EndChild();
        ImGui::SameLine();
    }

    void VisualScriptingSystem::RenderScriptProperties()
    {
        ImGui::SameLine();
        ImGui::BeginChild("Properties", ImVec2(m_propertiesWidth, 0), true);
        ImGui::Text("Properties");
        ImGui::Separator();

        if (m_selectedNodes.empty())
        {
            // Show script-level properties
            ImGui::Text("Script: %s", m_currentScript.name.c_str());

            static char nameBuffer[256];
            std::strncpy(nameBuffer, m_currentScript.name.c_str(), sizeof(nameBuffer) - 1);
            nameBuffer[sizeof(nameBuffer) - 1] = '\0';
            if (ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer)))
            {
                m_currentScript.name = nameBuffer;
            }

            ImGui::Separator();
            ImGui::Text("Nodes: %d", static_cast<int>(m_currentScript.nodes.size()));
            ImGui::Text("Connections: %d", static_cast<int>(m_currentScript.connections.size()));
            ImGui::Text("Variables: %d", static_cast<int>(m_currentScript.variables.size()));

            ImGui::Separator();
            if (ImGui::Button("Compile", ImVec2(-1, 0)))
            {
                CompileScript();
            }
            if (m_currentScript.isCompiled)
            {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Compiled successfully");
            }
            else if (!m_currentScript.compilationErrors.empty())
            {
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Compilation errors:");
                for (const auto& err : m_currentScript.compilationErrors)
                {
                    ImGui::TextWrapped("  - %s", err.c_str());
                }
            }

            // Variables section
            ImGui::Separator();
            ImGui::Text("Variables");
            for (size_t i = 0; i < m_currentScript.variables.size(); ++i)
            {
                auto& var = m_currentScript.variables[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::Text("%s", var.name.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("X"))
                {
                    m_currentScript.variables.erase(m_currentScript.variables.begin() + static_cast<ptrdiff_t>(i));
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
        }
        else
        {
            // Show properties for selected node
            ScriptNode* selected = m_currentScript.FindNode(m_selectedNodes[0]);
            if (selected)
            {
                ImGui::Text("Node: %s", selected->name.c_str());
                ImGui::Text("Type: %d", static_cast<int>(selected->type));
                ImGui::Text("ID: %u", selected->id);
                ImGui::Separator();

                // Position
                ImGui::DragFloat2("Position", &selected->position.x, 1.0f);

                // Enabled toggle
                ImGui::Checkbox("Enabled", &selected->isEnabled);

                // Breakpoint toggle
                ImGui::Checkbox("Breakpoint", &selected->isBreakpoint);
                if (selected->isBreakpoint && m_executor)
                {
                    m_executor->SetBreakpoint(selected->id, true);
                }

                // Show input socket default values
                ImGui::Separator();
                ImGui::Text("Input Sockets:");
                for (size_t i = 0; i < selected->inputSockets.size(); ++i)
                {
                    auto& socket = selected->inputSockets[i];
                    if (socket.isExecution)
                        continue;

                    ImGui::PushID(static_cast<int>(i));
                    ImGui::Text("%s:", socket.name.c_str());
                    ImGui::SameLine();

                    switch (socket.type)
                    {
                    case ScriptVariableType::FLOAT:
                    {
                        float val = ToFloat(socket.defaultValue);
                        if (ImGui::DragFloat("##val", &val, 0.1f))
                        {
                            socket.defaultValue = val;
                        }
                        break;
                    }
                    case ScriptVariableType::INTEGER:
                    {
                        int val = ToInt(socket.defaultValue);
                        if (ImGui::DragInt("##val", &val))
                        {
                            socket.defaultValue = val;
                        }
                        break;
                    }
                    case ScriptVariableType::BOOLEAN:
                    {
                        bool val = ToBool(socket.defaultValue);
                        if (ImGui::Checkbox("##val", &val))
                        {
                            socket.defaultValue = val;
                        }
                        break;
                    }
                    case ScriptVariableType::STRING:
                    {
                        std::string val = ToString(socket.defaultValue);
                        char buf[256];
                        std::strncpy(buf, val.c_str(), sizeof(buf) - 1);
                        buf[sizeof(buf) - 1] = '\0';
                        if (ImGui::InputText("##val", buf, sizeof(buf)))
                        {
                            socket.defaultValue = std::string(buf);
                        }
                        break;
                    }
                    default:
                        ImGui::Text("(complex type)");
                        break;
                    }
                    ImGui::PopID();
                }

                // Output sockets info
                ImGui::Separator();
                ImGui::Text("Output Sockets:");
                for (const auto& socket : selected->outputSockets)
                {
                    ImGui::BulletText("%s (%s)", socket.name.c_str(), socket.isExecution ? "exec" : "data");
                }

                // Error display
                if (selected->hasError)
                {
                    ImGui::Separator();
                    ImGui::TextColored(ImVec4(1, 0, 0, 1), "Error: %s", selected->errorMessage.c_str());
                }

                ImGui::Separator();
                if (ImGui::Button("Delete Node", ImVec2(-1, 0)))
                {
                    RemoveNode(selected->id);
                }
            }
        }

        ImGui::EndChild();
    }

    void VisualScriptingSystem::RenderDebugInterface()
    {
        ImGui::BeginChild("DebugInterface", ImVec2(0, m_debugHeight * 0.5f), true);
        ImGui::Text("Debug Controls");
        ImGui::Separator();

        // Control buttons
        if (m_isDebugging)
        {
            if (ImGui::Button("Continue"))
            {
                if (m_executor)
                    m_executor->Continue();
            }
            ImGui::SameLine();
            if (ImGui::Button("Step"))
            {
                if (m_executor)
                    m_executor->StepNext();
            }
            ImGui::SameLine();
            if (ImGui::Button("Stop"))
            {
                StopDebugging();
            }

            ImGui::Separator();

            // Current execution state
            uint32_t currentNode = m_executor ? m_executor->GetCurrentNode() : 0;
            if (currentNode != 0)
            {
                ScriptNode* node = m_currentScript.FindNode(currentNode);
                if (node)
                {
                    ImGui::Text("Current Node: %s (ID: %u)", node->name.c_str(), node->id);
                }
            }
            else
            {
                ImGui::Text("Execution paused");
            }

            // Breakpoints list
            ImGui::Separator();
            ImGui::Text("Breakpoints:");
            for (const auto& node : m_currentScript.nodes)
            {
                if (node && node->isBreakpoint)
                {
                    ImGui::BulletText("%s (ID: %u)", node->name.c_str(), node->id);
                    ImGui::SameLine();
                    ImGui::PushID(static_cast<int>(node->id));
                    if (ImGui::SmallButton("Remove"))
                    {
                        node->isBreakpoint = false;
                        if (m_executor)
                            m_executor->SetBreakpoint(node->id, false);
                    }
                    ImGui::PopID();
                }
            }
        }
        else
        {
            if (ImGui::Button("Start Debugging"))
            {
                StartDebugging(m_currentScript.targetObjectID);
            }
        }

        // Performance info
        ImGui::Separator();
        ImGui::Text("Last Execution: %.3f ms", m_lastExecutionTime);
        ImGui::Text("Nodes Executed: %d", m_executedNodesCount);

        ImGui::EndChild();
    }

    void VisualScriptingSystem::RenderExecutionLog()
    {
        ImGui::BeginChild("ExecutionLog", ImVec2(0, m_debugHeight * 0.5f), true);
        ImGui::Text("Execution Log");
        ImGui::Separator();

        // Scrolling log
        ImGui::BeginChild("LogScroll", ImVec2(0, 0), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        for (const auto& msg : m_executionLog)
        {
            ImGui::TextWrapped("%s", msg.c_str());
        }

        // Auto-scroll to bottom
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();

        ImGui::EndChild();
    }

    void VisualScriptingSystem::RenderScriptNode(ScriptNode* node)
    {
        if (!node)
            return;

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 canvasPos = ImGui::GetCursorScreenPos();

        // Calculate screen position from graph position
        float screenX = canvasPos.x + (node->position.x + m_graphViewOffset.x) * m_graphViewScale;
        float screenY = canvasPos.y + (node->position.y + m_graphViewOffset.y) * m_graphViewScale;
        float nodeWidth = node->size.x * m_graphViewScale;

        // Calculate node height based on socket count
        constexpr float HEADER_HEIGHT = 24.0f;
        constexpr float SOCKET_HEIGHT = 20.0f;
        constexpr float PADDING = 4.0f;
        int maxSockets = static_cast<int>(std::max(node->inputSockets.size(), node->outputSockets.size()));
        float bodyHeight = (static_cast<float>(maxSockets) * SOCKET_HEIGHT + PADDING * 2.0f) * m_graphViewScale;
        float headerHeight = HEADER_HEIGHT * m_graphViewScale;
        float totalHeight = headerHeight + bodyHeight;

        ImVec2 nodeMin(screenX, screenY);
        ImVec2 nodeMax(screenX + nodeWidth, screenY + totalHeight);

        // Node body background
        ImU32 bodyCol = node->isSelected ? IM_COL32(50, 50, 60, 230) : IM_COL32(38, 38, 38, 230);
        if (node->hasError)
        {
            bodyCol = IM_COL32(80, 30, 30, 230);
        }
        drawList->AddRectFilled(nodeMin, nodeMax, bodyCol, 4.0f * m_graphViewScale);

        // Node header
        XMFLOAT4 hdrColor = GetNodeCategoryColor(node->category);
        ImU32 headerCol = IM_COL32(static_cast<int>(hdrColor.x * 255), static_cast<int>(hdrColor.y * 255),
                                   static_cast<int>(hdrColor.z * 255), 230);
        if (node->isExecuting)
        {
            headerCol = IM_COL32(255, 200, 50, 255); // Highlight executing node
        }
        ImVec2 headerMax(screenX + nodeWidth, screenY + headerHeight);
        drawList->AddRectFilled(nodeMin, headerMax, headerCol, 4.0f * m_graphViewScale, ImDrawFlags_RoundCornersTop);

        // Node title
        float fontSize = 13.0f * m_graphViewScale;
        if (fontSize > 6.0f) // Only draw text if large enough to read
        {
            drawList->AddText(nullptr, fontSize,
                              ImVec2(screenX + 6.0f * m_graphViewScale, screenY + 4.0f * m_graphViewScale),
                              IM_COL32(255, 255, 255, 255), node->name.c_str());
        }

        // Breakpoint indicator
        if (node->isBreakpoint)
        {
            float radius = 5.0f * m_graphViewScale;
            drawList->AddCircleFilled(
                ImVec2(screenX + nodeWidth - radius - 4.0f * m_graphViewScale, screenY + headerHeight * 0.5f), radius,
                IM_COL32(255, 0, 0, 255));
        }

        // Node border
        ImU32 borderCol = node->isSelected ? IM_COL32(255, 200, 50, 255) : IM_COL32(100, 100, 100, 255);
        drawList->AddRect(nodeMin, nodeMax, borderCol, 4.0f * m_graphViewScale, 0, 1.5f);

        // Draw input sockets
        constexpr float SOCKET_RADIUS = 5.0f;
        float socketY = screenY + headerHeight + PADDING * m_graphViewScale;
        for (size_t i = 0; i < node->inputSockets.size(); ++i)
        {
            auto& socket = node->inputSockets[i];
            float sy = socketY + static_cast<float>(i) * SOCKET_HEIGHT * m_graphViewScale;
            float sx = screenX;

            // Socket circle
            XMFLOAT4 sockColor = GetSocketTypeColor(socket.type);
            ImU32 sockCol = IM_COL32(static_cast<int>(sockColor.x * 255), static_cast<int>(sockColor.y * 255),
                                     static_cast<int>(sockColor.z * 255), 255);
            float radius = SOCKET_RADIUS * m_graphViewScale;

            if (socket.isExecution)
            {
                // Draw triangle for execution sockets
                drawList->AddTriangleFilled(ImVec2(sx - radius, sy - radius), ImVec2(sx + radius, sy),
                                            ImVec2(sx - radius, sy + radius), sockCol);
            }
            else
            {
                drawList->AddCircleFilled(ImVec2(sx, sy), radius,
                                          socket.isConnected ? sockCol : IM_COL32(60, 60, 60, 255));
                drawList->AddCircle(ImVec2(sx, sy), radius, sockCol);
            }

            // Store socket position for connection rendering
            socket.position = {sx, sy};

            // Socket label
            if (fontSize > 6.0f)
            {
                drawList->AddText(nullptr, fontSize * 0.85f,
                                  ImVec2(sx + radius + 4.0f * m_graphViewScale, sy - fontSize * 0.4f),
                                  IM_COL32(200, 200, 200, 255), socket.name.c_str());
            }
        }

        // Draw output sockets
        for (size_t i = 0; i < node->outputSockets.size(); ++i)
        {
            auto& socket = node->outputSockets[i];
            float sy = socketY + static_cast<float>(i) * SOCKET_HEIGHT * m_graphViewScale;
            float sx = screenX + nodeWidth;

            XMFLOAT4 sockColor = GetSocketTypeColor(socket.type);
            ImU32 sockCol = IM_COL32(static_cast<int>(sockColor.x * 255), static_cast<int>(sockColor.y * 255),
                                     static_cast<int>(sockColor.z * 255), 255);
            float radius = SOCKET_RADIUS * m_graphViewScale;

            if (socket.isExecution)
            {
                drawList->AddTriangleFilled(ImVec2(sx - radius, sy - radius), ImVec2(sx + radius, sy),
                                            ImVec2(sx - radius, sy + radius), sockCol);
            }
            else
            {
                drawList->AddCircleFilled(ImVec2(sx, sy), radius,
                                          socket.isConnected ? sockCol : IM_COL32(60, 60, 60, 255));
                drawList->AddCircle(ImVec2(sx, sy), radius, sockCol);
            }

            // Store socket position
            socket.position = {sx, sy};

            // Socket label (right-aligned)
            if (fontSize > 6.0f)
            {
                ImVec2 textSize = ImGui::CalcTextSize(socket.name.c_str());
                float scaledTextWidth = textSize.x * (fontSize * 0.85f / ImGui::GetFontSize());
                drawList->AddText(nullptr, fontSize * 0.85f,
                                  ImVec2(sx - radius - 4.0f * m_graphViewScale - scaledTextWidth, sy - fontSize * 0.4f),
                                  IM_COL32(200, 200, 200, 255), socket.name.c_str());
            }
        }
    }

    void VisualScriptingSystem::RenderConnections()
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        for (const auto& conn : m_currentScript.connections)
        {
            // Find source and target nodes
            ScriptNode* fromNode = m_currentScript.FindNode(conn.fromNodeID);
            ScriptNode* toNode = m_currentScript.FindNode(conn.toNodeID);
            if (!fromNode || !toNode)
                continue;
            if (conn.fromSocketIndex >= fromNode->outputSockets.size())
                continue;
            if (conn.toSocketIndex >= toNode->inputSockets.size())
                continue;

            // Get socket screen positions
            auto& fromSocket = fromNode->outputSockets[conn.fromSocketIndex];
            auto& toSocket = toNode->inputSockets[conn.toSocketIndex];

            ImVec2 p1(fromSocket.position.x, fromSocket.position.y);
            ImVec2 p4(toSocket.position.x, toSocket.position.y);

            // Bezier control points
            float dist = std::abs(p4.x - p1.x) * 0.5f;
            dist = std::max(dist, 50.0f * m_graphViewScale);
            ImVec2 p2(p1.x + dist, p1.y);
            ImVec2 p3(p4.x - dist, p4.y);

            // Connection color based on type
            XMFLOAT4 sockColor = GetSocketTypeColor(fromSocket.type);
            ImU32 lineColor = IM_COL32(static_cast<int>(sockColor.x * 200), static_cast<int>(sockColor.y * 200),
                                       static_cast<int>(sockColor.z * 200), 200);

            if (conn.isActive)
            {
                lineColor = IM_COL32(255, 255, 100, 255); // Highlight active connections
            }
            if (conn.isSelected)
            {
                lineColor = IM_COL32(255, 200, 50, 255);
            }

            float thickness = conn.thickness * m_graphViewScale;
            drawList->AddBezierCubic(p1, p2, p3, p4, lineColor, thickness);
        }

        // Draw in-progress connection
        if (m_isCreatingConnection)
        {
            ScriptNode* startNode = m_currentScript.FindNode(m_connectionStartNodeID);
            if (startNode)
            {
                ImVec2 startPos;
                if (m_connectionStartIsInput && m_connectionStartSocket < startNode->inputSockets.size())
                {
                    auto& sock = startNode->inputSockets[m_connectionStartSocket];
                    startPos = ImVec2(sock.position.x, sock.position.y);
                }
                else if (!m_connectionStartIsInput && m_connectionStartSocket < startNode->outputSockets.size())
                {
                    auto& sock = startNode->outputSockets[m_connectionStartSocket];
                    startPos = ImVec2(sock.position.x, sock.position.y);
                }

                ImVec2 mousePos = ImGui::GetIO().MousePos;
                float dist = std::abs(mousePos.x - startPos.x) * 0.5f;
                dist = std::max(dist, 50.0f * m_graphViewScale);

                ImVec2 cp1, cp2;
                if (m_connectionStartIsInput)
                {
                    cp1 = ImVec2(startPos.x - dist, startPos.y);
                    cp2 = ImVec2(mousePos.x + dist, mousePos.y);
                }
                else
                {
                    cp1 = ImVec2(startPos.x + dist, startPos.y);
                    cp2 = ImVec2(mousePos.x - dist, mousePos.y);
                }

                drawList->AddBezierCubic(startPos, cp1, cp2, mousePos, IM_COL32(200, 200, 200, 150),
                                         2.0f * m_graphViewScale);
            }
        }
    }

    void VisualScriptingSystem::HandleNodeCreation()
    {
        // Right-click context menu for creating nodes
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            ImGui::OpenPopup("NodeContextMenu");
        }

        if (ImGui::BeginPopup("NodeContextMenu"))
        {
            ImGui::Text("Add Node");
            ImGui::Separator();

            static char contextSearch[128] = "";
            ImGui::InputTextWithHint("##ContextSearch", "Search...", contextSearch, sizeof(contextSearch));

            std::string searchStr(contextSearch);
            std::string searchLower = searchStr;
            std::transform(searchLower.begin(), searchLower.end(), searchLower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            ImVec2 mousePos = ImGui::GetIO().MousePos;
            ImVec2 canvasPos = ImGui::GetCursorScreenPos();

            for (const auto& [categoryName, nodeTypes] : m_nodeCategories)
            {
                if (ImGui::BeginMenu(categoryName.c_str()))
                {
                    for (const auto& nodeType : nodeTypes)
                    {
                        auto tempNode = CreateNode(nodeType);
                        if (!tempNode)
                            continue;

                        if (!searchLower.empty())
                        {
                            std::string nameLower = tempNode->name;
                            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(),
                                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                            if (nameLower.find(searchLower) == std::string::npos)
                                continue;
                        }

                        if (ImGui::MenuItem(tempNode->name.c_str()))
                        {
                            XMFLOAT2 spawnPos = {(mousePos.x - canvasPos.x) / m_graphViewScale - m_graphViewOffset.x,
                                                 (mousePos.y - canvasPos.y) / m_graphViewScale - m_graphViewOffset.y};
                            AddNode(nodeType, spawnPos);
                            contextSearch[0] = '\0';
                        }
                    }
                    ImGui::EndMenu();
                }
            }
            ImGui::EndPopup();
        }
    }

    void VisualScriptingSystem::HandleNodeDragging()
    {
        if (!m_isDraggingNode)
        {
            // Check if mouse is over a node and start dragging
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                ImVec2 mousePos = ImGui::GetIO().MousePos;
                ImVec2 canvasPos = ImGui::GetCursorScreenPos();

                for (auto& node : m_currentScript.nodes)
                {
                    if (!node)
                        continue;

                    float screenX = canvasPos.x + (node->position.x + m_graphViewOffset.x) * m_graphViewScale;
                    float screenY = canvasPos.y + (node->position.y + m_graphViewOffset.y) * m_graphViewScale;
                    float nodeWidth = node->size.x * m_graphViewScale;
                    float nodeHeight = node->size.y * m_graphViewScale;

                    if (mousePos.x >= screenX && mousePos.x <= screenX + nodeWidth && mousePos.y >= screenY &&
                        mousePos.y <= screenY + nodeHeight)
                    {
                        m_isDraggingNode = true;
                        m_draggedNodeID = node->id;
                        m_dragOffset = {(mousePos.x - screenX) / m_graphViewScale,
                                        (mousePos.y - screenY) / m_graphViewScale};
                        break;
                    }
                }
            }
        }
        else
        {
            // Update dragged node position
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            {
                ScriptNode* node = m_currentScript.FindNode(m_draggedNodeID);
                if (node)
                {
                    ImVec2 mousePos = ImGui::GetIO().MousePos;
                    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
                    node->position.x =
                        (mousePos.x - canvasPos.x) / m_graphViewScale - m_graphViewOffset.x - m_dragOffset.x;
                    node->position.y =
                        (mousePos.y - canvasPos.y) / m_graphViewScale - m_graphViewOffset.y - m_dragOffset.y;
                }
            }

            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            {
                m_isDraggingNode = false;
                m_draggedNodeID = 0;
            }
        }
    }

    void VisualScriptingSystem::HandleConnectionCreation()
    {
        constexpr float SOCKET_HIT_RADIUS = 8.0f;

        if (!m_isCreatingConnection)
        {
            // Check if mouse clicked on a socket
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                ImVec2 mousePos = ImGui::GetIO().MousePos;

                for (auto& node : m_currentScript.nodes)
                {
                    if (!node)
                        continue;

                    // Check output sockets
                    for (size_t i = 0; i < node->outputSockets.size(); ++i)
                    {
                        auto& socket = node->outputSockets[i];
                        float dx = mousePos.x - socket.position.x;
                        float dy = mousePos.y - socket.position.y;
                        if (dx * dx + dy * dy <=
                            SOCKET_HIT_RADIUS * SOCKET_HIT_RADIUS * m_graphViewScale * m_graphViewScale)
                        {
                            m_isCreatingConnection = true;
                            m_connectionStartNodeID = node->id;
                            m_connectionStartSocket = static_cast<uint32_t>(i);
                            m_connectionStartIsInput = false;
                            return;
                        }
                    }

                    // Check input sockets
                    for (size_t i = 0; i < node->inputSockets.size(); ++i)
                    {
                        auto& socket = node->inputSockets[i];
                        float dx = mousePos.x - socket.position.x;
                        float dy = mousePos.y - socket.position.y;
                        if (dx * dx + dy * dy <=
                            SOCKET_HIT_RADIUS * SOCKET_HIT_RADIUS * m_graphViewScale * m_graphViewScale)
                        {
                            m_isCreatingConnection = true;
                            m_connectionStartNodeID = node->id;
                            m_connectionStartSocket = static_cast<uint32_t>(i);
                            m_connectionStartIsInput = true;
                            return;
                        }
                    }
                }
            }
        }
        else
        {
            // Connection in progress - check for release on a socket
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            {
                ImVec2 mousePos = ImGui::GetIO().MousePos;
                bool connected = false;

                for (auto& node : m_currentScript.nodes)
                {
                    if (!node || node->id == m_connectionStartNodeID)
                        continue;

                    if (m_connectionStartIsInput)
                    {
                        // Started from input, look for output socket
                        for (size_t i = 0; i < node->outputSockets.size(); ++i)
                        {
                            auto& socket = node->outputSockets[i];
                            float dx = mousePos.x - socket.position.x;
                            float dy = mousePos.y - socket.position.y;
                            if (dx * dx + dy * dy <=
                                SOCKET_HIT_RADIUS * SOCKET_HIT_RADIUS * m_graphViewScale * m_graphViewScale)
                            {
                                ConnectSockets(node->id, static_cast<uint32_t>(i), m_connectionStartNodeID,
                                               m_connectionStartSocket);
                                connected = true;
                                break;
                            }
                        }
                    }
                    else
                    {
                        // Started from output, look for input socket
                        for (size_t i = 0; i < node->inputSockets.size(); ++i)
                        {
                            auto& socket = node->inputSockets[i];
                            float dx = mousePos.x - socket.position.x;
                            float dy = mousePos.y - socket.position.y;
                            if (dx * dx + dy * dy <=
                                SOCKET_HIT_RADIUS * SOCKET_HIT_RADIUS * m_graphViewScale * m_graphViewScale)
                            {
                                ConnectSockets(m_connectionStartNodeID, m_connectionStartSocket, node->id,
                                               static_cast<uint32_t>(i));
                                connected = true;
                                break;
                            }
                        }
                    }

                    if (connected)
                        break;
                }

                m_isCreatingConnection = false;
                m_connectionStartNodeID = 0;
                m_connectionStartSocket = 0;
            }
        }
    }

    void VisualScriptingSystem::HandleNodeSelection()
    {
        // Click to select - only if we're not dragging or creating connections
        if (m_isDraggingNode || m_isCreatingConnection)
            return;

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsItemHovered())
        {
            ImVec2 mousePos = ImGui::GetIO().MousePos;
            ImVec2 canvasPos = ImGui::GetCursorScreenPos();
            bool clickedOnNode = false;

            for (auto& node : m_currentScript.nodes)
            {
                if (!node)
                    continue;

                float screenX = canvasPos.x + (node->position.x + m_graphViewOffset.x) * m_graphViewScale;
                float screenY = canvasPos.y + (node->position.y + m_graphViewOffset.y) * m_graphViewScale;
                float nodeWidth = node->size.x * m_graphViewScale;
                float nodeHeight = node->size.y * m_graphViewScale;

                if (mousePos.x >= screenX && mousePos.x <= screenX + nodeWidth && mousePos.y >= screenY &&
                    mousePos.y <= screenY + nodeHeight)
                {
                    clickedOnNode = true;
                    bool isShiftHeld = ImGui::GetIO().KeyShift;

                    if (isShiftHeld)
                    {
                        // Toggle selection
                        auto it = std::find(m_selectedNodes.begin(), m_selectedNodes.end(), node->id);
                        if (it != m_selectedNodes.end())
                        {
                            node->isSelected = false;
                            m_selectedNodes.erase(it);
                        }
                        else
                        {
                            node->isSelected = true;
                            m_selectedNodes.push_back(node->id);
                        }
                    }
                    else
                    {
                        // Clear selection and select this node
                        for (auto& n : m_currentScript.nodes)
                        {
                            if (n)
                                n->isSelected = false;
                        }
                        m_selectedNodes.clear();
                        node->isSelected = true;
                        m_selectedNodes.push_back(node->id);
                    }
                    break;
                }
            }

            // Clicked on empty space - deselect all
            if (!clickedOnNode && !ImGui::GetIO().KeyShift)
            {
                for (auto& node : m_currentScript.nodes)
                {
                    if (node)
                        node->isSelected = false;
                }
                m_selectedNodes.clear();
            }
        }
    }

    void VisualScriptingSystem::HandleCanvasNavigation()
    {
        bool isCanvasHovered = ImGui::IsItemHovered();

        // Pan with middle mouse button
        if (isCanvasHovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
        {
            ImVec2 delta = ImGui::GetIO().MouseDelta;
            m_graphViewOffset.x += delta.x / m_graphViewScale;
            m_graphViewOffset.y += delta.y / m_graphViewScale;
        }

        // Zoom with scroll wheel
        if (isCanvasHovered)
        {
            float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f)
            {
                float oldScale = m_graphViewScale;
                m_graphViewScale *= (wheel > 0.0f) ? 1.1f : 0.9f;
                m_graphViewScale = std::max(0.1f, std::min(4.0f, m_graphViewScale));

                // Zoom toward mouse position
                if (oldScale != m_graphViewScale)
                {
                    ImVec2 mousePos = ImGui::GetIO().MousePos;
                    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
                    float mouseGraphX = (mousePos.x - canvasPos.x) / oldScale - m_graphViewOffset.x;
                    float mouseGraphY = (mousePos.y - canvasPos.y) / oldScale - m_graphViewOffset.y;
                    m_graphViewOffset.x = (mousePos.x - canvasPos.x) / m_graphViewScale - mouseGraphX;
                    m_graphViewOffset.y = (mousePos.y - canvasPos.y) / m_graphViewScale - mouseGraphY;
                }
            }
        }

        // Reset view with Home key
        if (isCanvasHovered && ImGui::IsKeyPressed(ImGuiKey_Home))
        {
            m_graphViewOffset = {0.0f, 0.0f};
            m_graphViewScale = 1.0f;
        }
    }

    void VisualScriptingSystem::RenderGraphOverview()
    {
        // Minimap overlay in the bottom-right corner of the canvas
        constexpr float MINIMAP_WIDTH = 150.0f;
        constexpr float MINIMAP_HEIGHT = 100.0f;
        constexpr float MINIMAP_PADDING = 10.0f;

        if (m_currentScript.nodes.empty())
        {
            return;
        }

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 windowPos = ImGui::GetWindowPos();
        ImVec2 windowSize = ImGui::GetWindowSize();

        ImVec2 minimapMin = ImVec2(windowPos.x + windowSize.x - MINIMAP_WIDTH - MINIMAP_PADDING,
                                   windowPos.y + windowSize.y - MINIMAP_HEIGHT - MINIMAP_PADDING);
        ImVec2 minimapMax = ImVec2(minimapMin.x + MINIMAP_WIDTH, minimapMin.y + MINIMAP_HEIGHT);

        // Minimap background
        drawList->AddRectFilled(minimapMin, minimapMax, IM_COL32(20, 20, 20, 180), 4.0f);
        drawList->AddRect(minimapMin, minimapMax, IM_COL32(80, 80, 80, 200), 4.0f);

        // Calculate bounds of all nodes
        float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
        for (const auto& node : m_currentScript.nodes)
        {
            if (!node)
            {
                continue;
            }
            minX = std::min(minX, node->position.x);
            minY = std::min(minY, node->position.y);
            maxX = std::max(maxX, node->position.x + node->size.x);
            maxY = std::max(maxY, node->position.y + node->size.y);
        }

        float rangeX = std::max(maxX - minX, 1.0f);
        float rangeY = std::max(maxY - minY, 1.0f);

        // Add padding
        float padX = rangeX * 0.1f;
        float padY = rangeY * 0.1f;
        minX -= padX;
        minY -= padY;
        rangeX += padX * 2.0f;
        rangeY += padY * 2.0f;

        float scaleX = (MINIMAP_WIDTH - 4.0f) / rangeX;
        float scaleY = (MINIMAP_HEIGHT - 4.0f) / rangeY;
        float minimapScale = std::min(scaleX, scaleY);

        // Draw nodes as small rectangles
        for (const auto& node : m_currentScript.nodes)
        {
            if (!node)
            {
                continue;
            }

            float nx = minimapMin.x + 2.0f + (node->position.x - minX) * minimapScale;
            float ny = minimapMin.y + 2.0f + (node->position.y - minY) * minimapScale;
            float nw = std::max(3.0f, node->size.x * minimapScale);
            float nh = std::max(2.0f, node->size.y * minimapScale);

            XMFLOAT4 col = GetNodeCategoryColor(node->category);
            ImU32 nodeCol = IM_COL32(static_cast<int>(col.x * 200), static_cast<int>(col.y * 200),
                                     static_cast<int>(col.z * 200), 180);
            drawList->AddRectFilled(ImVec2(nx, ny), ImVec2(nx + nw, ny + nh), nodeCol);
        }

        // Draw viewport rectangle showing visible area
        ImVec2 canvasSize = ImGui::GetContentRegionAvail();
        float vpX = minimapMin.x + 2.0f + (-m_graphViewOffset.x - minX) * minimapScale;
        float vpY = minimapMin.y + 2.0f + (-m_graphViewOffset.y - minY) * minimapScale;
        float vpW = (canvasSize.x / m_graphViewScale) * minimapScale;
        float vpH = (canvasSize.y / m_graphViewScale) * minimapScale;
        drawList->AddRect(ImVec2(vpX, vpY), ImVec2(vpX + vpW, vpY + vpH), IM_COL32(255, 255, 255, 150), 0.0f, 0, 1.0f);
    }

    std::unique_ptr<ScriptNode> VisualScriptingSystem::CreateNode(ScriptNodeType nodeType)
    {
        // Check custom node factories first
        auto factoryIt = m_nodeFactories.find(nodeType);
        if (factoryIt != m_nodeFactories.end())
        {
            return factoryIt->second();
        }

        // Built-in node types
        switch (nodeType)
        {
        // Events
        case ScriptNodeType::EVENT_START:
            return std::make_unique<EventStartNode>();
        case ScriptNodeType::EVENT_UPDATE:
            return std::make_unique<EventUpdateNode>();

        // Flow control
        case ScriptNodeType::BRANCH:
            return std::make_unique<BranchNode>();
        case ScriptNodeType::SEQUENCE:
            return std::make_unique<SequenceNode>();
        case ScriptNodeType::FOR_LOOP:
            return std::make_unique<ForLoopNode>();

        // Math
        case ScriptNodeType::ADD:
            return std::make_unique<MathBinaryNode>(MathBinaryNode::ADD, "Add", nodeType);
        case ScriptNodeType::SUBTRACT:
            return std::make_unique<MathBinaryNode>(MathBinaryNode::SUB, "Subtract", nodeType);
        case ScriptNodeType::MULTIPLY:
            return std::make_unique<MathBinaryNode>(MathBinaryNode::MUL, "Multiply", nodeType);
        case ScriptNodeType::DIVIDE:
            return std::make_unique<MathBinaryNode>(MathBinaryNode::DIV, "Divide", nodeType);
        case ScriptNodeType::POWER:
            return std::make_unique<MathBinaryNode>(MathBinaryNode::POW, "Power", nodeType);
        case ScriptNodeType::SQRT:
            return std::make_unique<MathUnaryNode>(MathUnaryNode::SQRT, "Square Root", nodeType);
        case ScriptNodeType::SIN:
            return std::make_unique<MathUnaryNode>(MathUnaryNode::SIN, "Sine", nodeType);
        case ScriptNodeType::COS:
            return std::make_unique<MathUnaryNode>(MathUnaryNode::COS, "Cosine", nodeType);
        case ScriptNodeType::TAN:
            return std::make_unique<MathUnaryNode>(MathUnaryNode::TAN, "Tangent", nodeType);
        case ScriptNodeType::CLAMP:
            return std::make_unique<ClampNode>();
        case ScriptNodeType::LERP:
            return std::make_unique<LerpNode>();

        // Logic
        case ScriptNodeType::AND:
            return std::make_unique<LogicBinaryNode>(LogicBinaryNode::AND, "AND", nodeType);
        case ScriptNodeType::OR:
            return std::make_unique<LogicBinaryNode>(LogicBinaryNode::OR, "OR", nodeType);
        case ScriptNodeType::XOR:
            return std::make_unique<LogicBinaryNode>(LogicBinaryNode::XOR, "XOR", nodeType);
        case ScriptNodeType::NOT:
            return std::make_unique<NotNode>();

        // Comparison
        case ScriptNodeType::EQUAL:
            return std::make_unique<ComparisonNode>(ComparisonNode::EQ, "Equal", nodeType);
        case ScriptNodeType::NOT_EQUAL:
            return std::make_unique<ComparisonNode>(ComparisonNode::NE, "Not Equal", nodeType);
        case ScriptNodeType::LESS:
            return std::make_unique<ComparisonNode>(ComparisonNode::LT, "Less Than", nodeType);
        case ScriptNodeType::LESS_EQUAL:
            return std::make_unique<ComparisonNode>(ComparisonNode::LE, "Less or Equal", nodeType);
        case ScriptNodeType::GREATER:
            return std::make_unique<ComparisonNode>(ComparisonNode::GT, "Greater Than", nodeType);
        case ScriptNodeType::GREATER_EQUAL:
            return std::make_unique<ComparisonNode>(ComparisonNode::GE, "Greater or Equal", nodeType);

        // Flow control (additional)
        case ScriptNodeType::WHILE_LOOP:
            return std::make_unique<WhileLoopNode>();
        case ScriptNodeType::DELAY:
            return std::make_unique<DelayNode>();
        case ScriptNodeType::GATE:
            return std::make_unique<GateNode>();
        case ScriptNodeType::FLIP_FLOP:
            return std::make_unique<FlipFlopNode>();
        case ScriptNodeType::SWITCH:
            return std::make_unique<SwitchNode>();

        // Strings
        case ScriptNodeType::STRING_CONCAT:
            return std::make_unique<StringConcatNode>();
        case ScriptNodeType::STRING_LENGTH:
            return std::make_unique<StringLengthNode>();
        case ScriptNodeType::STRING_SUBSTRING:
            return std::make_unique<SubstringNode>();
        case ScriptNodeType::STRING_CONTAINS:
            return std::make_unique<StringContainsNode>();
        case ScriptNodeType::STRING_REPLACE:
            return std::make_unique<StringReplaceNode>();
        case ScriptNodeType::STRING_TO_UPPER:
            return std::make_unique<StringToUpperNode>();
        case ScriptNodeType::STRING_TO_LOWER:
            return std::make_unique<StringToLowerNode>();

        // Arrays
        case ScriptNodeType::ARRAY_GET:
            return std::make_unique<ArrayGetNode>();
        case ScriptNodeType::ARRAY_SET:
            return std::make_unique<ArraySetNode>();
        case ScriptNodeType::ARRAY_ADD:
            return std::make_unique<ArrayAddNode>();
        case ScriptNodeType::ARRAY_REMOVE:
            return std::make_unique<ArrayRemoveNode>();
        case ScriptNodeType::ARRAY_LENGTH:
            return std::make_unique<ArrayLengthNode>();
        case ScriptNodeType::ARRAY_CONTAINS:
            return std::make_unique<ArrayContainsNode>();
        case ScriptNodeType::ARRAY_FIND:
            return std::make_unique<ArrayFindNode>();

        // Objects
        case ScriptNodeType::GET_COMPONENT:
            return std::make_unique<GetComponentNode>();
        case ScriptNodeType::SET_TRANSFORM:
            return std::make_unique<SetTransformNode>();
        case ScriptNodeType::GET_TRANSFORM:
            return std::make_unique<GetTransformNode>();
        case ScriptNodeType::DESTROY_OBJECT:
            return std::make_unique<DestroyObjectNode>();
        case ScriptNodeType::INSTANTIATE:
            return std::make_unique<InstantiateNode>();
        case ScriptNodeType::FIND_OBJECT:
            return std::make_unique<FindObjectNode>();

        // Input
        case ScriptNodeType::INPUT_KEY_DOWN:
            return std::make_unique<InputKeyDownNode>();
        case ScriptNodeType::INPUT_KEY_UP:
            return std::make_unique<InputKeyUpNode>();
        case ScriptNodeType::INPUT_MOUSE_BUTTON:
            return std::make_unique<InputMouseButtonNode>();
        case ScriptNodeType::INPUT_MOUSE_POSITION:
            return std::make_unique<InputMousePositionNode>();
        case ScriptNodeType::INPUT_AXIS:
            return std::make_unique<InputAxisNode>();

        // Audio
        case ScriptNodeType::PLAY_SOUND:
            return std::make_unique<PlaySoundNode>();
        case ScriptNodeType::STOP_SOUND:
            return std::make_unique<StopSoundNode>();
        case ScriptNodeType::SET_VOLUME:
            return std::make_unique<SetVolumeNode>();

        // Graphics
        case ScriptNodeType::SET_MATERIAL:
            return std::make_unique<SetMaterialNode>();
        case ScriptNodeType::SET_COLOR:
            return std::make_unique<SetColorNode>();
        case ScriptNodeType::SET_VISIBILITY:
            return std::make_unique<SetVisibilityNode>();

        // Physics
        case ScriptNodeType::ADD_FORCE:
            return std::make_unique<AddForceNode>();
        case ScriptNodeType::SET_VELOCITY:
            return std::make_unique<SetVelocityNode>();
        case ScriptNodeType::RAYCAST:
            return std::make_unique<RaycastNode>();

        // Variables
        case ScriptNodeType::GET_VARIABLE:
            return std::make_unique<GetVariableNode>();
        case ScriptNodeType::SET_VARIABLE:
            return std::make_unique<SetVariableNode>();

        // Functions
        case ScriptNodeType::FUNCTION_CALL:
            return std::make_unique<FunctionCallNode>();
        case ScriptNodeType::FUNCTION_RETURN:
            return std::make_unique<FunctionReturnNode>();

        // Event nodes (additional)
        case ScriptNodeType::EVENT_INPUT_KEY:
            return std::make_unique<EventInputKeyNode>();
        case ScriptNodeType::EVENT_INPUT_MOUSE:
            return std::make_unique<EventInputMouseNode>();
        case ScriptNodeType::EVENT_COLLISION:
            return std::make_unique<EventCollisionNode>();
        case ScriptNodeType::EVENT_TRIGGER:
            return std::make_unique<EventTriggerNode>();
        case ScriptNodeType::EVENT_TIMER:
            return std::make_unique<EventTimerNode>();
        case ScriptNodeType::EVENT_CUSTOM:
            return std::make_unique<EventCustomNode>();

        default:
            return nullptr;
        }
    }

    XMFLOAT4 VisualScriptingSystem::GetNodeCategoryColor(ScriptNodeCategory category) const
    {
        switch (category)
        {
        case ScriptNodeCategory::EVENT:
            return {0.7f, 0.2f, 0.2f, 1.0f}; // Red
        case ScriptNodeCategory::FLOW_CONTROL:
            return {0.8f, 0.8f, 0.8f, 1.0f}; // White/grey
        case ScriptNodeCategory::MATH:
            return {0.2f, 0.6f, 0.2f, 1.0f}; // Green
        case ScriptNodeCategory::LOGIC:
            return {0.3f, 0.3f, 0.7f, 1.0f}; // Blue
        case ScriptNodeCategory::COMPARISON:
            return {0.2f, 0.5f, 0.7f, 1.0f}; // Cyan
        case ScriptNodeCategory::STRING:
            return {0.8f, 0.4f, 0.6f, 1.0f}; // Pink
        case ScriptNodeCategory::ARRAY:
            return {0.6f, 0.4f, 0.2f, 1.0f}; // Brown
        case ScriptNodeCategory::OBJECT:
            return {0.3f, 0.5f, 0.8f, 1.0f}; // Light blue
        case ScriptNodeCategory::INPUT:
            return {0.8f, 0.6f, 0.2f, 1.0f}; // Orange
        case ScriptNodeCategory::AUDIO:
            return {0.6f, 0.2f, 0.6f, 1.0f}; // Purple
        case ScriptNodeCategory::GRAPHICS:
            return {0.2f, 0.7f, 0.7f, 1.0f}; // Teal
        case ScriptNodeCategory::PHYSICS:
            return {0.5f, 0.7f, 0.2f, 1.0f}; // Lime
        case ScriptNodeCategory::VARIABLE:
            return {0.4f, 0.6f, 0.4f, 1.0f}; // Sage
        case ScriptNodeCategory::FUNCTION:
            return {0.5f, 0.3f, 0.7f, 1.0f}; // Violet
        case ScriptNodeCategory::CUSTOM:
            return {0.6f, 0.6f, 0.6f, 1.0f}; // Grey
        default:
            return {0.3f, 0.3f, 0.3f, 1.0f};
        }
    }

    XMFLOAT4 VisualScriptingSystem::GetSocketTypeColor(ScriptVariableType type) const
    {
        switch (type)
        {
        case ScriptVariableType::EXECUTION:
            return {1.0f, 1.0f, 1.0f, 1.0f}; // White
        case ScriptVariableType::BOOLEAN:
            return {0.7f, 0.1f, 0.1f, 1.0f}; // Red
        case ScriptVariableType::INTEGER:
            return {0.1f, 0.8f, 0.8f, 1.0f}; // Cyan
        case ScriptVariableType::FLOAT:
            return {0.1f, 0.8f, 0.1f, 1.0f}; // Green
        case ScriptVariableType::STRING:
            return {0.9f, 0.1f, 0.9f, 1.0f}; // Magenta
        case ScriptVariableType::VECTOR2:
            return {0.9f, 0.7f, 0.1f, 1.0f}; // Gold
        case ScriptVariableType::VECTOR3:
            return {0.9f, 0.9f, 0.1f, 1.0f}; // Yellow
        case ScriptVariableType::VECTOR4:
            return {1.0f, 0.5f, 0.1f, 1.0f}; // Orange
        case ScriptVariableType::COLOR:
            return {1.0f, 1.0f, 1.0f, 1.0f}; // White
        case ScriptVariableType::OBJECT_REFERENCE:
            return {0.1f, 0.4f, 0.9f, 1.0f}; // Blue
        case ScriptVariableType::ARRAY:
            return {0.6f, 0.4f, 0.2f, 1.0f}; // Brown
        default:
            return {0.7f, 0.7f, 0.7f, 1.0f}; // Grey
        }
    }

    void VisualScriptingSystem::InitializeBuiltInNodes()
    {
        // Register category listings for the node palette
        m_nodeCategories["Events"] = {ScriptNodeType::EVENT_START,     ScriptNodeType::EVENT_UPDATE,
                                      ScriptNodeType::EVENT_INPUT_KEY, ScriptNodeType::EVENT_INPUT_MOUSE,
                                      ScriptNodeType::EVENT_COLLISION, ScriptNodeType::EVENT_TRIGGER,
                                      ScriptNodeType::EVENT_TIMER,     ScriptNodeType::EVENT_CUSTOM};
        m_nodeCategories["Flow Control"] = {
            ScriptNodeType::BRANCH, ScriptNodeType::SEQUENCE, ScriptNodeType::FOR_LOOP,  ScriptNodeType::WHILE_LOOP,
            ScriptNodeType::DELAY,  ScriptNodeType::GATE,     ScriptNodeType::FLIP_FLOP, ScriptNodeType::SWITCH};
        m_nodeCategories["Math"] = {ScriptNodeType::ADD,    ScriptNodeType::SUBTRACT, ScriptNodeType::MULTIPLY,
                                    ScriptNodeType::DIVIDE, ScriptNodeType::POWER,    ScriptNodeType::SQRT,
                                    ScriptNodeType::SIN,    ScriptNodeType::COS,      ScriptNodeType::TAN,
                                    ScriptNodeType::CLAMP,  ScriptNodeType::LERP};
        m_nodeCategories["Logic"] = {ScriptNodeType::AND, ScriptNodeType::OR, ScriptNodeType::NOT, ScriptNodeType::XOR};
        m_nodeCategories["Comparison"] = {ScriptNodeType::EQUAL,   ScriptNodeType::NOT_EQUAL,
                                          ScriptNodeType::LESS,    ScriptNodeType::LESS_EQUAL,
                                          ScriptNodeType::GREATER, ScriptNodeType::GREATER_EQUAL};
        m_nodeCategories["Strings"] = {ScriptNodeType::STRING_CONCAT,    ScriptNodeType::STRING_LENGTH,
                                       ScriptNodeType::STRING_SUBSTRING, ScriptNodeType::STRING_CONTAINS,
                                       ScriptNodeType::STRING_REPLACE,   ScriptNodeType::STRING_TO_UPPER,
                                       ScriptNodeType::STRING_TO_LOWER};
        m_nodeCategories["Arrays"] = {ScriptNodeType::ARRAY_GET,    ScriptNodeType::ARRAY_SET,
                                      ScriptNodeType::ARRAY_ADD,    ScriptNodeType::ARRAY_REMOVE,
                                      ScriptNodeType::ARRAY_LENGTH, ScriptNodeType::ARRAY_CONTAINS,
                                      ScriptNodeType::ARRAY_FIND};
        m_nodeCategories["Objects"] = {ScriptNodeType::GET_COMPONENT, ScriptNodeType::SET_TRANSFORM,
                                       ScriptNodeType::GET_TRANSFORM, ScriptNodeType::DESTROY_OBJECT,
                                       ScriptNodeType::INSTANTIATE,   ScriptNodeType::FIND_OBJECT};
        m_nodeCategories["Input"] = {ScriptNodeType::INPUT_KEY_DOWN, ScriptNodeType::INPUT_KEY_UP,
                                     ScriptNodeType::INPUT_MOUSE_BUTTON, ScriptNodeType::INPUT_MOUSE_POSITION,
                                     ScriptNodeType::INPUT_AXIS};
        m_nodeCategories["Audio"] = {ScriptNodeType::PLAY_SOUND, ScriptNodeType::STOP_SOUND,
                                     ScriptNodeType::SET_VOLUME};
        m_nodeCategories["Graphics"] = {ScriptNodeType::SET_MATERIAL, ScriptNodeType::SET_COLOR,
                                        ScriptNodeType::SET_VISIBILITY};
        m_nodeCategories["Physics"] = {ScriptNodeType::ADD_FORCE, ScriptNodeType::SET_VELOCITY,
                                       ScriptNodeType::RAYCAST};
        m_nodeCategories["Variables"] = {ScriptNodeType::GET_VARIABLE, ScriptNodeType::SET_VARIABLE};
        m_nodeCategories["Functions"] = {ScriptNodeType::FUNCTION_CALL, ScriptNodeType::FUNCTION_RETURN};
    }

    void VisualScriptingSystem::UpdateExecution()
    {
        // In continuous execution mode, step the executor
        if (m_isExecuting && m_executionContext && m_executionContext->ShouldStop())
        {
            m_isExecuting = false;
        }
    }

} // namespace SparkEditor
