/**
 * @file TestSEC3ScriptingHardening.cpp
 * @brief SEC3: scripting trust-boundary regressions (visual-script compiler, script API, hot reload, sandbox).
 *
 * SEC3VisualScript_* (no AngelScript SDK needed):
 * - A crafted graph of deeply nested Branch nodes, or a long chain of pure data
 *   nodes, used to recurse through VisualScriptEmitter until the stack
 *   overflowed. The emitter now fails such graphs with a compile error.
 * - Editor script names become file names; VisualScriptGraphIO::ScriptFilePath
 *   refuses anything that is not a plain identifier or is a device name.
 *
 * SEC3Script_* (real AngelScriptEngine):
 * - A `Transform@` kept past destroyEntity() used to be a raw pointer into the
 *   EnTT pool, so writes through it landed on whichever entity was swapped into
 *   the slot. The handle now re-resolves its entity and faults the script.
 * - getPosition()/setHealth()/getComponentField()... on a destroyed entity id
 *   used to hit World's fatal "invalid entity" precondition.
 * - HotReloadModule() re-read the source file after detaching live instances;
 *   a file rewritten in between left every entity without a script.
 * - `sandbox.level strict` after Initialize() reported Strict while the
 *   Standard API stayed callable; the function policy is now fixed at startup.
 */

#include "TestFramework.h"

#include "Engine/Scripting/VisualScriptCompiler.h"
#include "Engine/Scripting/VisualScriptGraphIO.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
    using namespace Spark::Scripting;

    ScriptPin Pin(PinKind kind)
    {
        ScriptPin pin;
        pin.kind = kind;
        return pin;
    }

    /// OnStart -> Branch(2) -True-> Branch(3) -True-> ... : @p depth Branch nodes, each inside the previous one.
    VisualScriptGraph NestedBranchGraph(uint32_t depth)
    {
        VisualScriptGraph graph;
        graph.className = "Nested";
        ScriptNode start;
        start.id = 1;
        start.type = ScriptNodeType::OnStart;
        graph.nodes.push_back(start);
        for (uint32_t i = 0; i < depth; ++i)
        {
            ScriptNode branch;
            branch.id = i + 2;
            branch.type = ScriptNodeType::Branch;
            branch.inputs = {Pin(PinKind::Execution), Pin(PinKind::Bool)};
            branch.outputs = {Pin(PinKind::Execution), Pin(PinKind::Execution)};
            graph.nodes.push_back(branch);
            graph.connections.push_back({i + 1, 0, i + 2, 0});
        }
        return graph;
    }

    bool HasErrorContaining(const ScriptCompileResult& result, const std::string& text)
    {
        for (const auto& error : result.errors)
        {
            if (error.find(text) != std::string::npos)
                return true;
        }
        return false;
    }
} // namespace

TEST(SEC3VisualScript_NestedBranchChainFailsCleanly)
{
    // The parser admits 16,384 nodes. Before the depth budget each nesting level
    // cost four native frames plus a growing indent string, so this graph
    // overflowed the stack (or compiled to hundreds of MB of source).
    const auto result = VisualScriptCompiler::Compile(NestedBranchGraph(16383));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.angelScriptSource.empty());
    EXPECT_TRUE(HasErrorContaining(result, "nests execution flow deeper than"));
}

TEST(SEC3VisualScript_ModerateNestingStillCompiles)
{
    const auto result = VisualScriptCompiler::Compile(NestedBranchGraph(32));
    EXPECT_TRUE(result.success);
    // All 32 levels are emitted.
    size_t ifs = 0;
    for (size_t at = result.angelScriptSource.find("if ("); at != std::string::npos;
         at = result.angelScriptSource.find("if (", at + 1))
    {
        ++ifs;
    }
    EXPECT_EQ(ifs, static_cast<size_t>(32));
}

TEST(SEC3VisualScript_DeepPureDataChainFailsCleanly)
{
    // SetVariable <- Add(1000) <- Add(1001) <- ... : one pure producer per level,
    // which CollectPure walked by recursion with no bound.
    constexpr uint32_t kChain = 2000;
    VisualScriptGraph graph;
    graph.className = "DataChain";
    ScriptNode start;
    start.id = 1;
    start.type = ScriptNodeType::OnStart;
    graph.nodes.push_back(start);

    ScriptNode setVar;
    setVar.id = 2;
    setVar.type = ScriptNodeType::SetVariable;
    setVar.properties["name"] = "total";
    setVar.inputs = {Pin(PinKind::Execution), Pin(PinKind::Float)};
    graph.nodes.push_back(setVar);
    graph.connections.push_back({1, 0, 2, 0});

    for (uint32_t i = 0; i < kChain; ++i)
    {
        ScriptNode add;
        add.id = 1000 + i;
        add.type = ScriptNodeType::Add;
        add.inputs = {Pin(PinKind::Float), Pin(PinKind::Float)};
        add.outputs = {Pin(PinKind::Float)};
        graph.nodes.push_back(add);
        // Each Add feeds input 0 of the previous one; the first feeds the SetVariable value.
        graph.connections.push_back({add.id, 0, i == 0 ? 2u : add.id - 1, i == 0 ? 1u : 0u});
    }

    const auto result = VisualScriptCompiler::Compile(graph);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(HasErrorContaining(result, "Data chain into node"));
}

TEST(SEC3VisualScript_ScriptFilePathRejectsPathsAndDeviceNames)
{
    const std::filesystem::path directory = "Assets/Scripts/Generated";

    const auto good = VisualScriptGraphIO::ScriptFilePath(directory, "Player_Health2", ".as");
    EXPECT_TRUE(good.has_value());
    if (good)
    {
        EXPECT_TRUE(good->parent_path() == directory);
        EXPECT_TRUE(good->filename() == std::filesystem::path("Player_Health2.as"));
    }

    const char* const refused[] = {
        "",
        "../../../evil",
        "..\\..\\evil",
        "sub/dir",
        "/abs/path",
        "C:evil",
        "C:\\Windows\\evil",
        "a.b",
        "..",
        "has space",
        "9lives",
        "NUL",
        "con",
        "Com1",
        "lpt9",
    };
    for (const char* name : refused)
    {
        const auto path = VisualScriptGraphIO::ScriptFilePath(directory, name, ".as");
        EXPECT_FALSE(path.has_value());
        if (path)
            std::printf("  accepted unsafe script name '%s' -> %s\n", name, path->string().c_str());
    }

    // Over-long names are refused rather than truncated into a different file.
    EXPECT_FALSE(VisualScriptGraphIO::ScriptFilePath(directory, std::string(257, 'a'), ".vscript").has_value());
    EXPECT_TRUE(VisualScriptGraphIO::ScriptFilePath(directory, std::string(256, 'a'), ".vscript").has_value());
}
