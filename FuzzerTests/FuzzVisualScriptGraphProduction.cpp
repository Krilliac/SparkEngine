/**
 * @file FuzzVisualScriptGraphProduction.cpp
 * @brief libc++-compiled production adapter for the .vscript graph libFuzzer harness.
 *
 * VisualScriptGraphIO::LoadFile reads a .vscript file and hands its text to
 * Parse, the fail-closed decoder whose result the visual script compiler turns
 * into AngelScript. The adapter calls Parse on the fuzz bytes and, for every
 * accepted graph, aborts so libFuzzer records a crash rather than a silent
 * pass, when:
 *  - Serialize(graph) does not parse again, or Serialize(Parse(Serialize(g)))
 *    differs from Serialize(g): the header promises canonical serialization,
 *    and SaveFile refuses to write a graph that would not load back,
 *  - a node id is 0 or above kMaxNodeId,
 *  - a body holds more nodes or connections, a node more pins or properties,
 *    or the graph more variables, functions or custom events than the
 *    decoder's documented caps, or a name or text value is longer than its cap.
 *
 * The caps are private to VisualScriptGraphIO.cpp, so they are restated here;
 * kMaxNodeId is public and checked against the header at compile time.
 */

#include "FuzzVisualScriptGraphProduction.h"

#include "Engine/Scripting/VisualScriptGraphIO.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using Spark::Scripting::VisualScriptGraph;
    using Spark::Scripting::VisualScriptGraphIO;

    constexpr std::size_t kMaxInputBytes = 64u * 1024u;

    // Mirrors of the decoder's caps (VisualScriptGraphIO.cpp).
    constexpr std::size_t kMaxNodes = 16384;
    constexpr std::size_t kMaxConnections = 65536;
    constexpr std::size_t kMaxPinsPerNode = 64;
    constexpr std::size_t kMaxProperties = 64;
    constexpr std::size_t kMaxNameBytes = 256;
    constexpr std::size_t kMaxTextBytes = 64u * 1024u;
    constexpr std::size_t kMaxVariables = 1024;
    constexpr std::size_t kMaxFunctions = 256;
    constexpr std::uint32_t kMaxNodeId = (1u << 24) - 1u;
    static_assert(kMaxNodeId == VisualScriptGraphIO::kMaxNodeId, "restated node-id cap drifted from the header");

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr,
                     "SparkFuzzVisualScriptGraph: VisualScriptGraphIO::Parse accepted a graph that violates: %s\n",
                     what);
        std::abort();
    }

    void RequireName(const std::string& name)
    {
        if (name.size() > kMaxNameBytes)
            InvariantFailure("a name longer than kMaxNameBytes");
    }

    void CheckParameters(const std::vector<Spark::Scripting::VariableDecl>& parameters)
    {
        if (parameters.size() > kMaxPinsPerNode)
            InvariantFailure("more parameters than kMaxPinsPerNode");
        for (const auto& parameter : parameters)
            RequireName(parameter.name);
    }

    void CheckBody(const std::vector<Spark::Scripting::ScriptNode>& nodes,
                   const std::vector<Spark::Scripting::ScriptConnection>& connections)
    {
        if (nodes.size() > kMaxNodes)
            InvariantFailure("a body with more nodes than kMaxNodes");
        if (connections.size() > kMaxConnections)
            InvariantFailure("a body with more connections than kMaxConnections");
        for (const auto& node : nodes)
        {
            if (node.id == 0 || node.id > kMaxNodeId)
                InvariantFailure("a node id outside [1, kMaxNodeId]");
            if (node.inputs.size() > kMaxPinsPerNode || node.outputs.size() > kMaxPinsPerNode)
                InvariantFailure("a node with more pins than kMaxPinsPerNode");
            if (node.properties.size() > kMaxProperties)
                InvariantFailure("a node with more properties than kMaxProperties");
            for (const auto& [key, value] : node.properties)
            {
                RequireName(key);
                if (value.size() > kMaxTextBytes)
                    InvariantFailure("a property value longer than kMaxTextBytes");
            }
            for (const auto& pin : node.inputs)
            {
                if (pin.defaultString.size() > kMaxTextBytes)
                    InvariantFailure("a pin default longer than kMaxTextBytes");
            }
        }
    }

    void CheckCaps(const VisualScriptGraph& graph)
    {
        RequireName(graph.className);
        if (graph.description.size() > kMaxTextBytes)
            InvariantFailure("a description longer than kMaxTextBytes");
        if (graph.variables.size() > kMaxVariables)
            InvariantFailure("more variables than kMaxVariables");
        for (const auto& variable : graph.variables)
        {
            RequireName(variable.name);
            RequireName(variable.defaultValue);
        }
        CheckBody(graph.nodes, graph.connections);
        if (graph.functions.size() > kMaxFunctions || graph.customEvents.size() > kMaxFunctions)
            InvariantFailure("more functions or custom events than kMaxFunctions");
        for (const auto& function : graph.functions)
        {
            RequireName(function.name);
            CheckParameters(function.parameters);
            CheckBody(function.nodes, function.connections);
        }
        for (const auto& event : graph.customEvents)
        {
            RequireName(event.name);
            CheckParameters(event.parameters);
        }
    }
} // namespace

// This C ABI is the only boundary between the libstdc++ libFuzzer executable
// and the libc++-compiled production decoder.
extern "C" int SparkFuzzParseVisualScriptGraph(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes)
        return 0;
    if (data == nullptr && size != 0)
        return 0;

    const std::string_view text(reinterpret_cast<const char*>(data), size);
    const auto graph = VisualScriptGraphIO::Parse(text);
    if (!graph)
        return 0;

    CheckCaps(*graph);

    const std::string canonical = VisualScriptGraphIO::Serialize(*graph);
    const auto reloaded = VisualScriptGraphIO::Parse(canonical);
    if (!reloaded)
        InvariantFailure("its canonical serialization does not parse again");
    if (VisualScriptGraphIO::Serialize(*reloaded) != canonical)
        InvariantFailure("its canonical serialization is not stable across a reload");
    return 0;
}
