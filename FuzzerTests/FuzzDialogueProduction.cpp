/** @brief libc++-compiled production adapter for DialogueTree fuzzing. */
#include "FuzzDialogueProduction.h"

#include "Engine/Dialogue/DialogueSystem.h"

#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace
{
    constexpr std::size_t kMaxDocumentBytes = 8u * 1024u * 1024u;
    constexpr std::size_t kMaxInputBytes = kMaxDocumentBytes + 1u;
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

    // The loader does not promise that node references resolve (DialogueSystem ends a
    // conversation on a missing node), so the oracle checks what it does promise: a
    // non-empty, self-consistent node index within the documented node and choice caps.
    void CheckTreeShape(const Spark::DialogueTree& tree)
    {
        constexpr std::size_t kMaxNodes = 100000;
        constexpr std::size_t kMaxChoicesPerNode = 4096;
        const std::vector<std::string> ids = tree.GetNodeIds();
        if (ids.empty() || ids.size() != tree.GetNodeCount() || ids.size() > kMaxNodes)
            InvariantFailure("accepted tree has an empty, inconsistent or oversized node index");
        for (const std::string& id : ids)
        {
            const Spark::DialogueNode* node = tree.GetNode(id);
            if (node == nullptr || node->id != id)
                InvariantFailure("node index does not resolve to the node it names");
            if (node->choices.size() > kMaxChoicesPerNode ||
                (!node->choices.empty() && node->type != Spark::DialogueNodeType::Choice))
                InvariantFailure("accepted node carries choices outside the loader contract");
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
    if (accepted && size > kMaxDocumentBytes)
        InvariantFailure("accepted dialogue file exceeds the documented size cap");
    if (accepted)
        CheckTreeShape(tree);
    else if (tree.GetId() != "sentinel" || tree.GetStartNodeId() != "sentinel-node" || tree.GetNodeCount() != 1 ||
             tree.GetNode("sentinel-node") == nullptr)
        InvariantFailure("rejected dialogue input modified the caller's tree");
    return 0;
}
