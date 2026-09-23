#include "SceneManager.h"
#include "../Core/Platform.h"
#include "Game/GameObject.h"
#include "Game/CubeObject.h"
#include "Game/PlaneObject.h"
#include "Game/SphereObject.h"
#include "Game/PlaceholderMesh.h"
#include "Game/PyramidObject.h"
#include "Game/RampObject.h"
#include "Game/WallObject.h"
#include "Graphics/GraphicsEngine.h"
#include "Engine/Events/EventSystem.h"
#include "Core/EngineContext.h"
#include "Utils/Assert.h"
#include "Utils/DebugHookManager.h"
#include "../Utils/Validate.h"
#include "../Utils/SparkConsole.h"
#include "../Utils/ConsoleProcessManager.h"
#include "Utils/LocalFileCache.h"

#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cmath>
#include <cerrno>
#include <atomic>
#include <optional>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

using DirectX::XMFLOAT3;

// Helper: convert wstring path to string for cross-platform file I/O
static std::string WideToNarrow(const std::wstring& wide)
{
    std::string narrow;
    narrow.reserve(wide.size());
    for (wchar_t wc : wide)
        narrow.push_back(static_cast<char>(wc));
    return narrow;
}

// Narrow cache/file APIs are usable only when their spelling resolves to the
// same native path. Keep the existing fast path for ASCII/ACP-compatible
// paths, but never let a lossy conversion choose a different file or cache key.
static std::optional<std::string> NarrowPathIfRoundTrips(const std::wstring& wide)
{
    const std::string narrow = WideToNarrow(wide);
    try
    {
        if (std::filesystem::path(narrow) == std::filesystem::path(wide))
            return narrow;
    }
    catch (const std::exception&)
    {
    }
    return std::nullopt;
}

namespace
{
    bool IsFinite(const DirectX::XMFLOAT3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    // Rebuild the derived child lists only after every parent reference has
    // been read. This deliberately permits forward references in the stable
    // line-oriented format while rejecting dangling/cyclic hierarchies.
    bool ValidateAndRebuildHierarchy(std::vector<SceneNode>& nodes)
    {
        for (auto& node : nodes)
            node.childIndices.clear();

        for (size_t i = 0; i < nodes.size(); ++i)
        {
            auto& node = nodes[i];
            if (node.type.empty() || node.name.empty() || !IsFinite(node.position) || !IsFinite(node.rotation) ||
                !IsFinite(node.scale))
                return false;
            if (node.parentIndex < -1 || node.parentIndex >= static_cast<int>(nodes.size()) ||
                node.parentIndex == static_cast<int>(i))
                return false;
            if (node.parentIndex >= 0)
                nodes[static_cast<size_t>(node.parentIndex)].childIndices.push_back(static_cast<int>(i));
        }

        // A parent chain must terminate at a root. Besides catching cycles,
        // this bounds validation independently of scene size.
        for (size_t i = 0; i < nodes.size(); ++i)
        {
            size_t current = i;
            for (size_t steps = 0; steps <= nodes.size(); ++steps)
            {
                const int parent = nodes[current].parentIndex;
                if (parent < 0)
                    break;
                current = static_cast<size_t>(parent);
                if (steps == nodes.size())
                    return false;
            }
        }
        return true;
    }

    bool FlushFileDurably(const std::filesystem::path& path, std::error_code& error)
    {
#if defined(_WIN32)
        const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                          FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            error = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
            return false;
        }
        const bool flushed = ::FlushFileBuffers(file) != FALSE;
        const DWORD flushError = flushed ? ERROR_SUCCESS : ::GetLastError();
        ::CloseHandle(file);
        if (!flushed)
        {
            error = std::error_code(static_cast<int>(flushError), std::system_category());
            return false;
        }
        return true;
#else
        const int file = ::open(path.c_str(), O_RDONLY);
        if (file < 0)
        {
            error = std::error_code(errno, std::generic_category());
            return false;
        }
        const bool flushed = ::fsync(file) == 0;
        const int flushError = flushed ? 0 : errno;
        ::close(file);
        if (!flushed)
        {
            error = std::error_code(flushError, std::generic_category());
            return false;
        }
        return true;
#endif
    }

    bool ReplaceFileAtomically(const std::filesystem::path& temporary, const std::filesystem::path& destination,
                               std::error_code& error)
    {
#if defined(_WIN32)
        if (::MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
        error = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
        return false;
#else
        std::filesystem::rename(temporary, destination, error);
        if (error)
            return false;
        const auto directory = destination.has_parent_path() ? destination.parent_path() : std::filesystem::path(".");
#if defined(O_DIRECTORY)
        const int directoryFile = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
#else
        const int directoryFile = ::open(directory.c_str(), O_RDONLY);
#endif
        if (directoryFile < 0)
        {
            error = std::error_code(errno, std::generic_category());
            SPARK_LOG_WARN(Spark::LogCategory::Scene,
                           "SceneManager: destination replaced but directory fsync is unavailable: %s",
                           error.message().c_str());
            return true;
        }
        const bool flushed = ::fsync(directoryFile) == 0;
        const int flushError = flushed ? 0 : errno;
        ::close(directoryFile);
        if (!flushed)
        {
            error = std::error_code(flushError, std::generic_category());
            // rename() already made the new image visible. Do not report a
            // failed save or attempt rollback after that point: callers rely
            // on false meaning that the old destination remains intact, and
            // directory fsync is unsupported on some macOS filesystems.
            SPARK_LOG_WARN(Spark::LogCategory::Scene,
                           "SceneManager: destination replaced but directory fsync failed: %s",
                           error.message().c_str());
        }
        return true;
#endif
    }

    std::filesystem::path MakeUniqueTemporaryPath(const std::filesystem::path& destination)
    {
        static std::atomic<uint64_t> sequence{0};
        const auto threadHash = std::hash<std::thread::id>{}(std::this_thread::get_id());
        for (int attempt = 0; attempt < 64; ++attempt)
        {
            const uint64_t serial = sequence.fetch_add(1, std::memory_order_relaxed);
            std::filesystem::path candidate = destination;
            candidate += ".tmp." + std::to_string(threadHash) + "." + std::to_string(serial);
            std::error_code existsError;
            if (!std::filesystem::exists(candidate, existsError) && !existsError)
                return candidate;
        }
        return {};
    }

    bool WriteDurableText(const std::filesystem::path& path, const std::string& text, std::error_code& error)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output.is_open())
            return false;
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        output.close();
        if (output.fail())
            return false;
        return FlushFileDurably(path, error);
    }

    void RemoveFileNoThrow(const std::filesystem::path& path)
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
} // namespace

