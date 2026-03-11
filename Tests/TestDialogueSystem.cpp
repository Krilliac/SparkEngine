/**
 * @file TestDialogueSystem.cpp
 * @brief Tests for Spark::DialogueSystem
 */

#include "TestFramework.h"
#include "../SparkEngine/Source/Engine/Dialogue/DialogueSystem.h"

TEST(Dialogue_CreateTree)
{
    Spark::DialogueTree tree;
    tree.SetId("test_tree");
    tree.SetStartNodeId("start");

    Spark::DialogueNode startNode;
    startNode.id = "start";
    startNode.type = Spark::DialogueNodeType::Text;
    startNode.speakerName = "Guard";
    startNode.text = "Halt! Who goes there?";
    startNode.nextNodeId = "response";
    tree.AddNode(startNode);

    Spark::DialogueNode responseNode;
    responseNode.id = "response";
    responseNode.type = Spark::DialogueNodeType::End;
    responseNode.text = "Very well, move along.";
    tree.AddNode(responseNode);

    EXPECT_EQ(tree.GetNodeCount(), static_cast<size_t>(2));
    EXPECT_EQ(tree.GetStartNodeId(), std::string("start"));

    const auto* node = tree.GetNode("start");
    EXPECT_TRUE(node != nullptr);
    EXPECT_EQ(node->speakerName, std::string("Guard"));
}

TEST(Dialogue_StartConversation)
{
    Spark::DialogueSystem sys;

    auto tree = std::make_unique<Spark::DialogueTree>();
    tree->SetStartNodeId("node1");

    Spark::DialogueNode node1;
    node1.id = "node1";
    node1.type = Spark::DialogueNodeType::Text;
    node1.text = "Hello!";
    node1.nextNodeId = "node2";
    tree->AddNode(node1);

    Spark::DialogueNode node2;
    node2.id = "node2";
    node2.type = Spark::DialogueNodeType::End;
    node2.text = "Goodbye!";
    tree->AddNode(node2);

    sys.RegisterTree("test", std::move(tree));
    EXPECT_TRUE(sys.StartConversation("test"));
    EXPECT_TRUE(sys.IsConversationActive());

    const auto* current = sys.GetCurrentNode();
    EXPECT_TRUE(current != nullptr);
    EXPECT_EQ(current->text, std::string("Hello!"));
}

TEST(Dialogue_AdvanceAndEnd)
{
    Spark::DialogueSystem sys;

    auto tree = std::make_unique<Spark::DialogueTree>();
    tree->SetStartNodeId("a");

    Spark::DialogueNode a;
    a.id = "a";
    a.type = Spark::DialogueNodeType::Text;
    a.text = "A";
    a.nextNodeId = "b";
    tree->AddNode(a);

    Spark::DialogueNode b;
    b.id = "b";
    b.type = Spark::DialogueNodeType::End;
    b.text = "B";
    tree->AddNode(b);

    sys.RegisterTree("flow", std::move(tree));
    sys.StartConversation("flow");
    sys.AdvanceNode();

    // Should be at End node, which auto-ends
    EXPECT_FALSE(sys.IsConversationActive());
}

TEST(Dialogue_Variables)
{
    Spark::DialogueSystem sys;
    sys.SetVariable("hasKey", "true");
    EXPECT_EQ(sys.GetVariable("hasKey"), std::string("true"));
    EXPECT_EQ(sys.GetVariable("missing"), std::string(""));
}
