/**
 * @file TestVisualScriptSystem.cpp
 * @brief Unit tests for the Visual Scripting system
 */

#include "../SparkEngine/Source/Engine/Scripting/VisualScriptSystem.h"
#include <cassert>
#include <iostream>
#include <string>

using namespace Spark::Scripting;

// ============================================================================
// Test helpers
// ============================================================================

static int g_testsPassed = 0;
static int g_testsFailed = 0;

#define TEST_ASSERT(cond, msg)                                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::cerr << "FAIL: " << (msg) << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl;                 \
            g_testsFailed++;                                                                                           \
            return;                                                                                                    \
        }                                                                                                              \
    } while (false)

#define TEST_PASS(name)                                                                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        g_testsPassed++;                                                                                               \
        std::cout << "  PASS: " << (name) << std::endl;                                                                \
    } while (false)

// ============================================================================
// Tests
// ============================================================================

void TestNodeLibraryRegistration()
{
    auto& library = NodeLibrary::GetInstance();
    library.RegisterBuiltinNodes();

    // Verify core event nodes exist
    TEST_ASSERT(library.FindTemplate("Event_BeginPlay") != nullptr, "BeginPlay template should exist");
    TEST_ASSERT(library.FindTemplate("Event_Tick") != nullptr, "Tick template should exist");

    // Verify math nodes exist
    TEST_ASSERT(library.FindTemplate("Math_Add") != nullptr, "Math_Add template should exist");
    TEST_ASSERT(library.FindTemplate("Math_Multiply") != nullptr, "Math_Multiply template should exist");

    // Verify flow control
    TEST_ASSERT(library.FindTemplate("Flow_Branch") != nullptr, "Flow_Branch template should exist");

    // Verify categories
    auto eventNodes = library.GetTemplatesByCategory(NodeCategory::Event);
    TEST_ASSERT(!eventNodes.empty(), "Should have event node templates");

    auto mathNodes = library.GetTemplatesByCategory(NodeCategory::Math);
    TEST_ASSERT(mathNodes.size() >= 5, "Should have at least 5 math node templates");

    TEST_PASS("NodeLibraryRegistration");
}

void TestGraphNodeManagement()
{
    VisualScriptGraph graph("TestGraph");

    // Register builtins so AddNode can find templates
    NodeLibrary::GetInstance().RegisterBuiltinNodes();

    NodeID n1 = graph.AddNode("Event_BeginPlay", 100, 200);
    NodeID n2 = graph.AddNode("Debug_Print", 300, 200);

    TEST_ASSERT(n1 != INVALID_NODE_ID, "First node should have valid ID");
    TEST_ASSERT(n2 != INVALID_NODE_ID, "Second node should have valid ID");
    TEST_ASSERT(n1 != n2, "Node IDs should be unique");

    auto* node1 = graph.GetNode(n1);
    TEST_ASSERT(node1 != nullptr, "Should retrieve node by ID");
    TEST_ASSERT(node1->GetName() == "Event_BeginPlay", "Node name should match");
    TEST_ASSERT(node1->GetCategory() == NodeCategory::Event, "Node category should be Event");

    // Remove node
    bool removed = graph.RemoveNode(n1);
    TEST_ASSERT(removed, "Should remove existing node");
    TEST_ASSERT(graph.GetNode(n1) == nullptr, "Removed node should not be found");

    TEST_PASS("GraphNodeManagement");
}

void TestGraphLinkManagement()
{
    NodeLibrary::GetInstance().RegisterBuiltinNodes();

    VisualScriptGraph graph("LinkTest");

    NodeID beginPlay = graph.AddNode("Event_BeginPlay");
    NodeID print = graph.AddNode("Debug_Print");

    TEST_ASSERT(beginPlay != INVALID_NODE_ID, "BeginPlay node created");
    TEST_ASSERT(print != INVALID_NODE_ID, "Print node created");

    auto* bpNode = graph.GetNode(beginPlay);
    auto* prNode = graph.GetNode(print);
    TEST_ASSERT(bpNode != nullptr && prNode != nullptr, "Nodes should exist");

    // Find execution output pin on BeginPlay and execution input pin on Print
    PinID execOut = INVALID_PIN_ID;
    for (const auto& pin : bpNode->GetOutputPins())
    {
        if (pin.type == PinType::Execution)
        {
            execOut = pin.id;
            break;
        }
    }

    PinID execIn = INVALID_PIN_ID;
    for (const auto& pin : prNode->GetInputPins())
    {
        if (pin.type == PinType::Execution)
        {
            execIn = pin.id;
            break;
        }
    }

    TEST_ASSERT(execOut != INVALID_PIN_ID, "BeginPlay should have execution output");
    TEST_ASSERT(execIn != INVALID_PIN_ID, "Print should have execution input");

    LinkID link = graph.AddLink(beginPlay, execOut, print, execIn);
    TEST_ASSERT(link != INVALID_LINK_ID, "Should create link");
    TEST_ASSERT(graph.GetLinks().size() == 1, "Should have one link");

    // Remove link
    bool removed = graph.RemoveLink(link);
    TEST_ASSERT(removed, "Should remove link");
    TEST_ASSERT(graph.GetLinks().empty(), "Links should be empty after removal");

    TEST_PASS("GraphLinkManagement");
}