// Use logging macros from LogMacros.h (included transitively via headers)

SceneManager::SceneManager(GraphicsEngine* graphics, InputManager* input) : m_graphics(graphics), m_input(input)
{
    SPARK_LOG_INFO(Spark::LogCategory::Scene, "SceneManager constructed");
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Scene, graphics);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Scene, input);
}

SceneManager::~SceneManager()
{
    // Join any in-flight async load thread to avoid use-after-free
    if (m_asyncLoadThread.joinable())
    {
        m_asyncLoadThread.join();
    }
}

const std::vector<std::unique_ptr<GameObject>>& SceneManager::GetObjects() const
{
    return m_objects;
}

// ============================================================================
// Scene Loading / Saving
// ============================================================================

bool SceneManager::LoadScene(const std::wstring& filepath)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Scene);
    SPARK_REQUIRE_MSG(Spark::LogCategory::Scene, !filepath.empty(),
                      "SceneManager::LoadScene — filepath must not be empty");
    std::string narrowName(filepath.begin(), filepath.end());
    SPARK_DEBUG_HOOK_SCENE(ScenePreLoad, narrowName);

    // Inspect native path components before any file access. Narrowing a wide
    // profile path can change its spelling and is not a traversal check.
    const std::filesystem::path nativeScenePath(filepath);
    if (std::find(nativeScenePath.begin(), nativeScenePath.end(), std::filesystem::path(L"..")) !=
        nativeScenePath.end())
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"SceneManager: Path traversal rejected: " + filepath, L"ERROR");
        return false;
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"SceneManager::LoadScene called. filepath=" + filepath, L"OPERATION");

    // Loading formats currently clear the live graph before parsing. Keep a
    // value snapshot so a malformed or unreadable replacement cannot leave the
    // running editor/game with an empty half-scene. Objects are recreated from
    // the restored nodes below; this is intentionally synchronous and bounded
    // to the existing scene-load path.
    const auto previousNodes = m_sceneNodes;
    const auto previousMetadata = m_metadata;
    const auto previousFilePath = m_currentFilePath;
    const bool previousDirty = m_dirty;
    // Keep ownership of the live objects out of the loader while it stages a
    // replacement.  Re-instantiating on rollback is observably wrong: callers
    // may retain a GameObject pointer and runtime state (health, physics,
    // component state, etc.) must survive a failed reload byte-for-byte.
    auto previousObjects = std::move(m_objects);
    const auto restorePrevious =
        [this, &previousNodes, &previousMetadata, &previousFilePath, previousDirty, &previousObjects]()
    {
        m_objects.clear();
        m_sceneNodes = previousNodes;
        m_metadata = previousMetadata;
        m_currentFilePath = previousFilePath;
        m_dirty = previousDirty;
        m_nodeNameIndex.clear();
        for (int i = 0; i < static_cast<int>(m_sceneNodes.size()); ++i)
        {
            if (!m_sceneNodes[i].name.empty())
                m_nodeNameIndex[m_sceneNodes[i].name] = i;
        }
        m_objects = std::move(previousObjects);
    };

    auto ext = std::filesystem::path(filepath).extension();
    bool loaded = false;
    const bool previousEventSuppression = m_suppressSceneEvents;
    m_suppressSceneEvents = true;

    try
    {
        if (ext == L".scene")
            loaded = LoadCustom(filepath);
        else if (ext == L".json")
            loaded = LoadJSON(filepath);
        else
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Scene file extension not recognized: " + filepath, L"WARNING");
            restorePrevious();
            m_suppressSceneEvents = previousEventSuppression;
            return false;
        }
    }
    catch (...)
    {
        restorePrevious();
        m_suppressSceneEvents = previousEventSuppression;
        return false;
    }

    if (!loaded)
    {
        restorePrevious();
    }
    else
        // The replacement is now committed; release the old graph only after
        // all parsing and instantiation succeeded.
        previousObjects.clear();

    m_suppressSceneEvents = previousEventSuppression;

    if (loaded)
    {
        m_currentFilePath = filepath;
        m_dirty = false;
        const auto instantiated =
            std::count_if(m_objects.begin(), m_objects.end(), [](const auto& object) { return object != nullptr; });
        LOG_TO_CONSOLE_IMMEDIATE(L"Scene loaded: " + std::to_wstring(instantiated) + L" objects, " +
                                     std::to_wstring(m_sceneNodes.size()) + L" nodes",
                                 L"SUCCESS");

        SPARK_DEBUG_HOOK_SCENE(ScenePostLoad, narrowName);

        // Publish SceneLoadedEvent
        if (auto* ctx = EngineContext::Get())
        {
            if (auto* bus = ctx->GetEventBus())
            {
                std::string narrowPath(filepath.begin(), filepath.end());
                bus->Publish(Spark::SceneLoadedEvent{narrowPath});
            }
        }
    }
    return loaded;
}

