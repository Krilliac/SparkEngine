/**
 * @file VisualScriptEmitNode.cpp
 * @brief Per-node-type AngelScript statement emission for the visual script compiler
 */

#include "VisualScriptEmitter.h"

#include <algorithm>

namespace Spark::Scripting::Detail
{
    namespace
    {
        /// Node property @p key; failing that, the value of the node's smallest property key
        /// (graphs that named the property differently), else @p fallback. Choosing by key
        /// rather than by unordered_map order keeps the generated source identical on every
        /// standard library.
        std::string PropertyOrSmallestKey(const ScriptNode& node, const char* key, const char* fallback)
        {
            std::string value = PropertyOr(node, key, "");
            if (!value.empty())
            {
                return value;
            }
            if (node.properties.empty())
            {
                return fallback;
            }
            return std::min_element(node.properties.begin(), node.properties.end(),
                                    [](const auto& a, const auto& b) { return a.first < b.first; })
                ->second;
        }
    } // namespace

    /// Emit a node's own statement(s). Branch/ForLoop/Sequence go through EmitStep.
    void VisualScriptEmitter::EmitNode(const ScriptNode& node, const std::string& indent, std::string& code,
                                       bool assignHoistedResult)
    {
        const auto input = [&](uint32_t idx) { return ResolveInput(node, idx); };
        const auto out = [&](uint32_t idx) { return VarName(node.id, idx); };
        const auto line = [&](const std::string& statement) { code += indent + statement + "\n"; };
        const auto constant = [&](PinKind kind)
        {
            ScriptPin pin;
            pin.kind = kind;
            if (!node.outputs.empty())
            {
                pin = node.outputs[0];
            }
            line(PinTypeString(kind) + " " + out(0) + " = " + DefaultLiteral(pin) + ";");
        };

        switch (node.type)
        {
        // -- Constants --
        case ScriptNodeType::ConstFloat:
            constant(PinKind::Float);
            break;
        case ScriptNodeType::ConstInt:
            constant(PinKind::Int);
            break;
        case ScriptNodeType::ConstBool:
            constant(PinKind::Bool);
            break;
        case ScriptNodeType::ConstString:
            constant(PinKind::String);
            break;
        case ScriptNodeType::ConstVector3:
            constant(PinKind::Vector3);
            break;

        // -- Math --
        case ScriptNodeType::Add:
            line("float " + out(0) + " = " + input(0) + " + " + input(1) + ";");
            break;
        case ScriptNodeType::Subtract:
            line("float " + out(0) + " = " + input(0) + " - " + input(1) + ";");
            break;
        case ScriptNodeType::Multiply:
            line("float " + out(0) + " = " + input(0) + " * " + input(1) + ";");
            break;
        case ScriptNodeType::Divide:
            line("float " + out(0) + " = (" + input(1) + " != 0.0f) ? " + input(0) + " / " + input(1) + " : 0.0f;");
            break;
        case ScriptNodeType::Negate:
            line("float " + out(0) + " = -" + input(0) + ";");
            break;
        case ScriptNodeType::Abs:
            line("float " + out(0) + " = abs(" + input(0) + ");");
            break;
        case ScriptNodeType::Lerp:
            line("float " + out(0) + " = " + input(0) + " + (" + input(1) + " - " + input(0) + ") * " + input(2) + ";");
            break;
        case ScriptNodeType::Clamp:
        {
            const std::string value = "_v" + std::to_string(node.id);
            line("float " + value + " = " + input(0) + ";");
            line("float " + out(0) + " = (" + value + " < " + input(1) + ") ? " + input(1) + " : ((" + value + " > " +
                 input(2) + ") ? " + input(2) + " : " + value + ");");
            break;
        }
        case ScriptNodeType::Random:
            line("float " + out(0) + " = float(rand()) / float(2147483647);");
            break;
        case ScriptNodeType::RandomRange:
            line("float " + out(0) + " = " + input(0) + " + float(rand()) / float(2147483647) * (" + input(1) + " - " +
                 input(0) + ");");
            break;
        case ScriptNodeType::BreakVector3:
        {
            const std::string vector = input(0);
            line("float " + out(0) + " = " + vector + ".x;");
            line("float " + out(1) + " = " + vector + ".y;");
            line("float " + out(2) + " = " + vector + ".z;");
            break;
        }
        case ScriptNodeType::MakeVector3:
            line("Vector3 " + out(0) + ";");
            line(out(0) + ".x = " + input(0) + ";");
            line(out(0) + ".y = " + input(1) + ";");
            line(out(0) + ".z = " + input(2) + ";");
            break;
        case ScriptNodeType::ToInt:
            line("int " + out(0) + " = int(" + input(0) + ");");
            break;
        case ScriptNodeType::AppendString:
            line("string " + out(0) + " = " + input(0) + " + " + input(1) + ";");
            break;

        // -- Logic --
        case ScriptNodeType::And:
            line("bool " + out(0) + " = " + input(0) + " && " + input(1) + ";");
            break;
        case ScriptNodeType::Or:
            line("bool " + out(0) + " = " + input(0) + " || " + input(1) + ";");
            break;
        case ScriptNodeType::Not:
            line("bool " + out(0) + " = !" + input(0) + ";");
            break;
        case ScriptNodeType::Equal:
            line("bool " + out(0) + " = (" + input(0) + " == " + input(1) + ");");
            break;
        case ScriptNodeType::NotEqual:
            line("bool " + out(0) + " = (" + input(0) + " != " + input(1) + ");");
            break;
        case ScriptNodeType::Greater:
            line("bool " + out(0) + " = (" + input(0) + " > " + input(1) + ");");
            break;
        case ScriptNodeType::Less:
            line("bool " + out(0) + " = (" + input(0) + " < " + input(1) + ");");
            break;
        case ScriptNodeType::GreaterEqual:
            line("bool " + out(0) + " = (" + input(0) + " >= " + input(1) + ");");
            break;
        case ScriptNodeType::LessEqual:
            line("bool " + out(0) + " = (" + input(0) + " <= " + input(1) + ");");
            break;
        case ScriptNodeType::Select:
            line(PinTypeString(OutputKind(node, 0, PinKind::Float)) + " " + out(0) + " = " + input(0) + " ? " +
                 input(1) + " : " + input(2) + ";");
            break;

        // -- Getters --
        case ScriptNodeType::GetKeyDown:
        case ScriptNodeType::GetKey:
        {
            const std::string key = PropertyOrSmallestKey(node, "key", "Space");
            const char* call = node.type == ScriptNodeType::GetKeyDown ? "getKeyDown" : "getKey";
            line("bool " + out(0) + " = " + call + "(\"" + EscapeAngelScriptString(key) + "\");");
            break;
        }
        case ScriptNodeType::GetDeltaTime:
            line("float " + out(0) + " = dt;");
            break;
        case ScriptNodeType::GetSelf:
            line("uint " + out(0) + " = selfEntity;");
            break;
        case ScriptNodeType::GetPosition:
            line("Vector3 " + out(0) + " = getPosition(" + input(0) + ");");
            break;
        case ScriptNodeType::GetRotation:
            line("Vector3 " + out(0) + " = getRotation(" + input(0) + ");");
            break;
        case ScriptNodeType::GetHealth:
            line("float " + out(0) + " = getHealth(" + input(0) + ");");
            break;
        case ScriptNodeType::GetSpeed:
            line("float " + out(0) + " = getSpeed(" + input(0) + ");");
            break;
        case ScriptNodeType::GetEntityByName:
            line("uint " + out(0) + " = getEntityByName(\"" +
                 EscapeAngelScriptString(PropertyOr(node, "name", "Entity")) + "\");");
            break;

        // -- Actions (input[0] is the Exec pin, data starts at input[1]) --
        case ScriptNodeType::PrintMessage:
            line("print(" + input(1) + ");");
            break;
        case ScriptNodeType::SetPosition:
            line("setPosition(" + input(1) + ", " + input(2) + ");");
            break;
        case ScriptNodeType::SetRotation:
            line("setRotation(" + input(1) + ", " + input(2) + ");");
            break;
        case ScriptNodeType::SetHealth:
            line("setHealth(" + input(1) + ", " + input(2) + ");");
            break;
        case ScriptNodeType::ApplyForce:
            line("applyForce(" + input(1) + ", " + input(2) + ");");
            break;
        case ScriptNodeType::PlaySound:
        case ScriptNodeType::PlayAnimation:
        case ScriptNodeType::FireEvent:
        {
            const char* key = node.type == ScriptNodeType::PlaySound       ? "sound"
                              : node.type == ScriptNodeType::PlayAnimation ? "animation"
                                                                           : "event";
            const std::string literal = "\"" + EscapeAngelScriptString(PropertyOrSmallestKey(node, key, "")) + "\"";
            if (node.type == ScriptNodeType::PlaySound)
            {
                line("playSound(selfEntity, " + literal + ");");
            }
            else if (node.type == ScriptNodeType::PlayAnimation)
            {
                line("playAnimation(selfEntity, " + literal + ");");
            }
            else
            {
                line("fireEvent(" + literal + ");");
            }
            break;
        }
        case ScriptNodeType::SpawnEntity:
        {
            const std::string name = PropertyOrSmallestKey(node, "name", "Entity");
            line("uint " + out(FirstDataOutput(node)) + " = createEntity(\"" + EscapeAngelScriptString(name) + "\");");
            break;
        }
        case ScriptNodeType::DestroyEntity:
            line("destroyEntity(" + input(1) + ");");
            break;
        case ScriptNodeType::DoNothing:
        case ScriptNodeType::Branch:
        case ScriptNodeType::ForLoop:
        case ScriptNodeType::Sequence:
        case ScriptNodeType::DefineCustomEvent:
            break;

        // -- Variables --
        case ScriptNodeType::GetVariable:
        {
            const std::string name = SanitizeIdentifier(PropertyOr(node, "name", "var"), "var");
            line(PinTypeString(OutputKind(node, 0, PinKind::Float)) + " " + out(0) + " = " + name + ";");
            break;
        }
        case ScriptNodeType::SetVariable:
            line(SanitizeIdentifier(PropertyOr(node, "name", "var"), "var") + " = " + input(1) + ";");
            break;

        // -- Custom events & functions --
        case ScriptNodeType::CallFunction:
        {
            const std::string function = SanitizeIdentifier(PropertyOr(node, "function", "myFunction"), "myFunction");
            std::string args;
            for (uint32_t i = 0; i < static_cast<uint32_t>(node.inputs.size()); ++i)
            {
                if (node.inputs[i].kind == PinKind::Execution)
                {
                    continue;
                }
                if (!args.empty())
                {
                    args += ", ";
                }
                args += input(i);
            }
            const uint32_t result = FirstDataOutput(node);
            const std::string call = function + "(" + args + ");";
            if (OutputKind(node, result, PinKind::Execution) == PinKind::Execution)
            {
                line(call);
            }
            else if (assignHoistedResult)
            {
                line(out(result) + " = " + call);
            }
            else
            {
                line(PinTypeString(node.outputs[result].kind) + " " + out(result) + " = " + call);
            }
            break;
        }
        case ScriptNodeType::ReturnValue:
        {
            // The value is the first data input (the editor lays out Exec then Value); a node
            // declaring only an execution input returns from a void function.
            const auto value = std::find_if(node.inputs.begin(), node.inputs.end(),
                                            [](const ScriptPin& pin) { return pin.kind != PinKind::Execution; });
            if (node.inputs.empty())
            {
                line("return " + input(0) + ";");
            }
            else if (value == node.inputs.end())
            {
                line("return;");
            }
            else
            {
                line("return " + input(static_cast<uint32_t>(value - node.inputs.begin())) + ";");
            }
            break;
        }

        // -- Vector math --
        case ScriptNodeType::Normalize:
            line("Vector3 " + out(0) + " = normalize(" + input(0) + ");");
            break;
        case ScriptNodeType::DotProduct:
            line("float " + out(0) + " = dot(" + input(0) + ", " + input(1) + ");");
            break;
        case ScriptNodeType::Distance:
            line("float " + out(0) + " = distance(" + input(0) + ", " + input(1) + ");");
            break;

        default:
            line("// Unhandled node type " + std::to_string(static_cast<uint32_t>(node.type)));
            break;
        }
    }

} // namespace Spark::Scripting::Detail