void TestGraphValidation()
{
    NodeLibrary::GetInstance().RegisterBuiltinNodes();

    VisualScriptGraph graph("ValidationTest");

    // Empty graph should validate (no errors)
    auto result = graph.Validate();
    TEST_ASSERT(result.isValid, "Empty graph should be valid");

    // Add a node and validate
    NodeID n1 = graph.AddNode("Event_BeginPlay");
    result = graph.Validate();
    TEST_ASSERT(result.isValid, "Graph with single event node should be valid");

    TEST_PASS("GraphValidation");
}

void TestGraphVariables()
{
    VisualScriptGraph graph("VarTest");

    graph.AddVariable("Health", PinType::Float, PinValue{100.0f}, true);
    graph.AddVariable("PlayerName", PinType::String, PinValue{std::string("Player1")}, false);

    TEST_ASSERT(graph.GetVariables().size() == 2, "Should have 2 variables");

    auto* health = graph.GetVariable("Health");
    TEST_ASSERT(health != nullptr, "Should find Health variable");
    TEST_ASSERT(health->type == PinType::Float, "Health should be float type");
    TEST_ASSERT(health->isPublic, "Health should be public");

    bool removed = graph.RemoveVariable("Health");
    TEST_ASSERT(removed, "Should remove Health variable");
    TEST_ASSERT(graph.GetVariables().size() == 1, "Should have 1 variable after removal");

    TEST_PASS("GraphVariables");
}

void TestCompileToAngelScript()
{
    NodeLibrary::GetInstance().RegisterBuiltinNodes();

    VisualScriptGraph graph("CompileTest");

    // Create a simple graph: BeginPlay -> PrintString
    NodeID beginPlay = graph.AddNode("Event_BeginPlay");
    NodeID print = graph.AddNode("Debug_Print");

    auto* bpNode = graph.GetNode(beginPlay);
    auto* prNode = graph.GetNode(print);

    // Connect execution flow
    PinID execOut = INVALID_PIN_ID;
    for (const auto& pin : bpNode->GetOutputPins())
    {
        if (pin.type == PinType::Execution)
        {
            execOut = pin.id;
            break;
        }
    }
    PinID execIn = INVALID_PIN_ID;
    for (const auto& pin : prNode->GetInputPins())
    {
        if (pin.type == PinType::Execution)
        {
            execIn = pin.id;
            break;
        }
    }

    graph.AddLink(beginPlay, execOut, print, execIn);

    // Add a variable
    graph.AddVariable("Score", PinType::Int, PinValue{int32_t(0)}, true);

    // Compile
    std::string asSource = graph.CompileToAngelScript();
    TEST_ASSERT(!asSource.empty(), "Compiled AngelScript should not be empty");
    TEST_ASSERT(asSource.find("class") != std::string::npos, "Should contain class declaration");
    TEST_ASSERT(asSource.find("BeginPlay") != std::string::npos || asSource.find("Start") != std::string::npos,
                "Should contain BeginPlay/Start method");

    TEST_PASS("CompileToAngelScript");
}

void TestVisualScriptSystemManager()
{
    auto& system = VisualScriptSystem::GetInstance();
    system.Initialize();

    auto* graph = system.CreateGraph("TestScript");
    TEST_ASSERT(graph != nullptr, "Should create graph");
    TEST_ASSERT(graph->GetName() == "TestScript", "Graph name should match");

    auto* found = system.GetGraph("TestScript");
    TEST_ASSERT(found == graph, "Should find graph by name");

    auto status = system.Console_GetStatus();
    TEST_ASSERT(!status.empty(), "Status should not be empty");

    bool removed = system.RemoveGraph("TestScript");
    TEST_ASSERT(removed, "Should remove graph");
    TEST_ASSERT(system.GetGraph("TestScript") == nullptr, "Removed graph should not be found");

    system.Shutdown();
    TEST_PASS("VisualScriptSystemManager");
}

void TestGraphSerialization()
{
    NodeLibrary::GetInstance().RegisterBuiltinNodes();

    VisualScriptGraph graph("SerializeTest");
    graph.AddNode("Event_BeginPlay", 100, 200);
    graph.AddNode("Debug_Print", 300, 200);
    graph.AddVariable("Speed", PinType::Float, PinValue{5.0f}, true);

    std::string json = graph.SerializeToJSON();
    TEST_ASSERT(!json.empty(), "JSON should not be empty");
    TEST_ASSERT(json.find("SerializeTest") != std::string::npos, "JSON should contain graph name");

    // Deserialize into a new graph
    VisualScriptGraph graph2;
    bool loaded = graph2.DeserializeFromJSON(json);
    TEST_ASSERT(loaded, "Should deserialize successfully");
    TEST_ASSERT(graph2.GetName() == "SerializeTest", "Deserialized name should match");

    TEST_PASS("GraphSerialization");
}

// ============================================================================
// Main
// ============================================================================

int main()
{
    std::cout << "=== Visual Script System Tests ===" << std::endl;

    TestNodeLibraryRegistration();
    TestGraphNodeManagement();
    TestGraphLinkManagement();
    TestGraphValidation();
    TestGraphVariables();
    TestCompileToAngelScript();
    TestVisualScriptSystemManager();
    TestGraphSerialization();

    std::cout << "\nResults: " << g_testsPassed << " passed, " << g_testsFailed << " failed" << std::endl;
    return g_testsFailed > 0 ? 1 : 0;
}