bool SceneManager::SaveScene(const std::wstring& filepath) const
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Scene);
    SPARK_REQUIRE_MSG(Spark::LogCategory::Scene, !filepath.empty(),
                      "SceneManager::SaveScene — filepath must not be empty");
    LOG_TO_CONSOLE_IMMEDIATE(L"SceneManager::SaveScene called. filepath=" + filepath, L"OPERATION");
    bool saved = SaveJSON(filepath);
    if (saved)
    {
        m_currentFilePath = filepath;
        m_dirty = false;
    }
    return saved;
}

void SceneManager::LoadSceneAsync(const std::wstring& filepath, SceneLoadCallback callback)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Scene);
    SPARK_REQUIRE_MSG(Spark::LogCategory::Scene, !filepath.empty(),
                      "SceneManager::LoadSceneAsync — filepath must not be empty");
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Scene, callback);
    // Join any previous async load before starting a new one
    if (m_asyncLoadThread.joinable())
    {
        m_asyncLoadThread.join();
    }

    m_asyncLoading = true;
    m_asyncLoadThread = std::thread(
        [this, filepath, callback]()
        {
            bool success = false;
            {
                std::lock_guard<std::mutex> lock(m_sceneMutex);
                success = LoadScene(filepath);
            }
            m_asyncLoading = false;
            if (callback)
                callback(success, std::string(filepath.begin(), filepath.end()));
        });
}

// ============================================================================
// Scene Hierarchy
// ============================================================================

int SceneManager::AddNode(const SceneNode& node)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Scene, !node.name.empty(),
                      "SceneManager::AddNode — node name must not be empty");
    int index = static_cast<int>(m_sceneNodes.size());
    m_sceneNodes.push_back(node);

    // Update name-to-index cache
    if (!node.name.empty())
    {
        m_nodeNameIndex[node.name] = index;
    }

    // Update parent's child list if parent specified
    if (node.parentIndex >= 0 && node.parentIndex < static_cast<int>(m_sceneNodes.size()))
    {
        m_sceneNodes[node.parentIndex].childIndices.push_back(index);
    }

    m_dirty = true;
    return index;
}

void SceneManager::RemoveNode(int index)
{
    if (index < 0 || index >= static_cast<int>(m_sceneNodes.size()))
        return;

    // Recursively remove children first
    auto& node = m_sceneNodes[index];
    for (int childIdx : node.childIndices)
        RemoveNode(childIdx);

    // Remove from parent's child list
    if (node.parentIndex >= 0 && node.parentIndex < static_cast<int>(m_sceneNodes.size()))
    {
        auto& parentChildren = m_sceneNodes[node.parentIndex].childIndices;
        parentChildren.erase(std::remove(parentChildren.begin(), parentChildren.end(), index), parentChildren.end());
    }

    // Remove from name cache before tombstoning
    if (!node.name.empty())
    {
        m_nodeNameIndex.erase(node.name);
    }

    // Mark as removed (tombstone approach to avoid index invalidation)
    node.name = "";
    node.type = "";
    m_dirty = true;
}

void SceneManager::SetParent(int childIndex, int parentIndex)
{
    if (childIndex < 0 || childIndex >= static_cast<int>(m_sceneNodes.size()))
        return;

    auto& child = m_sceneNodes[childIndex];

    // Remove from old parent
    if (child.parentIndex >= 0 && child.parentIndex < static_cast<int>(m_sceneNodes.size()))
    {
        auto& oldParentChildren = m_sceneNodes[child.parentIndex].childIndices;
        oldParentChildren.erase(std::remove(oldParentChildren.begin(), oldParentChildren.end(), childIndex),
                                oldParentChildren.end());
    }

    // Set new parent
    child.parentIndex = parentIndex;
    if (parentIndex >= 0 && parentIndex < static_cast<int>(m_sceneNodes.size()))
    {
        m_sceneNodes[parentIndex].childIndices.push_back(childIndex);
    }

    m_dirty = true;
}

std::vector<int> SceneManager::GetRootNodes() const
{
    std::vector<int> roots;
    for (int i = 0; i < static_cast<int>(m_sceneNodes.size()); ++i)
    {
        if (m_sceneNodes[i].parentIndex < 0 && !m_sceneNodes[i].type.empty())
            roots.push_back(i);
    }
    return roots;
}

const SceneNode* SceneManager::GetNode(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_sceneNodes.size()))
        return nullptr;
    return &m_sceneNodes[index];
}

SceneNode* SceneManager::GetNode(int index)
{
    if (index < 0 || index >= static_cast<int>(m_sceneNodes.size()))
        return nullptr;
    return &m_sceneNodes[index];
}

int SceneManager::FindNode(const std::string& name) const
{
    // O(1) lookup via name cache instead of O(n) linear scan
    auto it = m_nodeNameIndex.find(name);
    if (it != m_nodeNameIndex.end())
        return it->second;
    return -1;
}

// ============================================================================
// Prefab System
// ============================================================================

bool SceneManager::SavePrefab(int nodeIndex, const std::wstring& filepath) const
{
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(m_sceneNodes.size()))
    {
        SPARK_LOG_WARN(Spark::LogCategory::Scene, "SavePrefab: nodeIndex %d out of range [0, %zu)", nodeIndex,
                       m_sceneNodes.size());
        return false;
    }

    std::string narrowPath = WideToNarrow(filepath);
    std::ofstream file(narrowPath);
    if (!file.is_open())
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Scene, "SavePrefab: failed to open '%s' for writing (errno=%d)",
                        narrowPath.c_str(), errno);
        return false;
    }

    // Write prefab header
    file << "# SparkEngine Prefab v1.0\n";

    // Collect the node subtree
    std::vector<int> subtree;
    std::function<void(int)> collectNodes = [&](int idx)
    {
        subtree.push_back(idx);
        if (idx < static_cast<int>(m_sceneNodes.size()))
        {
            for (int child : m_sceneNodes[idx].childIndices)
                collectNodes(child);
        }
    };
    collectNodes(nodeIndex);

    // Write each node
    for (int idx : subtree)
    {
        const auto& node = m_sceneNodes[idx];
        file << node.type << " " << node.name << " " << node.position.x << " " << node.position.y << " "
             << node.position.z << " " << node.rotation.x << " " << node.rotation.y << " " << node.rotation.z << " "
             << node.scale.x << " " << node.scale.y << " " << node.scale.z << " "
             << (idx == nodeIndex ? -1 : node.parentIndex) << "\n";
    }

    file.close();
    return true;
}

