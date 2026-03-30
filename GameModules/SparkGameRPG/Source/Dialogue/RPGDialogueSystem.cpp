/**
 * @file RPGDialogueSystem.cpp
 * @brief Branching dialogue, skill checks, and reward distribution
 */

#include "RPGDialogueSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

namespace RPG
{

    bool RPGDialogueSystem::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;
        RegisterDefaultTrees();

        Spark::SimpleConsole::GetInstance().LogInfo("[RPG] Dialogue system initialized (" +
                                                    std::to_string(m_trees.size()) + " dialogue trees)");
        return true;
    }

    void RPGDialogueSystem::Update(float deltaTime)
    {
        (void)deltaTime;
        // Dialogue is event-driven; no per-frame work needed
    }

    void RPGDialogueSystem::Shutdown()
    {
        EndDialogue();
        m_trees.clear();
    }

    void RPGDialogueSystem::RegisterDefaultTrees()
    {
        // --- Blacksmith dialogue (village) ---
        {
            DialogueTree tree;
            tree.treeId = 1;
            tree.name = "Blacksmith Greeting";
            tree.rootNodeId = 100;

            DialogueNode greeting;
            greeting.nodeId = 100;
            greeting.type = DialogueNodeType::Choice;
            greeting.speakerName = "Grimjaw the Blacksmith";
            greeting.text = "Welcome, traveler. I forge the finest blades in the realm. What can I do for you?";
            greeting.choices = {
                {"I need a weapon forged.", 101},
                {"Tell me about this village.", 102},
                {"[Strength Check] Arm wrestle for a discount?", 103},
                {"Nothing, farewell.", 199},
            };
            tree.nodes[100] = greeting;

            DialogueNode forge;
            forge.nodeId = 101;
            forge.type = DialogueNodeType::Text;
            forge.speakerName = "Grimjaw the Blacksmith";
            forge.text = "Bring me iron ore and I'll make you something worthy of a hero.";
            forge.nextNodeId = 199;
            tree.nodes[101] = forge;

            DialogueNode village;
            village.nodeId = 102;
            village.type = DialogueNodeType::Text;
            village.speakerName = "Grimjaw the Blacksmith";
            village.text =
                "Oakhollow has stood for centuries. The forest protects us, but lately... dark things stir within.";
            village.nextNodeId = 199;
            tree.nodes[102] = village;

            DialogueNode armCheck;
            armCheck.nodeId = 103;
            armCheck.type = DialogueNodeType::Check;
            armCheck.speakerName = "Grimjaw the Blacksmith";
            armCheck.text = "Ha! Think you're strong enough? Let's see what you've got!";
            armCheck.skillCheck = {"strength", 12.0f, 104, 105};
            tree.nodes[103] = armCheck;

            DialogueNode passNode;
            passNode.nodeId = 104;
            passNode.type = DialogueNodeType::Reward;
            passNode.speakerName = "Grimjaw the Blacksmith";
            passNode.text = "By the forge! You beat me fair and square. Here, take this on the house.";
            passNode.reward = {25, 0, 0, 5}; // 25 XP, +5 disposition
            passNode.nextNodeId = 199;
            tree.nodes[104] = passNode;

            DialogueNode failNode;
            failNode.nodeId = 105;
            failNode.type = DialogueNodeType::Text;
            failNode.speakerName = "Grimjaw the Blacksmith";
            failNode.text = "Nice try, but you'll need more muscle than that! Come back when you've been training.";
            failNode.nextNodeId = 199;
            tree.nodes[105] = failNode;

            DialogueNode endNode;
            endNode.nodeId = 199;
            endNode.type = DialogueNodeType::End;
            endNode.text = "";
            tree.nodes[199] = endNode;

            RegisterTree(tree);
        }

        // --- Elder dialogue (quest giver) ---
        {
            DialogueTree tree;
            tree.treeId = 2;
            tree.name = "Village Elder";
            tree.rootNodeId = 200;

            DialogueNode greeting;
            greeting.nodeId = 200;
            greeting.type = DialogueNodeType::Choice;
            greeting.speakerName = "Elder Mirwen";
            greeting.text =
                "Ah, an adventurer. Our village is troubled. Dark creatures emerge from the Thornwood at night.";
            greeting.choices = {
                {"I'll help. What needs to be done?", 201},
                {"[Wisdom Check] I sense there's more to this story.", 202},
                {"I have my own problems.", 299},
            };
            tree.nodes[200] = greeting;

            DialogueNode quest;
            quest.nodeId = 201;
            quest.type = DialogueNodeType::Reward;
            quest.speakerName = "Elder Mirwen";
            quest.text = "Brave soul! Clear the shadow wolves from Thornwood. Return and I shall reward you.";
            quest.reward = {10, 0, 0, 3};
            quest.nextNodeId = 299;
            tree.nodes[201] = quest;

            DialogueNode wisdomCheck;
            wisdomCheck.nodeId = 202;
            wisdomCheck.type = DialogueNodeType::Check;
            wisdomCheck.speakerName = "Elder Mirwen";
            wisdomCheck.text = "Your insight serves you well...";
            wisdomCheck.skillCheck = {"wisdom", 14.0f, 203, 201};
            tree.nodes[202] = wisdomCheck;

            DialogueNode secretPath;
            secretPath.nodeId = 203;
            secretPath.type = DialogueNodeType::Reward;
            secretPath.speakerName = "Elder Mirwen";
            secretPath.text = "You see clearly. A necromancer hides in the old crypt beneath Thornwood. Defeat it, and "
                              "the wolves will scatter. "
                              "Take this amulet of warding.";
            secretPath.reward = {50, 0, 0, 10};
            secretPath.nextNodeId = 299;
            tree.nodes[203] = secretPath;

            DialogueNode endNode;
            endNode.nodeId = 299;
            endNode.type = DialogueNodeType::End;
            endNode.text = "";
            tree.nodes[299] = endNode;

            RegisterTree(tree);
        }
    }

    // === Tree management ===

    void RPGDialogueSystem::RegisterTree(const DialogueTree& tree)
    {
        m_trees[tree.treeId] = tree;
    }

    const DialogueTree* RPGDialogueSystem::GetTree(uint32_t treeId) const
    {
        auto it = m_trees.find(treeId);
        return it != m_trees.end() ? &it->second : nullptr;
    }

    // === Session management ===

    bool RPGDialogueSystem::StartDialogue(uint32_t treeId, uint32_t characterId, uint32_t npcId)
    {
        if (m_session.isActive)
            return false;

        const auto* tree = GetTree(treeId);
        if (!tree)
            return false;

        m_session.treeId = treeId;
        m_session.currentNodeId = tree->rootNodeId;
        m_session.characterId = characterId;
        m_session.npcId = npcId;
        m_session.isActive = true;

        SPARK_LOG_INFO(Spark::LogCategory::Game, "RPG dialogue started: %s (tree=%u, npc=%u)", tree->name.c_str(),
                       treeId, npcId);
        Spark::SimpleConsole::GetInstance().LogInfo("[RPG] Dialogue started: " + tree->name);
        return true;
    }

    void RPGDialogueSystem::AdvanceDialogue(int choiceIndex)
    {
        if (!m_session.isActive)
            return;

        const auto* node = GetCurrentNode();
        if (!node)
        {
            EndDialogue();
            return;
        }

        switch (node->type)
        {
        case DialogueNodeType::Text:
            m_session.currentNodeId = node->nextNodeId;
            break;

        case DialogueNodeType::Choice:
            if (choiceIndex >= 0 && choiceIndex < static_cast<int>(node->choices.size()))
            {
                m_session.currentNodeId = node->choices[choiceIndex].nextNodeId;
                SPARK_LOG_DEBUG(Spark::LogCategory::Game, "RPG dialogue choice %d selected -> node %u", choiceIndex,
                                node->choices[choiceIndex].nextNodeId);
            }
            break;

        case DialogueNodeType::Check:
        {
            float statValue = 0.0f;
            if (m_statLookup)
                statValue = m_statLookup(m_session.characterId, node->skillCheck.statName);

            m_lastCheckPassed = (statValue >= node->skillCheck.requiredValue);
            m_session.currentNodeId = m_lastCheckPassed ? node->skillCheck.passNodeId : node->skillCheck.failNodeId;

            auto& console = Spark::SimpleConsole::GetInstance();
            console.LogInfo("[RPG] Skill check (" + node->skillCheck.statName +
                            " >= " + std::to_string(static_cast<int>(node->skillCheck.requiredValue)) +
                            "): " + (m_lastCheckPassed ? "PASSED" : "FAILED"));
            break;
        }

        case DialogueNodeType::Reward:
            m_lastReward = node->reward;
            m_session.currentNodeId = node->nextNodeId;
            Spark::SimpleConsole::GetInstance().LogInfo("[RPG] Dialogue reward: +" + std::to_string(node->reward.xp) +
                                                        " XP");
            break;

        case DialogueNodeType::End:
            EndDialogue();
            return;

        default:
            EndDialogue();
            return;
        }

        // Check if the new node is an End node
        const auto* nextNode = GetCurrentNode();
        if (nextNode && nextNode->type == DialogueNodeType::End)
        {
            EndDialogue();
        }
    }

    void RPGDialogueSystem::EndDialogue()
    {
        if (m_session.isActive)
        {
            SPARK_LOG_DEBUG(Spark::LogCategory::Game, "RPG dialogue session ended");
            Spark::SimpleConsole::GetInstance().LogInfo("[RPG] Dialogue ended");
        }
        m_session = {};
    }

    const DialogueNode* RPGDialogueSystem::GetCurrentNode() const
    {
        if (!m_session.isActive)
            return nullptr;

        const auto* tree = GetTree(m_session.treeId);
        if (!tree)
            return nullptr;

        auto it = tree->nodes.find(m_session.currentNodeId);
        return it != tree->nodes.end() ? &it->second : nullptr;
    }

    void RPGDialogueSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (ImGui::TreeNode("RPG Dialogue System"))
        {
            ImGui::Text("Dialogue Trees: %zu", m_trees.size());
            ImGui::Text("In dialogue: %s", m_session.isActive ? "Yes" : "No");

            if (m_session.isActive)
            {
                const auto* tree = GetTree(m_session.treeId);
                const auto* node = GetCurrentNode();
                if (tree)
                    ImGui::Text("Tree: %s", tree->name.c_str());
                if (node)
                {
                    ImGui::Text("Node %u: %s", node->nodeId, node->speakerName.c_str());
                    ImGui::TextWrapped("%s", node->text.c_str());
                }
            }

            if (ImGui::TreeNode("Trees"))
            {
                for (const auto& [id, tree] : m_trees)
                {
                    ImGui::Text("[%u] %s (%zu nodes)", id, tree.name.c_str(), tree.nodes.size());
                }
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }
#endif
    }

} // namespace RPG
