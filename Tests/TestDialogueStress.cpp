// TestDialogueStress.cpp - Stress tests for the DialogueSystem
// Uses real DialogueSystem header for type-accurate testing

#include "TestFramework.h"
#include "Engine/Dialogue/DialogueSystem.h"
#include <string>
#include <vector>

// =============================================================================
// Helper: build a linear chain of N nodes (node_0 -> node_1 -> ... -> End)
// =============================================================================

static std::unique_ptr<Spark::DialogueTree> MakeLinearTree(int nodeCount, const std::string& startId = "node_0")
{
    auto tree = std::make_unique<Spark::DialogueTree>();
    tree->SetStartNodeId(startId);

    for (int i = 0; i < nodeCount; ++i)
    {
        Spark::DialogueNode node;
        node.id = "node_" + std::to_string(i);
        node.speakerName = "NPC";
        node.text = "Line " + std::to_string(i);

        if (i < nodeCount - 1)
        {
            node.type = Spark::DialogueNodeType::Text;
            node.nextNodeId = "node_" + std::to_string(i + 1);
        }
        else
        {
            node.type = Spark::DialogueNodeType::End;
        }

        tree->AddNode(node);
    }

    return tree;
}

// =============================================================================
// Tests
// =============================================================================

TEST(DialogueStress_EmptyTree)
{
    // Tree with zero nodes — StartConversation should fail gracefully
    Spark::DialogueSystem sys;

    auto tree = std::make_unique<Spark::DialogueTree>();
    tree->SetStartNodeId("start");
    // No nodes added

    sys.RegisterTree("empty", std::move(tree));

    bool started = sys.StartConversation("empty");
    // The tree exists but the start node does not, so conversation should
    // either fail to start or end immediately upon finding no valid node
    if (started)
    {
        // If it started, it should end on the first update since there is no node
        sys.Update(0.016f);
    }
    EXPECT_FALSE(sys.IsConversationActive());
}

TEST(DialogueStress_MissingStartNode)
{
    // Tree has nodes but the start node ID points to a nonexistent node
    Spark::DialogueSystem sys;

    auto tree = std::make_unique<Spark::DialogueTree>();
    tree->SetStartNodeId("nonexistent");

    Spark::DialogueNode node;
    node.id = "some_node";
    node.type = Spark::DialogueNodeType::Text;
    node.text = "You should never see this.";
    tree->AddNode(node);

    sys.RegisterTree("bad_start", std::move(tree));

    bool started = sys.StartConversation("bad_start");
    // The start node ID doesn't match any node, so conversation should
    // either not start or end immediately
    if (started)
    {
        sys.Update(0.016f);
    }
    EXPECT_FALSE(sys.IsConversationActive());
}

TEST(DialogueStress_CircularNodeLinks)
{
    // A -> B -> C -> A creates a cycle. Must not loop forever.
    // The system has kMaxProcessDepth guard to break cycles.
    Spark::DialogueSystem sys;

    auto tree = std::make_unique<Spark::DialogueTree>();
    tree->SetStartNodeId("a");

    Spark::DialogueNode a;
    a.id = "a";
    a.type = Spark::DialogueNodeType::Text;
    a.text = "Node A";
    a.nextNodeId = "b";
    tree->AddNode(a);

    Spark::DialogueNode b;
    b.id = "b";
    b.type = Spark::DialogueNodeType::Text;
    b.text = "Node B";
    b.nextNodeId = "c";
    tree->AddNode(b);

    Spark::DialogueNode c;
    c.id = "c";
    c.type = Spark::DialogueNodeType::Text;
    c.text = "Node C";
    c.nextNodeId = "a"; // Cycle back to A
    tree->AddNode(c);

    sys.RegisterTree("cycle", std::move(tree));
    sys.StartConversation("cycle");

    // Advance through the cycle with a bounded iteration cap
    for (int i = 0; i < 500; ++i)
    {
        sys.AdvanceNode();
        if (!sys.IsConversationActive())
        {
            break;
        }
    }

    // The test passes as long as we reach this line without hanging
    EXPECT_TRUE(true);
}

TEST(DialogueStress_MassiveNodeCount)
{
    // Tree with 5000 nodes in a linear chain — must not stack overflow
    constexpr int kNodeCount = 5000;

    Spark::DialogueSystem sys;
    sys.RegisterTree("massive", MakeLinearTree(kNodeCount));
    EXPECT_TRUE(sys.StartConversation("massive"));

    int nodesVisited = 0;
    while (sys.IsConversationActive())
    {
        sys.AdvanceNode();
        ++nodesVisited;

        // Safety valve
        if (nodesVisited > kNodeCount + 10)
        {
            break;
        }
    }

    EXPECT_FALSE(sys.IsConversationActive());
    EXPECT_GT(nodesVisited, 0);
}

TEST(DialogueStress_InvalidChoiceIndex)
{
    // Select choice 999 when only 2 exist — must return false, not crash
    Spark::DialogueSystem sys;

    auto tree = std::make_unique<Spark::DialogueTree>();
    tree->SetStartNodeId("choice_node");

    Spark::DialogueNode choiceNode;
    choiceNode.id = "choice_node";
    choiceNode.type = Spark::DialogueNodeType::Choice;
    choiceNode.text = "Pick one:";

    Spark::DialogueChoice c1;
    c1.text = "Option A";
    c1.nextNodeId = "end";
    choiceNode.choices.push_back(c1);

    Spark::DialogueChoice c2;
    c2.text = "Option B";
    c2.nextNodeId = "end";
    choiceNode.choices.push_back(c2);

    tree->AddNode(choiceNode);

    Spark::DialogueNode endNode;
    endNode.id = "end";
    endNode.type = Spark::DialogueNodeType::End;
    endNode.text = "Done.";
    tree->AddNode(endNode);

    sys.RegisterTree("choices", std::move(tree));
    sys.StartConversation("choices");

    // Out-of-bounds choice index
    bool result = sys.SelectChoice(999);
    EXPECT_FALSE(result);

    // Conversation should still be active (invalid choice was rejected)
    EXPECT_TRUE(sys.IsConversationActive());
}