int SceneManager::LoadPrefab(const std::wstring& filepath, const DirectX::XMFLOAT3& position)
{
    std::string narrowPath = WideToNarrow(filepath);
    std::ifstream file(narrowPath);
    if (!file.is_open())
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Scene, "LoadPrefab: failed to open prefab '%s' (errno=%d)",
                        narrowPath.c_str(), errno);
        return -1;
    }

    std::string line;
    int rootIndex = -1;

    while (std::getline(file, line))
    {
        if (line.empty() || line[0] == '#')
            continue;

        std::istringstream ss(line);
        SceneNode node;
        ss >> node.type >> node.name >> node.position.x >> node.position.y >> node.position.z >> node.rotation.x >>
            node.rotation.y >> node.rotation.z >> node.scale.x >> node.scale.y >> node.scale.z >> node.parentIndex;

        // Offset position for the root node
        if (rootIndex < 0)
        {
            node.position.x += position.x;
            node.position.y += position.y;
            node.position.z += position.z;
        }

        int idx = AddNode(node);
        if (rootIndex < 0)
            rootIndex = idx;
    }

    InstantiateNodes();
    return rootIndex;
}

// ============================================================================
// Scene State
// ============================================================================

void SceneManager::NewScene(const std::string& name)
{
    Clear();
    m_metadata.sceneName = name;
    m_currentFilePath.clear();
    m_dirty = false;
}

void SceneManager::Clear()
{
    // Publish SceneUnloadedEvent before clearing
    if (!m_suppressSceneEvents && !m_currentFilePath.empty())
    {
        if (auto* ctx = EngineContext::Get())
        {
            if (auto* bus = ctx->GetEventBus())
            {
                std::string narrowPath(m_currentFilePath.begin(), m_currentFilePath.end());
                bus->Publish(Spark::SceneUnloadedEvent{narrowPath});
            }
        }
    }

    m_objects.clear();
    m_sceneNodes.clear();
    m_nodeNameIndex.clear();
    m_dirty = true;
}

std::vector<std::string> SceneManager::GetAvailableScenes(const std::wstring& directory) const
{
    std::vector<std::string> scenes;
    if (!std::filesystem::exists(directory))
        return scenes;

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
    {
        auto ext = entry.path().extension();
        if (ext == L".scene" || ext == L".json")
        {
            scenes.push_back(entry.path().filename().string());
        }
    }
    std::sort(scenes.begin(), scenes.end());
    return scenes;
}

// ============================================================================
// JSON Scene Format
// ============================================================================

bool SceneManager::LoadJSON(const std::wstring& path)
{
    std::string content;
    const std::filesystem::path nativePath(path);

    if (m_fileCache)
    {
        if (const auto narrowPath = NarrowPathIfRoundTrips(path))
        {
            auto result = m_fileCache->ReadText(*narrowPath);
            if (result.IsOk())
                content = result.Value();
        }
    }

    if (content.empty())
    {
        std::ifstream file(nativePath);
        if (!file.is_open())
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"SceneManager: Cannot open JSON scene: " + path, L"ERROR");
            return false;
        }
        content.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();
    }

    // Simple text-based scene format (not actual JSON despite the method name):
    //   Lines starting with # are comments/metadata
    //   Data lines: type name posX posY posZ [rotX rotY rotZ scaleX scaleY scaleZ parentIndex]
    // Parse into isolated state. A malformed row must not clear or partially
    // replace the live scene (LoadScene also preserves the object graph).
    SceneMetadata stagedMetadata = m_metadata;
    std::vector<SceneNode> stagedNodes;
    std::istringstream ss(content);
    std::string line;

    // Parse metadata from comment headers
    while (std::getline(ss, line))
    {
        if (line.empty())
            continue;
        if (line[0] == '#')
        {
            // Parse metadata comments like "# name: MyScene"
            if (line.find("# name:") == 0)
                stagedMetadata.sceneName = line.substr(8);
            else if (line.find("# gravity:") == 0)
            {
                std::istringstream gs(line.substr(11));
                if (!(gs >> stagedMetadata.gravityX >> stagedMetadata.gravityY >> stagedMetadata.gravityZ) ||
                    !std::isfinite(stagedMetadata.gravityX) || !std::isfinite(stagedMetadata.gravityY) ||
                    !std::isfinite(stagedMetadata.gravityZ))
                    return false;
            }
            else if (line.find("# author:") == 0)
                stagedMetadata.author = line.substr(10);
            else if (line.find("# version:") == 0)
                stagedMetadata.version = line.substr(11);
            else if (line.find("# description:") == 0)
                stagedMetadata.description = line.substr(15);
            else if (line.find("# ambient:") == 0)
            {
                std::istringstream as(line.substr(11));
                if (!(as >> stagedMetadata.ambientLightR >> stagedMetadata.ambientLightG >>
                      stagedMetadata.ambientLightB) ||
                    !std::isfinite(stagedMetadata.ambientLightR) || !std::isfinite(stagedMetadata.ambientLightG) ||
                    !std::isfinite(stagedMetadata.ambientLightB))
                    return false;
            }
            continue;
        }
        if (line[0] == '/' || line[0] == '{' || line[0] == '}')
            continue;

        // Parse node line: type name posX posY posZ [rotX rotY rotZ scaleX scaleY scaleZ parentIndex]
        std::istringstream ls(line);
        SceneNode node;
        // std::quoted also accepts historical unquoted names, while new saves
        // can round-trip names containing whitespace and quotes.
        if (!(ls >> node.type >> std::quoted(node.name) >> node.position.x >> node.position.y >> node.position.z) ||
            node.type.empty() || node.name.empty() || !IsFinite(node.position))
            return false;

        // Extended fields are optional as required for old line-oriented files,
        // but once present they are an all-or-nothing, finite record. Previously
        // a bad coordinate caused extraction to fail and silently left defaults.
        ls >> std::ws;
        if (!ls.eof())
        {
            if (!(ls >> node.rotation.x >> node.rotation.y >> node.rotation.z >> node.scale.x >> node.scale.y >>
                  node.scale.z >> node.parentIndex) ||
                !IsFinite(node.rotation) || !IsFinite(node.scale))
                return false;

            ls >> std::ws;
            if (!ls.eof())
            {
                size_t propertyCount = 0;
                if (!(ls >> std::quoted(node.modelPath) >> std::quoted(node.materialPath) >> propertyCount))
                    return false;
                for (size_t property = 0; property < propertyCount; ++property)
                {
                    std::string key;
                    std::string value;
                    if (!(ls >> std::quoted(key) >> std::quoted(value)))
                        return false;
                    node.properties.emplace(std::move(key), std::move(value));
                }
                ls >> std::ws;
                if (!ls.eof())
                    return false;
            }
        }
        stagedNodes.push_back(std::move(node));
    }

    if (stagedNodes.empty() || !ValidateAndRebuildHierarchy(stagedNodes))
        return false;

    Clear();
    m_metadata = std::move(stagedMetadata);
    m_sceneNodes = std::move(stagedNodes);
    m_nodeNameIndex.clear();
    for (int i = 0; i < static_cast<int>(m_sceneNodes.size()); ++i)
        m_nodeNameIndex[m_sceneNodes[static_cast<size_t>(i)].name] = i;
    InstantiateNodes();
    return true;
}

