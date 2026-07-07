// Test_gameplay_dialogue.cpp — regression test for DialogueSystem shared-state leak.
//
// SelectChoice used to write 'visited = true' onto the node inside the shared DialogueTree
// (registered once and reused for every conversation), leaking per-conversation state
// across separate playthroughs. The fix stops mutating the shared tree definition.

#include "../TestFramework.h"
#include "Engine/Dialogue/DialogueSystem.h"

#include <memory>

using namespace Spark;

TEST(Dialogue_SelectChoice_DoesNotMutateSharedTree)
{
    DialogueSystem sys;

    auto tree = std::make_unique<DialogueTree>();
    tree->SetStartNodeId("start");

    DialogueNode start;
    start.id = "start";
    start.type = DialogueNodeType::Choice;
    DialogueChoice c0;
    c0.text = "Yes";
    c0.nextNodeId = "end";
    DialogueChoice c1;
    c1.text = "No";
    c1.nextNodeId = "end";
    start.choices.push_back(c0);
    start.choices.push_back(c1);
    tree->AddNode(start);

    DialogueNode end;
    end.id = "end";
    end.type = DialogueNodeType::End;
    tree->AddNode(end);

    // Keep a non-owning pointer so we can inspect the shared definition afterwards.
    DialogueTree* treePtr = tree.get();
    sys.RegisterTree("t", std::move(tree));

    ASSERT_TRUE(sys.StartConversation("t"));
    ASSERT_TRUE(sys.SelectChoice(0));

    // The shared tree definition must be untouched — no 'visited' flag written back.
    DialogueNode* node = treePtr->GetMutableNode("start");
    ASSERT_TRUE(node != nullptr);
    for (const auto& ch : node->choices)
    {
        EXPECT_FALSE(ch.visited); // true before the fix
    }
}
