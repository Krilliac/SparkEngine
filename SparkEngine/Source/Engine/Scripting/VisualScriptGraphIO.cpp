/**
 * @file VisualScriptGraphIO.cpp
 * @brief .vscript graph parsing (fail-closed validation), file loading and node/pin names
 */

#include "VisualScriptGraphIO.h"

#include "../../Utils/JsonUtils.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <format>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Spark::Scripting
{
    namespace
    {
        using Json = Spark::Json::Value;

        constexpr size_t kMaxNodes = 16384;
        constexpr size_t kMaxConnections = 65536;
        constexpr size_t kMaxPinsPerNode = 64;
        constexpr size_t kMaxProperties = 64;
        constexpr size_t kMaxNameBytes = 256;
        constexpr size_t kMaxTextBytes = 64u * 1024u;
        constexpr size_t kMaxVariables = 1024;
        constexpr size_t kMaxFunctions = 256;
        constexpr double kMaxExactInteger = 16777216.0; // 2^24: largest range a float pin default holds exactly

        // ====================================================================
        // Parsing
        // ====================================================================

        /// Thrown only inside this file to unwind a failed parse; Parse() converts it to an error value.
        struct ParseError
        {
            std::string message;
        };

        [[noreturn]] void Fail(const std::string& where, const std::string& what)
        {
            throw ParseError{where + ": " + what};
        }

        void RequireKeys(const Json& object, const std::string& where, std::initializer_list<const char*> required,
                         std::initializer_list<const char*> optional)
        {
            if (!object.IsObject())
                Fail(where, "must be an object");
            for (const char* key : required)
            {
                if (!object.HasKey(key))
                    Fail(where, std::string("missing required key '") + key + "'");
            }
            for (const auto& key : object.GetKeys())
            {
                const auto matches = [&](const char* candidate) { return key == candidate; };
                if (std::none_of(required.begin(), required.end(), matches) &&
                    std::none_of(optional.begin(), optional.end(), matches))
                {
                    Fail(where, "unknown key '" + key + "'");
                }
            }
        }

        const Json& ArrayAt(const Json& object, const char* key, const std::string& where, size_t limit)
        {
            static const Json kEmpty = Json::MakeArray();
            if (!object.HasKey(key))
                return kEmpty;
            const Json& value = object[key];
            if (!value.IsArray())
                Fail(where + "." + key, "must be an array");
            if (value.Size() > limit)
                Fail(where + "." + key,
                     "has " + std::to_string(value.Size()) + " elements; the limit is " + std::to_string(limit));
            return value;
        }

        std::string ReadString(const Json& value, const std::string& where, size_t limit)
        {
            if (!value.IsString())
                Fail(where, "must be a string");
            const auto& text = value.AsString();
            if (text.size() > limit)
                Fail(where, "is longer than " + std::to_string(limit) + " bytes");
            return text;
        }

        /// A JSON number that is a whole value in [minimum, maximum].
        double ReadInteger(const Json& value, const std::string& where, double minimum, double maximum)
        {
            const double number = value.IsNumber() ? value.AsNumber() : 0.5;
            if (!value.IsNumber() || std::floor(number) != number || number < minimum || number > maximum)
            {
                Fail(where, "must be an integer in [" + std::to_string(static_cast<int64_t>(minimum)) + ", " +
                                std::to_string(static_cast<int64_t>(maximum)) + "]");
            }
            return number;
        }

        bool IsIdentifier(std::string_view text)
        {
            if (text.empty() || std::isdigit(static_cast<unsigned char>(text.front())))
                return false;
            return std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
        }

        std::string ReadIdentifier(const Json& value, const std::string& where)
        {
            std::string text = ReadString(value, where, kMaxNameBytes);
            if (!IsIdentifier(text))
                Fail(where, "'" + text + "' is not an identifier ([A-Za-z_][A-Za-z0-9_]*)");
            return text;
        }

        uint32_t ReadUInt32(const Json& value, const std::string& where)
        {
            return static_cast<uint32_t>(ReadInteger(value, where, 0.0, std::numeric_limits<uint32_t>::max()));
        }

        float ReadFloat(const Json& value, const std::string& where)
        {
            if (!value.IsNumber())
                Fail(where, "must be a number");
            const double number = value.AsNumber();
            if (!std::isfinite(number) || std::fabs(number) > std::numeric_limits<float>::max())
                Fail(where, "is not a finite float");
            return static_cast<float>(number);
        }

        PinKind ReadPinKind(const Json& value, const std::string& where)
        {
            const std::string name = ReadString(value, where, kMaxNameBytes);
            const auto kind = VisualScriptGraphIO::PinKindFromName(name);
            if (!kind)
                Fail(where, "unknown pin kind '" + name + "'");
            return *kind;
        }

        /// Data kinds a variable, parameter or return value may have (Execution only as a void return).
        PinKind ReadDataKind(const Json& value, const std::string& where, bool allowVoid)
        {
            const PinKind kind = ReadPinKind(value, where);
            if (kind == PinKind::Any || (kind == PinKind::Execution && !allowVoid))
                Fail(where, std::string("'") + VisualScriptGraphIO::PinKindName(kind) + "' is not a value type");
            return kind;
        }

        ScriptPin ReadPin(const Json& object, const std::string& where)
        {
            RequireKeys(object, where, {"kind"}, {"default"});
            ScriptPin pin;
            pin.kind = ReadPinKind(object["kind"], where + ".kind");
            if (!object.HasKey("default"))
                return pin;

            const Json& value = object["default"];
            const std::string at = where + ".default";
            switch (pin.kind)
            {
            case PinKind::Bool:
                if (!value.IsBool())
                    Fail(at, "must be a boolean for a Bool pin");
                pin.defaultValue[0] = value.AsBool() ? 1.0f : 0.0f;
                break;
            case PinKind::Int:
            case PinKind::Entity:
                // Pin defaults are stored as float: only integers a float holds exactly are accepted.
                pin.defaultValue[0] = static_cast<float>(
                    ReadInteger(value, at, pin.kind == PinKind::Int ? -kMaxExactInteger : 0.0, kMaxExactInteger));
                break;
            case PinKind::Float:
                pin.defaultValue[0] = ReadFloat(value, at);
                break;
            case PinKind::String:
                pin.defaultString = ReadString(value, at, kMaxTextBytes);
                break;
            case PinKind::Vector3:
                if (!value.IsArray() || value.Size() != 3)
                    Fail(at, "must be an [x, y, z] array for a Vector3 pin");
                for (size_t i = 0; i < 3; ++i)
                    pin.defaultValue[i] = ReadFloat(value[i], at + "[" + std::to_string(i) + "]");
                break;
            default:
                Fail(at, std::string("a ") + VisualScriptGraphIO::PinKindName(pin.kind) + " pin has no default");
            }
            return pin;
        }

        std::vector<ScriptPin> ReadPins(const Json& object, const char* key, const std::string& where)
        {
            std::vector<ScriptPin> pins;
            const Json& array = ArrayAt(object, key, where, kMaxPinsPerNode);
            for (size_t i = 0; i < array.Size(); ++i)
                pins.push_back(ReadPin(array[i], where + "." + key + "[" + std::to_string(i) + "]"));
            return pins;
        }

        ScriptNode ReadNode(const Json& object, const std::string& where)
        {
            RequireKeys(object, where, {"id", "type"}, {"position", "inputs", "outputs", "properties"});
            ScriptNode node;
            node.id = ReadUInt32(object["id"], where + ".id");
            if (node.id == 0)
                Fail(where + ".id", "0 is reserved; node ids start at 1");
            if (node.id > VisualScriptGraphIO::kMaxNodeId)
                Fail(where + ".id", "exceeds the largest node id " + std::to_string(VisualScriptGraphIO::kMaxNodeId));

            const std::string typeName = ReadString(object["type"], where + ".type", kMaxNameBytes);
            const auto type = VisualScriptGraphIO::NodeTypeFromName(typeName);
            if (!type)
                Fail(where + ".type", "unknown node type '" + typeName + "'");
            node.type = *type;

            if (object.HasKey("position"))
            {
                const Json& position = object["position"];
                if (!position.IsArray() || position.Size() != 2)
                    Fail(where + ".position", "must be an [x, y] array");
                node.editorX = ReadFloat(position[0], where + ".position[0]");
                node.editorY = ReadFloat(position[1], where + ".position[1]");
            }

            node.inputs = ReadPins(object, "inputs", where);
            node.outputs = ReadPins(object, "outputs", where);

            if (object.HasKey("properties"))
            {
                const Json& properties = object["properties"];
                if (!properties.IsObject())
                    Fail(where + ".properties", "must be an object of strings");
                if (properties.Size() > kMaxProperties)
                    Fail(where + ".properties", "has more than " + std::to_string(kMaxProperties) + " entries");
                for (const auto& key : properties.GetKeys())
                {
                    if (key.empty() || key.size() > kMaxNameBytes)
                        Fail(where + ".properties", "has an empty or oversized key");
                    node.properties[key] = ReadString(properties[key], where + ".properties." + key, kMaxTextBytes);
                }
            }
            return node;
        }

        ScriptConnection ReadConnection(const Json& object, const std::string& where)
        {
            RequireKeys(object, where, {"from", "to"}, {});
            const auto endpoint = [&](const char* key, uint32_t& nodeId, uint32_t& pin)
            {
                const Json& value = object[key];
                if (!value.IsArray() || value.Size() != 2)
                    Fail(where + "." + key, "must be a [node, pin] array");
                nodeId = ReadUInt32(value[0], where + "." + key + "[0]");
                pin = ReadUInt32(value[1], where + "." + key + "[1]");
            };
            ScriptConnection conn;
            endpoint("from", conn.fromNode, conn.fromPin);
            endpoint("to", conn.toNode, conn.toPin);
            return conn;
        }

        /// Structural validation of one node/connection set (the event graph or a function body).
        void ReadGraphBody(const Json& object, const std::string& where, std::vector<ScriptNode>& nodes,
                           std::vector<ScriptConnection>& connections)
        {
            const Json& nodeArray = ArrayAt(object, "nodes", where, kMaxNodes);
            std::unordered_map<uint32_t, size_t> index;
            for (size_t i = 0; i < nodeArray.Size(); ++i)
            {
                const std::string at = where + ".nodes[" + std::to_string(i) + "]";
                ScriptNode node = ReadNode(nodeArray[i], at);
                if (!index.emplace(node.id, nodes.size()).second)
                    Fail(at + ".id", "duplicate node id " + std::to_string(node.id));
                nodes.push_back(std::move(node));
            }

            const Json& connectionArray = ArrayAt(object, "connections", where, kMaxConnections);
            std::set<std::pair<uint32_t, uint32_t>> wiredInputs;
            std::set<std::pair<uint32_t, uint32_t>> wiredExecOutputs;
            for (size_t i = 0; i < connectionArray.Size(); ++i)
            {
                const std::string at = where + ".connections[" + std::to_string(i) + "]";
                ScriptConnection conn = ReadConnection(connectionArray[i], at);

                const auto from = index.find(conn.fromNode);
                const auto to = index.find(conn.toNode);
                if (from == index.end())
                    Fail(at + ".from", "names missing node " + std::to_string(conn.fromNode));
                if (to == index.end())
                    Fail(at + ".to", "names missing node " + std::to_string(conn.toNode));
                if (conn.fromNode == conn.toNode)
                    Fail(at, "wires node " + std::to_string(conn.fromNode) + " to itself");

                const ScriptNode& source = nodes[from->second];
                const ScriptNode& target = nodes[to->second];
                if (conn.fromPin >= source.outputs.size())
                    Fail(at + ".from",
                         "node " + std::to_string(source.id) + " has no output pin " + std::to_string(conn.fromPin));
                if (conn.toPin >= target.inputs.size())
                    Fail(at + ".to",
                         "node " + std::to_string(target.id) + " has no input pin " + std::to_string(conn.toPin));

                const PinKind fromKind = source.outputs[conn.fromPin].kind;
                const PinKind toKind = target.inputs[conn.toPin].kind;
                if (!VisualScriptGraphIO::ArePinKindsCompatible(fromKind, toKind))
                {
                    Fail(at, std::string("cannot wire an output of kind ") +
                                 VisualScriptGraphIO::PinKindName(fromKind) + " to an input of kind " +
                                 VisualScriptGraphIO::PinKindName(toKind));
                }
                if (!wiredInputs.emplace(conn.toNode, conn.toPin).second)
                    Fail(at + ".to", "input pin " + std::to_string(conn.toPin) + " of node " +
                                         std::to_string(conn.toNode) + " already has a wire");
                if (fromKind == PinKind::Execution && !wiredExecOutputs.emplace(conn.fromNode, conn.fromPin).second)
                    Fail(at + ".from", "execution output " + std::to_string(conn.fromPin) + " of node " +
                                           std::to_string(conn.fromNode) + " already has a wire");
                connections.push_back(conn);
            }
        }

        std::vector<VariableDecl> ReadParameters(const Json& object, const std::string& where)
        {
            std::vector<VariableDecl> parameters;
            std::unordered_set<std::string> names;
            const Json& array = ArrayAt(object, "parameters", where, kMaxPinsPerNode);
            for (size_t i = 0; i < array.Size(); ++i)
            {
                const std::string at = where + ".parameters[" + std::to_string(i) + "]";
                RequireKeys(array[i], at, {"name", "type"}, {});
                VariableDecl parameter;
                parameter.name = ReadIdentifier(array[i]["name"], at + ".name");
                parameter.type = ReadDataKind(array[i]["type"], at + ".type", false);
                if (!names.insert(parameter.name).second)
                    Fail(at + ".name", "duplicate parameter '" + parameter.name + "'");
                parameters.push_back(std::move(parameter));
            }
            return parameters;
        }

        VisualScriptGraph ReadGraph(const Json& root)
        {
            const std::string where = "graph";
            RequireKeys(root, where, {"format", "version", "className", "nodes"},
                        {"description", "variables", "connections", "functions", "customEvents"});

            if (ReadString(root["format"], "format", kMaxNameBytes) != VisualScriptGraphIO::kFormat)
                Fail("format", std::string("must be \"") + std::string(VisualScriptGraphIO::kFormat) + "\"");
            if (!root["version"].IsNumber() || root["version"].AsNumber() != VisualScriptGraphIO::kVersion)
            {
                const std::string found = root["version"].IsNumber() ? std::format("{}", root["version"].AsNumber())
                                                                     : std::string("(not a number)");
                Fail("version", "unsupported .vscript version " + found + "; this build reads " +
                                    std::to_string(VisualScriptGraphIO::kVersion));
            }

            VisualScriptGraph graph;
            graph.className = ReadIdentifier(root["className"], "className");
            if (root.HasKey("description"))
                graph.description = ReadString(root["description"], "description", kMaxTextBytes);

            const Json& variables = ArrayAt(root, "variables", where, kMaxVariables);
            std::unordered_set<std::string> variableNames;
            for (size_t i = 0; i < variables.Size(); ++i)
            {
                const std::string at = "variables[" + std::to_string(i) + "]";
                RequireKeys(variables[i], at, {"name", "type"}, {"default"});
                VariableDecl var;
                var.name = ReadIdentifier(variables[i]["name"], at + ".name");
                var.type = ReadDataKind(variables[i]["type"], at + ".type", false);
                if (variables[i].HasKey("default"))
                    var.defaultValue = ReadString(variables[i]["default"], at + ".default", kMaxNameBytes);
                if (!variableNames.insert(var.name).second)
                    Fail(at + ".name", "duplicate variable '" + var.name + "'");
                graph.variables.push_back(std::move(var));
            }

            ReadGraphBody(root, "graph", graph.nodes, graph.connections);

            const Json& functions = ArrayAt(root, "functions", where, kMaxFunctions);
            for (size_t i = 0; i < functions.Size(); ++i)
            {
                const std::string at = "functions[" + std::to_string(i) + "]";
                RequireKeys(functions[i], at, {"name", "returnType"}, {"parameters", "nodes", "connections"});
                FunctionGraph function;
                function.name = ReadIdentifier(functions[i]["name"], at + ".name");
                function.returnType = ReadDataKind(functions[i]["returnType"], at + ".returnType", true);
                function.parameters = ReadParameters(functions[i], at);
                ReadGraphBody(functions[i], at, function.nodes, function.connections);
                graph.functions.push_back(std::move(function));
            }

            const Json& events = ArrayAt(root, "customEvents", where, kMaxFunctions);
            for (size_t i = 0; i < events.Size(); ++i)
            {
                const std::string at = "customEvents[" + std::to_string(i) + "]";
                RequireKeys(events[i], at, {"name"}, {"parameters"});
                CustomEventDef event;
                event.name = ReadIdentifier(events[i]["name"], at + ".name");
                event.parameters = ReadParameters(events[i], at);
                graph.customEvents.push_back(std::move(event));
            }
            return graph;
        }
    } // namespace

    std::expected<VisualScriptGraph, std::string> VisualScriptGraphIO::Parse(std::string_view text)
    {
        if (text.size() > kMaxFileBytes)
            return std::unexpected("graph is larger than " + std::to_string(kMaxFileBytes) + " bytes");

        Spark::Json::JsonLimits limits;
        limits.maxBytes = kMaxFileBytes;
        limits.maxDepth = 16;              // the format nests at most six levels deep
        limits.rejectDuplicateKeys = true; // a repeated key would otherwise hide all but its last value
        Json root;
        std::string error;
        if (!Spark::Json::ParseBounded(text, limits, &root, &error))
            return std::unexpected("graph is not valid JSON: " + error);

        try
        {
            return ReadGraph(root);
        }
        catch (const ParseError& failure)
        {
            return std::unexpected(failure.message);
        }
    }

    std::expected<VisualScriptGraph, std::string> VisualScriptGraphIO::LoadFile(const std::filesystem::path& path)
    {
        const std::string name = path.generic_string();
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error)
            return std::unexpected(name + ": cannot read (" + error.message() + ")");
        if (size > kMaxFileBytes)
            return std::unexpected(name + ": larger than " + std::to_string(kMaxFileBytes) + " bytes");

        std::ifstream stream(path, std::ios::binary);
        std::ostringstream text;
        text << stream.rdbuf();
        if (!stream)
            return std::unexpected(name + ": cannot read");

        auto graph = Parse(text.str());
        if (!graph)
            return std::unexpected(name + ": " + graph.error());
        return graph;
    }

} // namespace Spark::Scripting