bool SceneManager::SaveJSON(const std::wstring& path) const
{
    std::vector<SceneNode> nodes;
    nodes.reserve(m_sceneNodes.size());
    std::vector<int> remap(m_sceneNodes.size(), -1);
    for (size_t oldIndex = 0; oldIndex < m_sceneNodes.size(); ++oldIndex)
    {
        if (!m_sceneNodes[oldIndex].type.empty())
        {
            remap[oldIndex] = static_cast<int>(nodes.size());
            nodes.push_back(m_sceneNodes[oldIndex]);
        }
    }
    for (auto& node : nodes)
    {
        if (node.parentIndex >= 0 && node.parentIndex < static_cast<int>(remap.size()))
        {
            const int mappedParent = remap[static_cast<size_t>(node.parentIndex)];
            node.parentIndex = mappedParent >= 0 ? mappedParent : -2;
        }
        else if (node.parentIndex < -1)
            node.parentIndex = -2; // validation below reports this as malformed
    }
    if (nodes.empty() || !ValidateAndRebuildHierarchy(nodes) || !std::isfinite(m_metadata.gravityX) ||
        !std::isfinite(m_metadata.gravityY) || !std::isfinite(m_metadata.gravityZ) ||
        !std::isfinite(m_metadata.ambientLightR) || !std::isfinite(m_metadata.ambientLightG) ||
        !std::isfinite(m_metadata.ambientLightB))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"SceneManager: Cannot save malformed scene hierarchy: " + path, L"ERROR");
        return false;
    }

    std::ostringstream serialized;
    serialized << "# SparkEngine Scene v1.0\n";
    serialized << "# name: " << m_metadata.sceneName << "\n";
    serialized << "# author: " << m_metadata.author << "\n";
    serialized << "# version: " << m_metadata.version << "\n";
    serialized << "# description: " << m_metadata.description << "\n";
    serialized << "# gravity: " << m_metadata.gravityX << " " << m_metadata.gravityY << " " << m_metadata.gravityZ
               << "\n";
    serialized << "# ambient: " << m_metadata.ambientLightR << " " << m_metadata.ambientLightG << " "
               << m_metadata.ambientLightB << "\n\n";

    for (const auto& node : nodes)
    {
        serialized << node.type << " " << std::quoted(node.name) << " " << std::fixed << std::setprecision(3)
                   << node.position.x << " " << node.position.y << " " << node.position.z << " " << node.rotation.x
                   << " " << node.rotation.y << " " << node.rotation.z << " " << node.scale.x << " " << node.scale.y
                   << " " << node.scale.z << " " << node.parentIndex << " " << std::quoted(node.modelPath) << " "
                   << std::quoted(node.materialPath) << " " << node.properties.size();
        std::vector<std::pair<std::string, std::string>> properties(node.properties.begin(), node.properties.end());
        std::sort(properties.begin(), properties.end());
        for (const auto& [key, value] : properties)
            serialized << " " << std::quoted(key) << " " << std::quoted(value);
        serialized << "\n";
    }

    const std::filesystem::path destination(path);
    const std::filesystem::path temporary = MakeUniqueTemporaryPath(destination);
    if (temporary.empty())
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"SceneManager: Cannot allocate unique temporary scene path: " + path, L"ERROR");
        return false;
    }
    std::error_code error;
    if (!WriteDurableText(temporary, serialized.str(), error) || !ReplaceFileAtomically(temporary, destination, error))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"SceneManager: Cannot save scene atomically: " + path, L"ERROR");
        RemoveFileNoThrow(temporary);
        return false;
    }

    if (m_fileCache)
    {
        if (const auto narrowPath = NarrowPathIfRoundTrips(path))
            m_fileCache->Invalidate(*narrowPath);
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Scene saved: " + std::to_wstring(m_sceneNodes.size()) + L" nodes", L"SUCCESS");
    return true;
}

