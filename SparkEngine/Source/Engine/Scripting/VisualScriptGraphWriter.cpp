/**
 * @file VisualScriptGraphWriter.cpp
 * @brief Canonical .vscript serialization and atomic graph file saves
 */

#include "VisualScriptGraphIO.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <utility>
#include <vector>

namespace Spark::Scripting
{
    namespace
    {

        /// JSON string literal. Control characters are \u-escaped; other bytes (UTF-8) pass through.
        std::string Quote(const std::string& text)
        {
            std::string out = "\"";
            for (const char c : text)
            {
                switch (c)
                {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
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
                    if (static_cast<unsigned char>(c) < 0x20)
                    {
                        char escaped[8];
                        std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(c));
                        out += escaped;
                    }
                    else
                    {
                        out += c;
                    }
                    break;
                }
            }
            return out + "\"";
        }

        /// Shortest text that parses back to exactly @p value.
        std::string Number(float value)
        {
            if (value == 0.0f || !std::isfinite(value))
                return "0";
            char buffer[32];
            const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value);
            return error == std::errc{} ? std::string(buffer, end) : "0";
        }

        std::string PinText(const ScriptPin& pin)
        {
            std::string text = "{\"kind\": " + Quote(VisualScriptGraphIO::PinKindName(pin.kind));
            switch (pin.kind)
            {
            case PinKind::Bool:
                if (pin.defaultValue[0] != 0.0f)
                    text += ", \"default\": true";
                break;
            case PinKind::Int:
            case PinKind::Entity:
            case PinKind::Float:
                if (pin.defaultValue[0] != 0.0f)
                    text += ", \"default\": " + Number(pin.defaultValue[0]);
                break;
            case PinKind::String:
                if (!pin.defaultString.empty())
                    text += ", \"default\": " + Quote(pin.defaultString);
                break;
            case PinKind::Vector3:
                if (pin.defaultValue[0] != 0.0f || pin.defaultValue[1] != 0.0f || pin.defaultValue[2] != 0.0f)
                {
                    text += ", \"default\": [" + Number(pin.defaultValue[0]) + ", " + Number(pin.defaultValue[1]) +
                            ", " + Number(pin.defaultValue[2]) + "]";
                }
                break;
            default:
                break;
            }
            return text + "}";
        }

        std::string PinListText(const std::vector<ScriptPin>& pins)
        {
            std::string text = "[";
            for (size_t i = 0; i < pins.size(); ++i)
                text += (i > 0 ? ", " : "") + PinText(pins[i]);
            return text + "]";
        }

        std::string NodeText(const ScriptNode& node)
        {
            const char* typeName = VisualScriptGraphIO::NodeTypeName(node.type);
            std::string text = "{\"id\": " + std::to_string(node.id) + ", \"type\": " +
                               Quote(typeName ? typeName : std::to_string(static_cast<uint32_t>(node.type))) +
                               ", \"position\": [" + Number(node.editorX) + ", " + Number(node.editorY) + "]";
            if (!node.inputs.empty())
                text += ", \"inputs\": " + PinListText(node.inputs);
            if (!node.outputs.empty())
                text += ", \"outputs\": " + PinListText(node.outputs);
            if (!node.properties.empty())
            {
                std::vector<std::pair<std::string, std::string>> sorted(node.properties.begin(), node.properties.end());
                std::sort(sorted.begin(), sorted.end());
                text += ", \"properties\": {";
                for (size_t i = 0; i < sorted.size(); ++i)
                    text += (i > 0 ? ", " : "") + Quote(sorted[i].first) + ": " + Quote(sorted[i].second);
                text += "}";
            }
            return text + "}";
        }

        std::string ConnectionText(const ScriptConnection& conn)
        {
            return "{\"from\": [" + std::to_string(conn.fromNode) + ", " + std::to_string(conn.fromPin) +
                   "], \"to\": [" + std::to_string(conn.toNode) + ", " + std::to_string(conn.toPin) + "]}";
        }

        std::string ParametersText(const std::vector<VariableDecl>& parameters)
        {
            std::string text = "[";
            for (size_t i = 0; i < parameters.size(); ++i)
            {
                text += (i > 0 ? ", " : "") + std::string("{\"name\": ") + Quote(parameters[i].name) +
                        ", \"type\": " + Quote(VisualScriptGraphIO::PinKindName(parameters[i].type)) + "}";
            }
            return text + "]";
        }

        /// A top-level array with one element per line.
        void AppendArray(std::string& out, const char* key, const std::vector<std::string>& elements, bool last)
        {
            out += "  \"" + std::string(key) + "\": [";
            for (size_t i = 0; i < elements.size(); ++i)
                out += std::string(i > 0 ? "," : "") + "\n    " + elements[i];
            out += elements.empty() ? "]" : "\n  ]";
            out += last ? "\n" : ",\n";
        }

        template <typename T, typename Fn> std::vector<std::string> Lines(const std::vector<T>& items, Fn&& toText)
        {
            std::vector<std::string> lines;
            lines.reserve(items.size());
            for (const auto& item : items)
                lines.push_back(toText(item));
            return lines;
        }
    } // namespace

    std::string VisualScriptGraphIO::Serialize(const VisualScriptGraph& graph)
    {
        std::string out = "{\n";
        out += "  \"format\": " + Quote(std::string(kFormat)) + ",\n";
        out += "  \"version\": " + std::to_string(kVersion) + ",\n";
        out += "  \"className\": " + Quote(graph.className) + ",\n";
        if (!graph.description.empty())
            out += "  \"description\": " + Quote(graph.description) + ",\n";

        AppendArray(out, "variables",
                    Lines(graph.variables,
                          [](const VariableDecl& var)
                          {
                              std::string text =
                                  "{\"name\": " + Quote(var.name) + ", \"type\": " + Quote(PinKindName(var.type));
                              if (!var.defaultValue.empty())
                                  text += ", \"default\": " + Quote(var.defaultValue);
                              return text + "}";
                          }),
                    false);
        AppendArray(out, "nodes", Lines(graph.nodes, NodeText), false);
        AppendArray(out, "connections", Lines(graph.connections, ConnectionText), false);
        AppendArray(out, "functions",
                    Lines(graph.functions,
                          [](const FunctionGraph& function)
                          {
                              std::string text = "{\"name\": " + Quote(function.name) +
                                                 ", \"returnType\": " + Quote(PinKindName(function.returnType)) +
                                                 ", \"parameters\": " + ParametersText(function.parameters) +
                                                 ", \"nodes\": [";
                              for (size_t i = 0; i < function.nodes.size(); ++i)
                                  text += (i > 0 ? ", " : "") + NodeText(function.nodes[i]);
                              text += "], \"connections\": [";
                              for (size_t i = 0; i < function.connections.size(); ++i)
                                  text += (i > 0 ? ", " : "") + ConnectionText(function.connections[i]);
                              return text + "]}";
                          }),
                    false);
        AppendArray(out, "customEvents",
                    Lines(graph.customEvents,
                          [](const CustomEventDef& event) {
                              return "{\"name\": " + Quote(event.name) +
                                     ", \"parameters\": " + ParametersText(event.parameters) + "}";
                          }),
                    true);
        out += "}\n";
        return out;
    }

    std::expected<void, std::string> VisualScriptGraphIO::SaveFile(const std::filesystem::path& path,
                                                                   const VisualScriptGraph& graph)
    {
        const std::string name = path.generic_string();
        const std::string text = Serialize(graph);
        if (auto check = Parse(text); !check)
            return std::unexpected(name + ": refusing to save a graph that would not load: " + check.error());

        std::filesystem::path temporary = path;
        temporary += ".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            stream.write(text.data(), static_cast<std::streamsize>(text.size()));
            stream.flush();
            if (!stream)
            {
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                return std::unexpected(name + ": cannot write " + temporary.generic_string());
            }
        }

        std::error_code error;
        std::filesystem::rename(temporary, path, error);
        if (error)
        {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return std::unexpected(name + ": cannot replace (" + error.message() + ")");
        }
        return {};
    }

} // namespace Spark::Scripting
