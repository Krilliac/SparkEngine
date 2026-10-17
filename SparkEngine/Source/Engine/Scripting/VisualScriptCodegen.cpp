/**
 * @file VisualScriptCodegen.cpp
 * @brief Literal, identifier and node-classification rules shared by the visual script compiler and emitter
 */

#include "VisualScriptEmitter.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>

namespace Spark::Scripting::Detail
{
    namespace
    {
        bool IsDigits(std::string_view text)
        {
            return !text.empty() &&
                   std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
        }

        bool IsActionNode(ScriptNodeType type)
        {
            const auto val = static_cast<uint32_t>(type);
            // Action nodes: flow control (50-53) and setters (150-159)
            return (val >= 50 && val <= 53) || (val >= 150 && val <= 159);
        }

        /// Float literal that parses back to exactly @p value. The fixed six-decimal
        /// form is kept whenever it round-trips (stable output for ordinary values);
        /// otherwise the shortest round-trip form is used.
        std::string FloatLiteral(float value)
        {
            if (!std::isfinite(value))
            {
                return "0.0f";
            }

            std::string fixed = std::to_string(value);
            if (std::strtof(fixed.c_str(), nullptr) == value)
            {
                return fixed + "f";
            }

            char buffer[32];
            const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value);
            std::string shortest(buffer, error == std::errc{} ? end : buffer);
            const size_t exponent = shortest.find('e');
            if (shortest.find('.') == std::string::npos)
            {
                shortest.insert(exponent == std::string::npos ? shortest.size() : exponent, ".0");
            }
            return shortest + "f";
        }
    } // namespace

    /// Escape a raw string so it is safe to splice into a double-quoted
    /// AngelScript string literal. Node properties (sound names, event
    /// names, key names, etc.) are author/save-file controlled and must
    /// never be able to break out of the literal and inject statements.
    std::string EscapeAngelScriptString(const std::string& raw)
    {
        std::string out;
        out.reserve(raw.size() + 8);
        for (unsigned char c : raw)
        {
            switch (c)
            {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                // Other raw control characters are stripped rather than emitted.
                if (c >= 0x20)
                {
                    out += static_cast<char>(c);
                }
                break;
            }
        }
        return out;
    }

    /// Sanitize a raw string so it is safe to splice into generated
    /// AngelScript as a bare identifier (variable/function/class/method
    /// name). Any character outside [A-Za-z0-9_] becomes '_', and a
    /// leading digit is prefixed with '_' — this makes it structurally
    /// impossible for a property value to close a statement and inject
    /// new AngelScript code via an "identifier" position.
    std::string SanitizeIdentifier(const std::string& raw, const char* fallback)
    {
        if (raw.empty())
        {
            return fallback;
        }

        std::string out;
        out.reserve(raw.size());
        for (unsigned char c : raw)
        {
            if (std::isalnum(c) || c == '_')
            {
                out += static_cast<char>(c);
            }
            else
            {
                out += '_';
            }
        }
        if (std::isdigit(static_cast<unsigned char>(out[0])))
        {
            out.insert(out.begin(), '_');
        }

        return out;
    }

    std::string VarName(uint32_t nodeID, uint32_t pinIndex)
    {
        return "n" + std::to_string(nodeID) + "_out" + std::to_string(pinIndex);
    }

    std::string PinTypeString(PinKind kind)
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

    std::string DefaultLiteral(const ScriptPin& pin)
    {
        switch (pin.kind)
        {
        case PinKind::Bool:
            return pin.defaultValue[0] != 0.0f ? "true" : "false";
        case PinKind::Int:
            return std::to_string(static_cast<int>(pin.defaultValue[0]));
        case PinKind::Float:
            return FloatLiteral(pin.defaultValue[0]);
        case PinKind::String:
            return "\"" + EscapeAngelScriptString(pin.defaultString) + "\"";
        case PinKind::Vector3:
            return "Vector3(" + FloatLiteral(pin.defaultValue[0]) + ", " + FloatLiteral(pin.defaultValue[1]) + ", " +
                   FloatLiteral(pin.defaultValue[2]) + ")";
        case PinKind::Entity:
            return std::to_string(static_cast<uint32_t>(pin.defaultValue[0]));
        default:
            return "0.0f";
        }
    }

    /// A variable default is spliced verbatim after "=", so it must be a
    /// single literal of the variable's type and nothing else.
    bool IsVariableDefaultLiteral(PinKind kind, std::string_view text)
    {
        if (text.empty())
        {
            return true;
        }

        switch (kind)
        {
        case PinKind::Bool:
            return text == "true" || text == "false";
        case PinKind::Int:
            return IsDigits(text.starts_with('-') ? text.substr(1) : text);
        case PinKind::Entity:
            return IsDigits(text);
        case PinKind::Float:
        {
            std::string_view rest = text.starts_with('-') ? text.substr(1) : text;
            if (rest.ends_with('f'))
            {
                rest.remove_suffix(1);
            }
            const size_t exponent = rest.find_first_of("eE");
            std::string_view mantissa = rest.substr(0, exponent);
            if (exponent != std::string_view::npos)
            {
                std::string_view power = rest.substr(exponent + 1);
                if (power.starts_with('-') || power.starts_with('+'))
                {
                    power.remove_prefix(1);
                }
                if (!IsDigits(power))
                {
                    return false;
                }
            }
            const size_t dot = mantissa.find('.');
            if (dot == std::string_view::npos)
            {
                return IsDigits(mantissa);
            }
            return IsDigits(mantissa.substr(0, dot)) && IsDigits(mantissa.substr(dot + 1));
        }
        case PinKind::String:
        {
            if (text.size() < 2 || text.front() != '"' || text.back() != '"')
            {
                return false;
            }
            const std::string_view inner = text.substr(1, text.size() - 2);
            for (size_t i = 0; i < inner.size(); ++i)
            {
                const auto c = static_cast<unsigned char>(inner[i]);
                if (c < 0x20 || c == '"')
                {
                    return false;
                }
                if (c == '\\')
                {
                    if (i + 1 >= inner.size() || std::string_view("\\\"nrt").find(inner[i + 1]) == std::string::npos)
                    {
                        return false;
                    }
                    ++i;
                }
            }
            return true;
        }
        default:
            // Vector3 defaults are not accepted as text, and Any/Execution have no literal form.
            return false;
        }
    }

    bool IsEventNode(ScriptNodeType type)
    {
        return type == ScriptNodeType::OnStart || type == ScriptNodeType::OnUpdate ||
               type == ScriptNodeType::OnTriggerEnter || type == ScriptNodeType::OnTriggerExit ||
               type == ScriptNodeType::OnDamaged || type == ScriptNodeType::OnKeyPress ||
               type == ScriptNodeType::OnCollision || type == ScriptNodeType::OnCustomEvent ||
               type == ScriptNodeType::DefineCustomEvent;
    }

    /// Pure nodes have no execution pins and no side effects; they are
    /// re-evaluated wherever a statement consumes their output.
    bool IsPureNode(const ScriptNode& node)
    {
        if (IsEventNode(node.type) || IsActionNode(node.type))
        {
            return false;
        }
        switch (node.type)
        {
        case ScriptNodeType::SetVariable:
        case ScriptNodeType::ReturnValue:
        case ScriptNodeType::Comment:
            return false;
        default:
            break;
        }
        const auto isExec = [](const ScriptPin& pin) { return pin.kind == PinKind::Execution; };
        return std::none_of(node.inputs.begin(), node.inputs.end(), isExec) &&
               std::none_of(node.outputs.begin(), node.outputs.end(), isExec);
    }

    /// Index of a node's first data output (the result pin), or 0 without pin metadata.
    uint32_t FirstDataOutput(const ScriptNode& node)
    {
        for (uint32_t i = 0; i < static_cast<uint32_t>(node.outputs.size()); ++i)
        {
            if (node.outputs[i].kind != PinKind::Execution)
            {
                return i;
            }
        }
        return 0;
    }

    PinKind OutputKind(const ScriptNode& node, uint32_t index, PinKind fallback)
    {
        return index < node.outputs.size() ? node.outputs[index].kind : fallback;
    }

    std::string PropertyOr(const ScriptNode& node, const char* key, const char* fallback)
    {
        const auto it = node.properties.find(key);
        if (it != node.properties.end() && !it->second.empty())
        {
            return it->second;
        }
        return fallback;
    }

} // namespace Spark::Scripting::Detail