void SceneManager::InstantiateNodes()
{
    if (!m_graphics || !m_graphics->GetDevice() || !m_graphics->GetContext())
    {
        // Data-only scenes still preserve the documented node/object index
        // relationship; mesh slots remain null until a graphics device exists.
        m_objects.clear();
        m_objects.resize(m_sceneNodes.size());
        return;
    }

    for (const auto& node : m_sceneNodes)
    {
        if (node.type.empty())
        {
            m_objects.push_back(nullptr);
            continue;
        }

        // These authored scene entries carry game data, not renderable meshes.
        if (node.type == "Camera" || node.type == "SpawnPoint")
        {
            m_objects.push_back(nullptr);
            continue;
        }

        std::unique_ptr<GameObject> obj;

        // Determine mesh path: use modelPath if specified, otherwise construct from type
        std::wstring meshPath;
        if (!node.modelPath.empty())
        {
            meshPath = std::wstring(node.modelPath.begin(), node.modelPath.end());
        }

        // Primitives are built at UNIT size and scaled via the node's scale so
        // non-uniform scales work; their constructor geometry is authoritative
        // (no mesh file load). Previously the ctor took node.scale.x AND
        // LoadOrPlaceholderMesh replaced the geometry with a 1 m placeholder
        // cube whenever "Assets/Models/<type>.obj" did not exist — a 4096 m
        // scene terrain plane silently rendered as a 1 m cube.
        bool isPrimitive = true;
        if (node.type == "Cube" || node.type == "cube")
            obj = std::make_unique<CubeObject>(1.0f);
        else if (node.type == "Plane" || node.type == "plane")
            obj = std::make_unique<PlaneObject>(1.0f, 1.0f);
        else if (node.type == "Sphere" || node.type == "sphere")
            obj = std::make_unique<SphereObject>(0.5f, 16, 16);
        else if (node.type == "Pyramid" || node.type == "pyramid")
            obj = std::make_unique<PyramidObject>(1.0f);
        else if (node.type == "Ramp" || node.type == "ramp")
            obj = std::make_unique<RampObject>(1.0f, 1.0f);
        else if (node.type == "Wall" || node.type == "wall")
            obj = std::make_unique<WallObject>(1.0f, 1.0f);
        else if (node.type == "model" || node.type == "Model")
        {
            // Model type: create a cube as placeholder geometry, then load the actual mesh
            isPrimitive = false;
            obj = std::make_unique<CubeObject>(1.0f);
            if (meshPath.empty())
            {
                // No modelPath specified, skip this model node
                LOG_TO_CONSOLE(L"SceneManager: model node '" + std::wstring(node.name.begin(), node.name.end()) +
                                   L"' has no modelPath, using placeholder",
                               L"WARNING");
            }
        }
        else
        {
            LOG_TO_CONSOLE(L"SceneManager: Unknown node type '" + std::wstring(node.type.begin(), node.type.end()) +
                               L"', skipping",
                           L"WARNING");
            m_objects.push_back(nullptr);
            continue;
        }

        if (obj)
        {
            HRESULT hr = obj->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
            if (SUCCEEDED(hr))
            {
                if (!isPrimitive)
                    LoadOrPlaceholderMesh(*obj->GetMesh(), m_graphics->GetDevice(), m_graphics->GetContext(), meshPath);
                obj->SetPosition(node.position);
                // SceneNode rotations are authored in degrees; GameObject
                // stores radians (XMMatrixRotationRollPitchYaw input).
                constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
                obj->SetRotation(
                    {node.rotation.x * kDegToRad, node.rotation.y * kDegToRad, node.rotation.z * kDegToRad});
                obj->SetScale(node.scale);
                obj->SetName(node.name);
                obj->SetMaterialPath(node.materialPath);
                m_objects.push_back(std::move(obj));
            }
            else
            {
                m_objects.push_back(nullptr);
            }
        }
    }
}

// ============================================================================
// Legacy .scene loader (preserved for backward compatibility)
// ============================================================================

