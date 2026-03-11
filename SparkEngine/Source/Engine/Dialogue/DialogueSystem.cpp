/**
 * @file DialogueSystem.cpp
 * @brief Implementation of the dialogue tree and conversation system
 */

#include "DialogueSystem.h"

#include <fstream>
#include <sstream>
#include <regex>

namespace Spark
{

    // =============================================================================
    // DialogueTree
    // =============================================================================

    void DialogueTree::AddNode(const DialogueNode& node)
    {
        m_nodes[node.id] = node;
    }

    const DialogueNode* DialogueTree::GetNode(const std::string& nodeId) const
    {
        auto it = m_nodes.find(nodeId);
        return it != m_nodes.end() ? &it->second : nullptr;
    }

    std::vector<std::string> DialogueTree::GetNodeIds() const
    {
        std::vector<std::string> ids;
        ids.reserve(m_nodes.size());
        for (const auto& [id, node] : m_nodes)
        {
            ids.push_back(id);
        }
        return ids;
    }

    bool DialogueTree::LoadFromFile(const std::string& filePath)
    {
        std::ifstream file(filePath);
        if (!file.is_open())
        {
            return false;
        }

        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        // Parse tree ID and start node
        std::regex idRegex(R"~~("id"\s*:\s*"([^"]+)")~~");
        std::regex startRegex(R"~~("startNode"\s*:\s*"([^"]+)")~~");
        std::smatch match;

        if (std::regex_search(content, match, idRegex))
        {
            m_id = match[1].str();
        }
        if (std::regex_search(content, match, startRegex))
        {
            m_startNodeId = match[1].str();
        }

        // Parse nodes — simplified parser for the dialogue format
        std::regex nodeRegex(R"~~("nodeId"\s*:\s*"([^"]+)")~~");
        std::regex speakerRegex(R"~~("speaker"\s*:\s*"([^"]*)")~~");
        std::regex textRegex(R"~~("text"\s*:\s*"([^"]*)")~~");
        std::regex nextRegex(R"~~("next"\s*:\s*"([^"]*)")~~");

        auto nodeBegin = std::sregex_iterator(content.begin(), content.end(), nodeRegex);
        auto nodeEnd = std::sregex_iterator();

        for (auto it = nodeBegin; it != nodeEnd; ++it)
        {
            DialogueNode node;
            node.id = (*it)[1].str();
            node.type = DialogueNodeType::Text;

            // Search for properties near this node
            std::string nodeContext = content.substr(
                static_cast<size_t>(it->position()),
                (std::min)(static_cast<size_t>(500), content.size() - static_cast<size_t>(it->position())));

            if (std::regex_search(nodeContext, match, speakerRegex))
            {
                node.speakerName = match[1].str();
            }
            if (std::regex_search(nodeContext, match, textRegex))
            {
                node.text = match[1].str();
            }
            if (std::regex_search(nodeContext, match, nextRegex))
            {
                node.nextNodeId = match[1].str();
            }

            AddNode(node);
        }

        return !m_nodes.empty();
    }

    // =============================================================================
    // DialogueSystem
    // =============================================================================

    DialogueSystem::DialogueSystem() = default;

    bool DialogueSystem::LoadTree(const std::string& treeId, const std::string& filePath)
    {
        auto tree = std::make_unique<DialogueTree>();
        if (!tree->LoadFromFile(filePath))
        {
            return false;
        }
        tree->SetId(treeId);
        m_trees[treeId] = std::move(tree);
        return true;
    }

    void DialogueSystem::RegisterTree(const std::string& treeId, std::unique_ptr<DialogueTree> tree)
    {
        tree->SetId(treeId);
        m_trees[treeId] = std::move(tree);
    }

    bool DialogueSystem::StartConversation(const std::string& treeId)
    {
        auto it = m_trees.find(treeId);
        if (it == m_trees.end())
        {
            return false;
        }

        m_state.treeId = treeId;
        m_state.currentNodeId = it->second->GetStartNodeId();
        m_state.nodeTimer = 0.0f;
        m_state.waitingForInput = false;
        m_state.isActive = true;
        m_state.variables.clear();

        // Process the first node
        if (const auto* node = GetCurrentNode())
        {
            ProcessNode(*node);
        }

        return true;
    }

    void DialogueSystem::EndConversation()
    {
        std::string treeId = m_state.treeId;
        m_state.isActive = false;
        m_state.currentNodeId.clear();
        m_state.treeId.clear();

        for (const auto& callback : m_endCallbacks)
        {
            callback(treeId);
        }
    }

    void DialogueSystem::Update(float deltaTime)
    {
        if (!m_state.isActive)
        {
            return;
        }

        m_state.nodeTimer += deltaTime;

        const auto* node = GetCurrentNode();
        if (!node)
        {
            EndConversation();
            return;
        }

        // Auto-advance for text nodes with duration
        if (node->type == DialogueNodeType::Text && !m_state.waitingForInput)
        {
            if (node->displayDuration > 0.0f && m_state.nodeTimer >= node->displayDuration)
            {
                AdvanceNode();
            }
        }
    }

    bool DialogueSystem::SelectChoice(size_t choiceIndex)
    {
        const auto* node = GetCurrentNode();
        if (!node || node->type != DialogueNodeType::Choice)
        {
            return false;
        }

        auto available = GetAvailableChoices();
        if (choiceIndex >= available.size())
        {
            return false;
        }

        const auto& choice = available[choiceIndex];
        m_state.currentNodeId = choice.nextNodeId;
        m_state.nodeTimer = 0.0f;
        m_state.waitingForInput = false;

        if (const auto* nextNode = GetCurrentNode())
        {
            ProcessNode(*nextNode);
        }
        else
        {
            EndConversation();
        }

        return true;
    }

    void DialogueSystem::AdvanceNode()
    {
        const auto* node = GetCurrentNode();
        if (!node)
        {
            EndConversation();
            return;
        }

        if (node->type == DialogueNodeType::End || node->nextNodeId.empty())
        {
            EndConversation();
            return;
        }

        m_state.currentNodeId = node->nextNodeId;
        m_state.nodeTimer = 0.0f;
        m_state.waitingForInput = false;

        if (const auto* nextNode = GetCurrentNode())
        {
            ProcessNode(*nextNode);
        }
        else
        {
            EndConversation();
        }
    }

    const DialogueNode* DialogueSystem::GetCurrentNode() const
    {
        auto treeIt = m_trees.find(m_state.treeId);
        if (treeIt == m_trees.end())
        {
            return nullptr;
        }
        return treeIt->second->GetNode(m_state.currentNodeId);
    }

    std::vector<DialogueChoice> DialogueSystem::GetAvailableChoices() const
    {
        const auto* node = GetCurrentNode();
        if (!node || node->type != DialogueNodeType::Choice)
        {
            return {};
        }

        std::vector<DialogueChoice> available;
        for (const auto& choice : node->choices)
        {
            if (choice.condition.empty() || EvaluateCondition(choice.condition))
            {
                available.push_back(choice);
            }
        }
        return available;
    }

    void DialogueSystem::RegisterCondition(const std::string& name, std::function<bool(const std::string&)> evaluator)
    {
        m_conditionEvaluators[name] = std::move(evaluator);
    }

    void DialogueSystem::SetVariable(const std::string& name, const std::string& value)
    {
        m_state.variables[name] = value;
    }

    std::string DialogueSystem::GetVariable(const std::string& name) const
    {
        auto it = m_state.variables.find(name);
        return it != m_state.variables.end() ? it->second : "";
    }

    void DialogueSystem::OnDialogueEvent(std::function<void(const std::string&, const std::string&)> callback)
    {
        m_eventCallbacks.push_back(std::move(callback));
    }

    void DialogueSystem::OnConversationEnded(std::function<void(const std::string&)> callback)
    {
        m_endCallbacks.push_back(std::move(callback));
    }

    void DialogueSystem::ProcessNode(const DialogueNode& node)
    {
        switch (node.type)
        {
        case DialogueNodeType::Text:
            m_state.waitingForInput = (node.displayDuration <= 0.0f);
            break;

        case DialogueNodeType::Choice:
            m_state.waitingForInput = true;
            break;

        case DialogueNodeType::Branch:
        {
            bool result = EvaluateCondition(node.condition);
            m_state.currentNodeId = result ? node.trueNodeId : node.falseNodeId;
            m_state.nodeTimer = 0.0f;
            if (const auto* nextNode = GetCurrentNode())
            {
                ProcessNode(*nextNode);
            }
            break;
        }

        case DialogueNodeType::Event:
            for (const auto& callback : m_eventCallbacks)
            {
                callback(node.eventName, node.eventData);
            }
            // Auto-advance past event nodes
            if (!node.nextNodeId.empty())
            {
                m_state.currentNodeId = node.nextNodeId;
                m_state.nodeTimer = 0.0f;
                if (const auto* nextNode = GetCurrentNode())
                {
                    ProcessNode(*nextNode);
                }
            }
            break;

        case DialogueNodeType::End:
            EndConversation();
            break;
        }
    }

    bool DialogueSystem::EvaluateCondition(const std::string& condition) const
    {
        if (condition.empty())
        {
            return true;
        }

        // Parse condition: "functionName:param"
        size_t colonPos = condition.find(':');
        std::string funcName = (colonPos != std::string::npos) ? condition.substr(0, colonPos) : condition;
        std::string param = (colonPos != std::string::npos) ? condition.substr(colonPos + 1) : "";

        // Check conversation variables first
        auto varIt = m_state.variables.find(funcName);
        if (varIt != m_state.variables.end())
        {
            return varIt->second == param || (param.empty() && varIt->second == "true");
        }

        // Check registered evaluators
        auto evalIt = m_conditionEvaluators.find(funcName);
        if (evalIt != m_conditionEvaluators.end())
        {
            return evalIt->second(param);
        }

        return false;
    }

    std::string DialogueSystem::Console_GetStatus() const
    {
        std::ostringstream oss;
        oss << "=== Dialogue System ===\n";
        oss << "Trees loaded: " << m_trees.size() << "\n";
        oss << "Conversation active: " << (m_state.isActive ? "YES" : "NO") << "\n";
        if (m_state.isActive)
        {
            oss << "  Tree: " << m_state.treeId << "\n";
            oss << "  Node: " << m_state.currentNodeId << "\n";
            oss << "  Waiting for input: " << (m_state.waitingForInput ? "YES" : "NO") << "\n";
            if (const auto* node = GetCurrentNode())
            {
                oss << "  Speaker: " << node->speakerName << "\n";
                oss << "  Text: " << node->text << "\n";
            }
        }
        oss << "Condition evaluators: " << m_conditionEvaluators.size() << "\n";
        return oss.str();
    }

    std::string DialogueSystem::Console_ListTrees() const
    {
        std::ostringstream oss;
        oss << "=== Dialogue Trees ===\n";
        for (const auto& [id, tree] : m_trees)
        {
            oss << "  " << id << ": " << tree->GetNodeCount() << " nodes" << " (start: " << tree->GetStartNodeId()
                << ")\n";
        }
        return oss.str();
    }

} // namespace Spark
