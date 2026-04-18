/**
 * @file TestDialogueSystem.cpp
 * @brief Tests for Spark::DialogueSystem
 */

#include "TestFramework.h"
#include "Engine/Dialogue/DialogueSystem.h"

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

// ============================================================================
// Branching Choice
// ============================================================================

TEST(Dialogue_BranchingChoice)
{
    Spark::DialogueSystem sys;

    auto tree = std::make_unique<Spark::DialogueTree>();
    tree->SetStartNodeId("question");

    Spark::DialogueNode question;
    question.id = "question";
    question.type = Spark::DialogueNodeType::Choice;
    question.text = "Where do you want to go?";

    Spark::DialogueChoice choice0;
    choice0.text = "The forest";
    choice0.nextNodeId = "forest";
    question.choices.push_back(choice0);

    Spark::DialogueChoice choice1;
    choice1.text = "The cave";
    choice1.nextNodeId = "cave";
    question.choices.push_back(choice1);

    Spark::DialogueChoice choice2;
    choice2.text = "The village";
    choice2.nextNodeId = "village";
    question.choices.push_back(choice2);

    tree->AddNode(question);

    Spark::DialogueNode forest;
    forest.id = "forest";
    forest.type = Spark::DialogueNodeType::End;
    forest.text = "You enter the forest.";
    tree->AddNode(forest);

    Spark::DialogueNode cave;
    cave.id = "cave";
    cave.type = Spark::DialogueNodeType::End;
    cave.text = "You enter the cave.";
    tree->AddNode(cave);

    Spark::DialogueNode village;
    village.id = "village";
    village.type = Spark::DialogueNodeType::End;
    village.text = "You enter the village.";
    tree->AddNode(village);

    sys.RegisterTree("branching", std::move(tree));
    sys.StartConversation("branching");

    // Select choice 2 (the village)
    sys.SelectChoice(2);

    // Conversation should have ended (End node auto-ends)
    EXPECT_FALSE(sys.IsConversationActive());
}

// ============================================================================
// Conditional Branch
// ============================================================================

TEST(Dialogue_ConditionalBranch)
{
    Spark::DialogueSystem sys;

    // Register a condition evaluator that checks variables
    sys.RegisterCondition("hasItem",
                          [&](const std::string& param) -> bool { return sys.GetVariable(param) == "true"; });

    auto tree = std::make_unique<Spark::DialogueTree>();
    tree->SetStartNodeId("check");

    Spark::DialogueNode check;
    check.id = "check";
    check.type = Spark::DialogueNodeType::Branch;
    check.condition = "hasItem:key";
    check.trueNodeId = "has_key";
    check.falseNodeId = "no_key";
    tree->AddNode(check);

    Spark::DialogueNode hasKey;
    hasKey.id = "has_key";
    hasKey.type = Spark::DialogueNodeType::End;
    hasKey.text = "You unlock the door.";
    tree->AddNode(hasKey);

    Spark::DialogueNode noKey;
    noKey.id = "no_key";
    noKey.type = Spark::DialogueNodeType::End;
    noKey.text = "The door is locked.";
    tree->AddNode(noKey);

    sys.RegisterTree("branch_test", std::move(tree));

    // Without the key — should take false path
    sys.StartConversation("branch_test");
    // Branch node auto-processes, conversation ends at End node
    EXPECT_FALSE(sys.IsConversationActive());
}

// ============================================================================
// Callback on Dialogue Event (Node Enter)
// ============================================================================

TEST(Dialogue_CallbackOnNodeEnter)
{
    Spark::DialogueSystem sys;

    bool callbackFired = false;
    std::string receivedEvent;
    sys.OnDialogueEvent(
        [&](const std::string& eventName, const std::string& /*eventData*/)
        {
            callbackFired = true;
            receivedEvent = eventName;
        });

    auto tree = std::make_unique<Spark::DialogueTree>();
    tree->SetStartNodeId("event_node");

    Spark::DialogueNode eventNode;
    eventNode.id = "event_node";
    eventNode.type = Spark::DialogueNodeType::Event;
    eventNode.eventName = "door_opened";
    eventNode.eventData = "front_door";
    eventNode.nextNodeId = "end_node";
    tree->AddNode(eventNode);

    Spark::DialogueNode endNode;
    endNode.id = "end_node";
    endNode.type = Spark::DialogueNodeType::End;
    endNode.text = "Done.";
    tree->AddNode(endNode);

    sys.RegisterTree("event_test", std::move(tree));
    sys.StartConversation("event_test");

    // Event node should fire callback during processing
    EXPECT_TRUE(callbackFired);
    EXPECT_EQ(receivedEvent, std::string("door_opened"));
}

// ============================================================================
// Callback on Conversation End (Node Exit)
// ============================================================================

TEST(Dialogue_CallbackOnNodeExit)
{
    Spark::DialogueSystem sys;

    bool endCallbackFired = false;
    std::string endedTreeId;
    sys.OnConversationEnded(
        [&](const std::string& treeId)
        {
            endCallbackFired = true;
            endedTreeId = treeId;
        });

    auto tree = std::make_unique<Spark::DialogueTree>();
    tree->SetStartNodeId("only");

    Spark::DialogueNode only;
    only.id = "only";
    only.type = Spark::DialogueNodeType::End;
    only.text = "Goodbye.";
    tree->AddNode(only);

    sys.RegisterTree("exit_test", std::move(tree));
    sys.StartConversation("exit_test");

    // End node should trigger conversation-ended callback
    EXPECT_TRUE(endCallbackFired);
    EXPECT_EQ(endedTreeId, std::string("exit_test"));
    EXPECT_FALSE(sys.IsConversationActive());
}