bool SceneManager::LoadCustom(const std::wstring& path)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"SceneManager::LoadCustom called. path=" + path, L"OPERATION");

    std::ifstream file{std::filesystem::path(path)};
    if (!file.is_open())
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"SceneManager: Could not open scene file: " + path, L"ERROR");
        return false;
    }

    // Peek at the first non-empty, non-comment line to detect format
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    // SaveScene writes the versioned text format for both .json and .scene.
    // Route its own .scene output back through the matching loader before
    // trying either older scene syntax; otherwise the legacy object parser
    // silently discards authored camera, spawn, material, and property data.
    constexpr char kSerializedHeader[] = "# SparkEngine Scene v1.0";
    constexpr size_t kHeaderLength = sizeof(kSerializedHeader) - 1;
    if (content.compare(0, kHeaderLength, kSerializedHeader) == 0 &&
        (content.size() == kHeaderLength || content[kHeaderLength] == '\r' || content[kHeaderLength] == '\n'))
        return LoadJSON(path);

    bool isINIFormat = (content.contains("[Scene]") || content.contains("[Object]") || content.contains("[Camera]") ||
                        content.contains("[SpawnPoint]"));

    if (isINIFormat)
    {
        // Parse into isolated state.  A malformed field invalidates the whole
        // candidate; this prevents a valid prefix from being published as a
        // partly loaded scene.
        SceneMetadata stagedMetadata{};
        std::vector<SceneNode> stagedNodes;
        std::istringstream ss(content);
        std::string line;
        std::string currentSection;
        SceneNode currentNode;
        bool hasNode = false;
        bool nodeHasPosition = false;
        bool nodeInvalid = false;
        bool parseError = false;

        auto trim = [](std::string value)
        {
            const auto first = value.find_first_not_of(" \t\r");
            if (first == std::string::npos)
                return std::string{};
            const auto last = value.find_last_not_of(" \t\r");
            return value.substr(first, last - first + 1);
        };
        auto parseVector = [&](const std::string& value, DirectX::XMFLOAT3& output)
        {
            std::string normalized = value;
            const auto comment = normalized.find('#');
            if (comment != std::string::npos)
                normalized.erase(comment);
            for (char& c : normalized)
                if (c == ',')
                    c = ' ';
            std::istringstream values(normalized);
            DirectX::XMFLOAT3 parsed{};
            if (!(values >> parsed.x >> parsed.y >> parsed.z))
                return false;
            values >> std::ws;
            if (!values.eof() || !IsFinite(parsed))
                return false;
            output = parsed;
            return true;
        };

        auto flushNode = [&]()
        {
            if (!hasNode)
                return;
            const bool requiresPosition = currentNode.type == "Camera" || currentNode.type == "SpawnPoint";
            if (nodeInvalid || currentNode.type.empty() || (requiresPosition && !nodeHasPosition))
            {
                parseError = true;
            }
            else
            {
                if (currentNode.name.empty())
                    currentNode.name = currentNode.type + "_" + std::to_string(stagedNodes.size());
                if (currentNode.type == "SpawnPoint")
                {
                    const auto tag = currentNode.properties.find("tag");
                    if (tag == currentNode.properties.end() || tag->second.empty())
                    {
                        parseError = true;
                    }
                }
                stagedNodes.push_back(std::move(currentNode));
            }
            currentNode = SceneNode{};
            hasNode = false;
            nodeHasPosition = false;
            nodeInvalid = false;
        };

        while (std::getline(ss, line))
        {
            // Trim whitespace
            line = trim(line);
            if (line.empty() || line[0] == '#' || line[0] == ';')
                continue;

            // Section header
            if (line.front() == '[')
            {
                if (line.back() != ']')
                {
                    parseError = true;
                    continue;
                }
                flushNode();
                currentSection = trim(line.substr(1, line.size() - 2));

                if (currentSection == "Object" || currentSection == "Terrain" || currentSection == "SpawnPoint" ||
                    currentSection == "Camera")
                {
                    hasNode = true;
                    if (currentSection == "SpawnPoint" || currentSection == "Camera")
                        currentNode.type = currentSection;
                }
                continue;
            }

            // Key=Value pair
            auto eqPos = line.find('=');
            if (eqPos == std::string::npos)
            {
                if (hasNode || currentSection == "Scene")
                {
                    parseError = true;
                }
                continue;
            }
            std::string key = trim(line.substr(0, eqPos));
            std::string value = trim(line.substr(eqPos + 1));
            if (key.empty())
            {
                parseError = true;
                continue;
            }

            if (currentSection == "Scene")
            {
                if (key == "name")
                    stagedMetadata.sceneName = value;
                else if (key == "author")
                    stagedMetadata.author = value;
                else if (key == "version")
                    stagedMetadata.version = value;
                else if (key == "description")
                    stagedMetadata.description = value;
                else if (key == "ambientLight")
                {
                    XMFLOAT3 ambient;
                    if (!parseVector(value, ambient))
                    {
                        parseError = true;
                    }
                    else
                        stagedMetadata.ambientLightR = ambient.x, stagedMetadata.ambientLightG = ambient.y,
                        stagedMetadata.ambientLightB = ambient.z;
                }
                else if (key == "gravity")
                {
                    XMFLOAT3 grav;
                    if (!parseVector(value, grav))
                    {
                        parseError = true;
                    }
                    else
                        stagedMetadata.gravityX = grav.x, stagedMetadata.gravityY = grav.y,
                        stagedMetadata.gravityZ = grav.z;
                }
            }
            else if (hasNode)
            {
                if (key == "type")
                {
                    if (currentSection == "Camera" || currentSection == "SpawnPoint")
                        currentNode.properties[key] = value;
                    else
                        currentNode.type = value;
                }
                else if (key == "name")
                    currentNode.name = value;
                else if (key == "model")
                    currentNode.modelPath = value;
                else if (key == "position")
                {
                    nodeHasPosition = true;
                    nodeInvalid = !parseVector(value, currentNode.position) || nodeInvalid;
                }
                else if (key == "rotation")
                    nodeInvalid = !parseVector(value, currentNode.rotation) || nodeInvalid;
                else if (key == "scale")
                    nodeInvalid = !parseVector(value, currentNode.scale) || nodeInvalid;
                else if (key == "material")
                    currentNode.materialPath = value;
                else
                    currentNode.properties[key] = value;
            }
        }
        flushNode();

        if (parseError || stagedNodes.empty() || !ValidateAndRebuildHierarchy(stagedNodes))
            return false;
        std::unordered_map<std::string, int> stagedNameIndex;
        for (int i = 0; i < static_cast<int>(stagedNodes.size()); ++i)
            if (!stagedNameIndex.emplace(stagedNodes[static_cast<size_t>(i)].name, i).second)
                return false;
        Clear();
        m_metadata = std::move(stagedMetadata);
        m_sceneNodes = std::move(stagedNodes);
        m_nodeNameIndex = std::move(stagedNameIndex);
        InstantiateNodes();
    }
    else
    {
        // Legacy space-delimited format: Type X Y Z [params...]
        if (!m_graphics || !m_graphics->GetDevice() || !m_graphics->GetContext())
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"SceneManager: Legacy object format requires a graphics device/context",
                                     L"ERROR");
            return false;
        }
        std::vector<SceneNode> stagedNodes;
        std::vector<std::unique_ptr<GameObject>> stagedObjects;
        std::istringstream ss(content);
        std::string line;
        int lineNum = 0;

        while (std::getline(ss, line))
        {
            ++lineNum;
            // Trim
            const auto comment = line.find('#');
            if (comment != std::string::npos)
                line.erase(comment);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
                line.pop_back();
            if (line.empty())
                continue;

            std::istringstream ls(line);
            std::string type;
            ls >> type;
            float x = 0, y = 0, z = 0;
            if (type.empty() || !(ls >> x >> y >> z) || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
                return false;
            ls >> std::ws;

            std::unique_ptr<GameObject> obj;

            auto finiteFloat = [&ls](float& value) { return bool(ls >> value) && std::isfinite(value); };

            if (type == "Cube")
            {
                float size = 1.0f;
                if (ls.peek() != EOF && !finiteFloat(size))
                    return false;
                obj = std::make_unique<CubeObject>(size);
            }
            else if (type == "Plane")
            {
                float width = 10.0f, depth = 10.0f;
                if (ls.peek() != EOF && (!finiteFloat(width) || !finiteFloat(depth)))
                    return false;
                obj = std::make_unique<PlaneObject>(width, depth);
            }
            else if (type == "Sphere")
            {
                float radius = 0.5f;
                int slices = 16, stacks = 16;
                if (ls.peek() != EOF &&
                    (!finiteFloat(radius) || !(ls >> slices) || !(ls >> stacks) || slices <= 0 || stacks <= 0))
                    return false;
                obj = std::make_unique<SphereObject>(radius, slices, stacks);
            }
            else if (type == "Pyramid")
            {
                float size = 1.0f;
                if (ls.peek() != EOF && !finiteFloat(size))
                    return false;
                obj = std::make_unique<PyramidObject>(size);
            }
            else if (type == "Ramp")
            {
                float length = 2.0f, height = 1.0f;
                if (ls.peek() != EOF && (!finiteFloat(length) || !finiteFloat(height)))
                    return false;
                obj = std::make_unique<RampObject>(length, height);
            }
            else if (type == "Wall")
            {
                float width = 1.0f, height = 2.0f;
                if (ls.peek() != EOF && (!finiteFloat(width) || !finiteFloat(height)))
                    return false;
                obj = std::make_unique<WallObject>(width, height);
            }
            else
                return false;

            ls >> std::ws;
            if (!ls.eof())
                return false;

            HRESULT hr = obj->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
            if (FAILED(hr))
            {
                LOG_TO_CONSOLE(L"SceneManager: object Initialize failed on line " + std::to_wstring(lineNum), L"ERROR");
                return false;
            }

            std::string lowerType = type;
            std::transform(lowerType.begin(), lowerType.end(), lowerType.begin(), ::tolower);
            std::wstring meshPath = L"Assets\\Models\\" + std::wstring(lowerType.begin(), lowerType.end()) + L".obj";
            LoadOrPlaceholderMesh(*obj->GetMesh(), m_graphics->GetDevice(), m_graphics->GetContext(), meshPath);
            obj->SetPosition({x, y, z});
            SceneNode node;
            node.type = type;
            node.name = type + "_" + std::to_string(lineNum);
            node.position = {x, y, z};
            stagedNodes.push_back(std::move(node));
            stagedObjects.push_back(std::move(obj));
        }
        if (stagedNodes.empty())
            return false;
        Clear();
        m_sceneNodes = std::move(stagedNodes);
        m_objects = std::move(stagedObjects);
        m_nodeNameIndex.clear();
        for (int i = 0; i < static_cast<int>(m_sceneNodes.size()); ++i)
            m_nodeNameIndex.emplace(m_sceneNodes[static_cast<size_t>(i)].name, i);
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"SceneManager: Loaded " + std::to_wstring(m_sceneNodes.size()) + L" nodes", L"SUCCESS");
    return !m_sceneNodes.empty();
}

