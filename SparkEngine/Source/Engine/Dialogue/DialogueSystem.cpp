/**
 * @file DialogueSystem.cpp
 * @brief Implementation of the dialogue tree and conversation system
 */

#include "DialogueSystem.h"
#include "../../Utils/Hash.h"
#include "../../Utils/JsonUtils.h"
#include "../../Utils/Validate.h"

#include <fstream>
#include <sstream>

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

    DialogueNode* DialogueTree::GetMutableNode(const std::string& nodeId)
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
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "DialogueTree: failed to open file: %s", filePath.c_str());
            return false;
        }

        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        auto root = Spark::Json::Parse(content);
        if (root.IsNull())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "DialogueTree: failed to parse JSON: %s", filePath.c_str());
            return false;
        }

        // Parse tree ID and start node
        if (root["id"].IsString())
        {
            m_id = root["id"].AsString();
        }
        if (root["startNode"].IsString())
        {
            m_startNodeId = root["startNode"].AsString();
        }

        // Parse nodes array
        const auto& nodes = root["nodes"];
        if (!nodes.IsArray())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "DialogueTree: missing 'nodes' array: %s", filePath.c_str());
            return false;
        }

        for (size_t i = 0; i < nodes.Size(); ++i)
        {
            const auto& jsonNode = nodes[i];
            if (!jsonNode["nodeId"].IsString())
            {
                continue;
            }

            DialogueNode node;
            node.id = jsonNode["nodeId"].AsString();
            node.type = DialogueNodeType::Text;

            // Parse node type using compile-time hash dispatch
            if (jsonNode["type"].IsString())
            {
                using namespace Spark::HashLiterals;
                const std::string& typeStr = jsonNode["type"].AsString();
                switch (Spark::FNV1a64(typeStr))
                {
                case "Choice"_hash64:
                    node.type = DialogueNodeType::Choice;
                    break;
                case "Branch"_hash64:
                    node.type = DialogueNodeType::Branch;
                    break;
                case "Event"_hash64:
                    node.type = DialogueNodeType::Event;
                    break;
                case "End"_hash64:
                    node.type = DialogueNodeType::End;
                    break;
                default:
                    break; // Stays as DialogueNodeType::Text
                }
            }

            if (jsonNode["speaker"].IsString())
            {
                node.speakerName = jsonNode["speaker"].AsString();
            }
            if (jsonNode["text"].IsString())
            {
                node.text = jsonNode["text"].AsString();
            }
            if (jsonNode["next"].IsString())
            {
                node.nextNodeId = jsonNode["next"].AsString();
            }
            if (jsonNode["displayDuration"].IsNumber())
            {
                node.displayDuration = static_cast<float>(jsonNode["displayDuration"].AsNumber());
            }

            // Branch node fields
            if (jsonNode["condition"].IsString())
            {
                node.condition = jsonNode["condition"].AsString();
            }
            if (jsonNode["trueNodeId"].IsString())
            {
                node.trueNodeId = jsonNode["trueNodeId"].AsString();
            }
            if (jsonNode["falseNodeId"].IsString())
            {
                node.falseNodeId = jsonNode["falseNodeId"].AsString();
            }

            // Event node fields
            if (jsonNode["eventName"].IsString())
            {
                node.eventName = jsonNode["eventName"].AsString();
            }
            if (jsonNode["eventData"].IsString())
            {
                node.eventData = jsonNode["eventData"].AsString();
            }

            // Parse choices array for Choice nodes
            if (node.type == DialogueNodeType::Choice && jsonNode["choices"].IsArray())
            {
                const auto& choices = jsonNode["choices"];
                for (size_t c = 0; c < choices.Size(); ++c)
                {
                    const auto& jsonChoice = choices[c];
                    DialogueChoice choice;

                    if (jsonChoice["text"].IsString())
                    {
                        choice.text = jsonChoice["text"].AsString();
                    }
                    if (jsonChoice["nextNodeId"].IsString())
                    {
                        choice.nextNodeId = jsonChoice["nextNodeId"].AsString();
                    }
                    if (jsonChoice["condition"].IsString())
                    {
                        choice.condition = jsonChoice["condition"].AsString();
                    }

                    node.choices.push_back(std::move(choice));
                }
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
        SPARK_TRACE_ENTER(Spark::LogCategory::Game);
        SPARK_VALIDATE_RET(Spark::LogCategory::Game, !treeId.empty(), false);
        SPARK_VALIDATE_RET(Spark::LogCategory::Game, !filePath.empty(), false);
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
        SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Game, tree);
        tree->SetId(treeId);
        m_trees[treeId] = std::move(tree);
    }

    bool DialogueSystem::StartConversation(const std::string& treeId)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Game);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Starting conversation with tree '%s'", treeId.c_str());
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
        SPARK_TRACE_ENTER(Spark::LogCategory::Game);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Ending conversation with tree '%s'", m_state.treeId.c_str());
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
        SPARK_TRACE_ENTER(Spark::LogCategory::Game);
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

        // Mark the selected choice as visited in the actual tree node
        auto treeIt = m_trees.find(m_state.treeId);
        if (treeIt != m_trees.end())
        {
            DialogueNode* mutableNode = treeIt->second->GetMutableNode(m_state.currentNodeId);
            if (mutableNode)
            {
                for (auto& treeChoice : mutableNode->choices)
                {
                    if (treeChoice.text == choice.text && treeChoice.nextNodeId == choice.nextNodeId)
                    {
                        treeChoice.visited = true;
                        break;
                    }
                }
            }
        }

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
        ++m_processDepth;
        if (m_processDepth > kMaxProcessDepth)
        {
            SPARK_LOG_WARN("Dialogue",
                           "ProcessNode exceeded max recursion depth (%d) - possible cycle at node '%s'. "
                           "Ending conversation.",
                           kMaxProcessDepth, node.id.c_str());
            m_processDepth = 0;
            EndConversation();
            return;
        }

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

        --m_processDepth;
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