TEST(DialogueStress_AdvancePastEnd)
{
    // Keep advancing after the conversation has already ended
    Spark::DialogueSystem sys;
    sys.RegisterTree("short", MakeLinearTree(2));
    sys.StartConversation("short");

    // Advance through both nodes (node_0 -> node_1 which is End)
    sys.AdvanceNode();
    EXPECT_FALSE(sys.IsConversationActive());

    // Advance 100 more times past the end — must not crash
    for (int i = 0; i < 100; ++i)
    {
        EXPECT_NO_THROW(sys.AdvanceNode());
    }

    EXPECT_FALSE(sys.IsConversationActive());
    EXPECT_TRUE(sys.GetCurrentNode() == nullptr);
}

TEST(DialogueStress_MultipleSimultaneousConversations)
{
    // Register 100 different trees and start conversations sequentially.
    // Each start should cleanly replace the previous conversation.
    Spark::DialogueSystem sys;

    for (int i = 0; i < 100; ++i)
    {
        std::string treeId = "tree_" + std::to_string(i);
        sys.RegisterTree(treeId, MakeLinearTree(3, "node_0"));
    }

    for (int i = 0; i < 100; ++i)
    {
        std::string treeId = "tree_" + std::to_string(i);
        EXPECT_TRUE(sys.StartConversation(treeId));
        EXPECT_TRUE(sys.IsConversationActive());

        const auto* node = sys.GetCurrentNode();
        EXPECT_TRUE(node != nullptr);
    }

    sys.EndConversation();
    EXPECT_FALSE(sys.IsConversationActive());
}

TEST(DialogueStress_EmptyNodeText)
{
    // Nodes with empty speaker and text strings — must not crash
    Spark::DialogueSystem sys;

    auto tree = std::make_unique<Spark::DialogueTree>();
    tree->SetStartNodeId("blank");

    Spark::DialogueNode blank;
    blank.id = "blank";
    blank.type = Spark::DialogueNodeType::Text;
    blank.speakerName = "";
    blank.text = "";
    blank.nextNodeId = "end";
    tree->AddNode(blank);

    Spark::DialogueNode endNode;
    endNode.id = "end";
    endNode.type = Spark::DialogueNodeType::End;
    endNode.speakerName = "";
    endNode.text = "";
    tree->AddNode(endNode);

    sys.RegisterTree("blanks", std::move(tree));
    EXPECT_TRUE(sys.StartConversation("blanks"));

    const auto* node = sys.GetCurrentNode();
    ASSERT_TRUE(node != nullptr);
    EXPECT_EQ(node->text, std::string(""));
    EXPECT_EQ(node->speakerName, std::string(""));

    sys.AdvanceNode();
    EXPECT_FALSE(sys.IsConversationActive());
}

TEST(DialogueStress_StartConversationWhileActive)
{
    // Start a new conversation while one is already running
    Spark::DialogueSystem sys;

    sys.RegisterTree("first", MakeLinearTree(5, "node_0"));
    sys.RegisterTree("second", MakeLinearTree(3, "node_0"));

    EXPECT_TRUE(sys.StartConversation("first"));
    EXPECT_TRUE(sys.IsConversationActive());

    // Advance partway through the first conversation
    sys.AdvanceNode();

    // Start a different conversation mid-way through
    EXPECT_TRUE(sys.StartConversation("second"));
    EXPECT_TRUE(sys.IsConversationActive());

    // Current node should be from the second tree
    const auto* node = sys.GetCurrentNode();
    ASSERT_TRUE(node != nullptr);
    EXPECT_EQ(node->id, std::string("node_0"));

    // Walk to end of second conversation
    while (sys.IsConversationActive())
    {
        sys.AdvanceNode();
    }
    EXPECT_FALSE(sys.IsConversationActive());
}

TEST(DialogueStress_RegisterDuplicateTree)
{
    // Register the same tree ID twice — the second should overwrite the first
    Spark::DialogueSystem sys;

    auto tree1 = std::make_unique<Spark::DialogueTree>();
    tree1->SetStartNodeId("start");
    Spark::DialogueNode n1;
    n1.id = "start";
    n1.type = Spark::DialogueNodeType::Text;
    n1.text = "First version";
    tree1->AddNode(n1);
    sys.RegisterTree("dup", std::move(tree1));

    // Overwrite with a different tree
    auto tree2 = std::make_unique<Spark::DialogueTree>();
    tree2->SetStartNodeId("start");
    Spark::DialogueNode n2;
    n2.id = "start";
    n2.type = Spark::DialogueNodeType::End;
    n2.text = "Second version";
    tree2->AddNode(n2);
    sys.RegisterTree("dup", std::move(tree2));

    // Start conversation — should use the second version
    // The second tree has an End node as start, so the conversation may
    // immediately end. Either way, the overwrite worked if no crash occurs.
    sys.StartConversation("dup");
    const auto* node = sys.GetCurrentNode();
    if (node != nullptr)
    {
        EXPECT_EQ(node->text, std::string("Second version"));
    }
    else
    {
        // End node causes immediate conversation end — this is expected behavior
        EXPECT_TRUE(true);
    }
}