// ============================================================================
// Console Integration
// ============================================================================

std::string SceneManager::Console_ListNodes() const
{
    std::ostringstream ss;
    ss << "=== Scene: " << m_metadata.sceneName << " ===\n";
    ss << "Nodes: " << m_sceneNodes.size() << " | Objects: " << m_objects.size() << "\n";

    for (int i = 0; i < static_cast<int>(m_sceneNodes.size()); ++i)
    {
        const auto& node = m_sceneNodes[i];
        if (node.type.empty())
            continue;

        std::string indent(node.parentIndex >= 0 ? 4 : 2, ' ');
        ss << indent << "[" << i << "] " << node.name << " (" << node.type << ") " << "pos=(" << node.position.x << ","
           << node.position.y << "," << node.position.z << ")\n";
    }
    return ss.str();
}

std::string SceneManager::Console_GetNodeInfo(int index) const
{
    const auto* node = GetNode(index);
    if (!node)
        return "Node not found: " + std::to_string(index);

    std::ostringstream ss;
    ss << "=== Node [" << index << "] ===\n"
       << "  Name: " << node->name << "\n"
       << "  Type: " << node->type << "\n"
       << "  Position: (" << node->position.x << ", " << node->position.y << ", " << node->position.z << ")\n"
       << "  Rotation: (" << node->rotation.x << ", " << node->rotation.y << ", " << node->rotation.z << ")\n"
       << "  Scale: (" << node->scale.x << ", " << node->scale.y << ", " << node->scale.z << ")\n"
       << "  Parent: " << node->parentIndex << "\n"
       << "  Children: " << node->childIndices.size() << "\n";
    return ss.str();
}

bool SceneManager::Console_MoveNode(int index, float x, float y, float z)
{
    auto* node = GetNode(index);
    if (!node)
        return false;
    node->position = {x, y, z};
    m_dirty = true;
    return true;
}

bool SceneManager::Console_RenameNode(int index, const std::string& newName)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Scene, !newName.empty(),
                      "SceneManager::Console_RenameNode — newName must not be empty");
    auto* node = GetNode(index);
    if (!node)
        return false;

    // Reject if another node already has this name (per documentation contract)
    int existing = FindNode(newName);
    if (existing >= 0 && existing != index)
        return false;

    node->name = newName;
    m_dirty = true;
    return true;
}
