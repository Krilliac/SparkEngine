/** @brief libc++-compiled production adapter for DialogueTree fuzzing. */
#include "FuzzDialogueProduction.h"

#include "Engine/Dialogue/DialogueSystem.h"

#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace
{
    constexpr std::size_t kMaxInputBytes = 8u * 1024u * 1024u + 1u;
    std::atomic<std::uint64_t> s_fileCounter{0};

    std::uint64_t ProcessId()
    {
#if defined(_WIN32)
        return static_cast<std::uint64_t>(_getpid());
#else
        return static_cast<std::uint64_t>(getpid());
#endif
    }

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzDialogue: %s\n", what);
        std::abort();
    }

    void CheckReferences(const Spark::DialogueTree& tree)
    {
        if (!tree.GetStartNodeId().empty() && tree.GetNode(tree.GetStartNodeId()) == nullptr)
            InvariantFailure("accepted tree has a missing start node");
        for (const std::string& id : tree.GetNodeIds())
        {
            const Spark::DialogueNode* node = tree.GetNode(id);
            if (node == nullptr)
                InvariantFailure("node index returned a missing node");
            if (!node->nextNodeId.empty() && tree.GetNode(node->nextNodeId) == nullptr)
                InvariantFailure("accepted tree has a missing next-node reference");
            if (node->type == Spark::DialogueNodeType::Branch)
            {
                if (!node->trueNodeId.empty() && tree.GetNode(node->trueNodeId) == nullptr)
                    InvariantFailure("accepted tree has a missing true branch target");
                if (!node->falseNodeId.empty() && tree.GetNode(node->falseNodeId) == nullptr)
                    InvariantFailure("accepted tree has a missing false branch target");
            }
            for (const Spark::DialogueChoice& choice : node->choices)
                if (!choice.nextNodeId.empty() && tree.GetNode(choice.nextNodeId) == nullptr)
                    InvariantFailure("accepted tree has a missing choice target");
        }
    }
} // namespace

extern "C" int SparkFuzzParseDialogue(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
        return 0;
    const auto suffix = s_fileCounter.fetch_add(1, std::memory_order_relaxed);
    const auto tempDirectory = std::filesystem::temp_directory_path();
    std::string fileName = "spark-fuzz-dialogue-";
    fileName += std::to_string(ProcessId());
    fileName += "-";
    fileName += std::to_string(suffix);
    fileName += ".json";
    const std::filesystem::path path = tempDirectory / fileName;
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return 0;
        if (size != 0)
            file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    }
    Spark::DialogueTree tree;
    tree.SetId("sentinel");
    tree.SetStartNodeId("sentinel-node");
    Spark::DialogueNode sentinel;
    sentinel.id = "sentinel-node";
    sentinel.text = "sentinel";
    tree.AddNode(sentinel);
    const bool accepted = tree.LoadFromFile(path.string());
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    if (accepted)
        CheckReferences(tree);
    else if (tree.GetId() != "sentinel" || tree.GetStartNodeId() != "sentinel-node" || tree.GetNodeCount() != 1 ||
             tree.GetNode("sentinel-node") == nullptr)
        InvariantFailure("rejected dialogue input modified the caller's tree");
    return 0;
}
