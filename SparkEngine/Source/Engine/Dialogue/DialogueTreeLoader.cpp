/**
 * @file DialogueTreeLoader.cpp
 * @brief Production DialogueTree storage and bounded JSON file loader.
 */

#include "DialogueSystem.h"

#include "../../Utils/Hash.h"
#include "../../Utils/JsonUtils.h"

#include <fstream>

namespace Spark
{
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
        constexpr std::streamoff kMaxFileBytes = 8 * 1024 * 1024;
        constexpr size_t kMaxNodes = 100000;
        constexpr size_t kMaxChoicesPerNode = 4096;
        // Binary mode: the byte count from tellg must match what read() returns (text mode would
        // translate CRLF on Windows and fail every CRLF file on a short read).
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "DialogueTree: failed to open file: %s", filePath.c_str());
            return false;
        }

        file.seekg(0, std::ios::end);
        const std::streamoff fileSize = file.tellg();
        if (fileSize < 0 || fileSize > kMaxFileBytes)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "DialogueTree: file exceeds %lld byte limit: %s",
                            static_cast<long long>(kMaxFileBytes), filePath.c_str());
            return false;
        }
        file.seekg(0, std::ios::beg);
        std::string content(static_cast<size_t>(fileSize), '\0');
        if (fileSize != 0)
        {
            file.read(content.data(), fileSize);
            if (!file)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Core, "DialogueTree: failed to read file: %s", filePath.c_str());
                return false;
            }
        }

        Spark::Json::Value root;
        Spark::Json::JsonLimits limits;
        limits.maxBytes = static_cast<size_t>(kMaxFileBytes);
        limits.maxDepth = 32;
        limits.maxNodes = 500000;
        std::string parseError;
        if (!Spark::Json::ParseBounded(content, limits, &root, &parseError))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "DialogueTree: failed to parse JSON: %s", filePath.c_str());
            return false;
        }

        // Parse into a temporary tree so malformed input cannot leave a half-loaded object.
        DialogueTree parsed;
        if (root["id"].IsString())
        {
            parsed.m_id = root["id"].AsString();
        }
        if (root["startNode"].IsString())
        {
            parsed.m_startNodeId = root["startNode"].AsString();
        }

        const auto& nodes = root["nodes"];
        if (!nodes.IsArray())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "DialogueTree: missing 'nodes' array: %s", filePath.c_str());
            return false;
        }

        if (nodes.Size() > kMaxNodes)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "DialogueTree: '%s' has %zu nodes, above the %zu node limit",
                            filePath.c_str(), nodes.Size(), kMaxNodes);
            return false;
        }
        for (size_t i = 0; i < nodes.Size(); ++i)
        {
            const auto& jsonNode = nodes[i];
            if (!jsonNode["nodeId"].IsString())
            {
                SPARK_LOG_WARN(Spark::LogCategory::Core,
                               "DialogueTree: skipping node index %zu in '%s' — missing or non-string 'nodeId'", i,
                               filePath.c_str());
                continue;
            }

            DialogueNode node;
            node.id = jsonNode["nodeId"].AsString();
            node.type = DialogueNodeType::Text;
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
                    break;
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
            if (jsonNode["eventName"].IsString())
            {
                node.eventName = jsonNode["eventName"].AsString();
            }
            if (jsonNode["eventData"].IsString())
            {
                node.eventData = jsonNode["eventData"].AsString();
            }

            if (node.type == DialogueNodeType::Choice && jsonNode["choices"].IsArray())
            {
                const auto& choices = jsonNode["choices"];
                if (choices.Size() > kMaxChoicesPerNode)
                {
                    return false;
                }
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
            parsed.AddNode(node);
        }

        if (parsed.m_nodes.empty())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core,
                            "DialogueTree: '%s' contained no valid nodes (parsed %zu JSON entries)", filePath.c_str(),
                            nodes.Size());
            return false;
        }
        *this = std::move(parsed);
        return true;
    }
} // namespace Spark
