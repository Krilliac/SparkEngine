#include "../Core/Platform.h"
#include "SceneManager.h"
#include "Game/GameObject.h"
#include "Game/CubeObject.h"
#include "Game/PlaneObject.h"
#include "Game/SphereObject.h"
#include "Game/PlaceholderMesh.h"
#include "Game/PyramidObject.h"
#include "Game/RampObject.h"
#include "Game/WallObject.h"
#include "Graphics/GraphicsEngine.h"
#include "Utils/Assert.h"
#include "../Utils/ConsoleProcessManager.h"

#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <algorithm>

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

// Rate-limited logging macro to prevent console spam
#define LOG_TO_CONSOLE_RATE_LIMITED(msg, type)                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        static auto lastLogTime = std::chrono::steady_clock::now();                                                    \
        static int logCounter = 0;                                                                                     \
        auto now = std::chrono::steady_clock::now();                                                                   \
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastLogTime).count();                    \
        if (elapsed >= 10 || logCounter < 1)                                                                           \
        {                                                                                                              \
            Spark::ConsoleProcessManager::GetInstance().Log(msg, type);                                                \
            if (elapsed >= 10)                                                                                         \
            {                                                                                                          \
                lastLogTime = now;                                                                                     \
                logCounter = 0;                                                                                        \
            }                                                                                                          \
            else                                                                                                       \
            {                                                                                                          \
                logCounter++;                                                                                          \
            }                                                                                                          \
        }                                                                                                              \
    } while (0)

#define LOG_TO_CONSOLE(msg, type) LOG_TO_CONSOLE_RATE_LIMITED(msg, type)
#define LOG_TO_CONSOLE_IMMEDIATE(msg, type) Spark::ConsoleProcessManager::GetInstance().Log(msg, type)

SceneManager::SceneManager(GraphicsEngine* graphics, InputManager* input) : m_graphics(graphics), m_input(input)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"SceneManager constructed.", L"INFO");
    ASSERT_MSG(graphics != nullptr, "SceneManager: graphics is null");
    ASSERT_MSG(input != nullptr, "SceneManager: input is null");
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
    LOG_TO_CONSOLE_IMMEDIATE(L"SceneManager::LoadScene called. filepath=" + filepath, L"OPERATION");

    auto ext = std::filesystem::path(filepath).extension();
    bool loaded = false;

    if (ext == L".scene")
    {
        loaded = LoadCustom(filepath);
    }
    else if (ext == L".json")
    {
        loaded = LoadJSON(filepath);
    }
    else
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Scene file extension not recognized: " + filepath, L"WARNING");
        return false;
    }

    if (loaded)
    {
        m_currentFilePath = filepath;
        m_dirty = false;
        LOG_TO_CONSOLE_IMMEDIATE(L"Scene loaded: " + std::to_wstring(m_objects.size()) + L" objects, " +
                                     std::to_wstring(m_sceneNodes.size()) + L" nodes",
                                 L"SUCCESS");
    }
    return loaded;
}

bool SceneManager::SaveScene(const std::wstring& filepath) const
{
    LOG_TO_CONSOLE_IMMEDIATE(L"SceneManager::SaveScene called. filepath=" + filepath, L"OPERATION");
    bool saved = SaveJSON(filepath);
    if (saved)
    {
        const_cast<SceneManager*>(this)->m_currentFilePath = filepath;
        const_cast<SceneManager*>(this)->m_dirty = false;
    }
    return saved;
}

void SceneManager::LoadSceneAsync(const std::wstring& filepath, SceneLoadCallback callback)
{
    // Join any previous async load before starting a new one
    if (m_asyncLoadThread.joinable())
    {
        m_asyncLoadThread.join();
    }

    m_asyncLoadThread = std::thread(
        [this, filepath, callback]()
        {
            bool success = LoadScene(filepath);
            if (callback)
                callback(success, std::string(filepath.begin(), filepath.end()));
        });
}

// ============================================================================
// Scene Hierarchy
// ============================================================================

int SceneManager::AddNode(const SceneNode& node)
{
    int index = static_cast<int>(m_sceneNodes.size());
    m_sceneNodes.push_back(node);

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
    for (int i = 0; i < static_cast<int>(m_sceneNodes.size()); ++i)
    {
        if (m_sceneNodes[i].name == name)
            return i;
    }
    return -1;
}

// ============================================================================
// Prefab System
// ============================================================================

bool SceneManager::SavePrefab(int nodeIndex, const std::wstring& filepath) const
{
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(m_sceneNodes.size()))
        return false;

    std::ofstream file(WideToNarrow(filepath));
    if (!file.is_open())
        return false;

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
    std::ifstream file(WideToNarrow(filepath));
    if (!file.is_open())
        return -1;

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
    m_objects.clear();
    m_sceneNodes.clear();
    m_dirty = true;
}

std::vector<std::string> SceneManager::GetAvailableScenes(const std::wstring& directory) const
{
    std::vector<std::string> scenes;
    if (!std::filesystem::exists(directory))
        return scenes;

    for (const auto& entry : std::filesystem::directory_iterator(directory))
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
    std::ifstream file(WideToNarrow(path));
    if (!file.is_open())
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"SceneManager: Cannot open JSON scene: " + path, L"ERROR");
        return false;
    }

    // Read entire file
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    // Simple text parser for scene format
    // Full format:   type name px py pz rx ry rz sx sy sz parentIndex
    // Legacy format: type name px py pz
    Clear();

    std::istringstream ss(content);
    std::string line;
    while (std::getline(ss, line))
    {
        if (line.empty() || line[0] == '#' || line[0] == '/' || line[0] == '{' || line[0] == '}')
            continue;

        std::istringstream ls(line);
        SceneNode node;
        ls >> node.type >> node.name >> node.position.x >> node.position.y >> node.position.z;

        // Try to parse extended fields (rotation, scale, parentIndex)
        ls >> node.rotation.x >> node.rotation.y >> node.rotation.z >> node.scale.x >> node.scale.y >> node.scale.z >>
            node.parentIndex;
        // If extended fields aren't present, defaults are fine (rotation=0, scale=1, parent=-1)

        if (!node.type.empty())
            AddNode(node);
    }

    InstantiateNodes();
    return !m_sceneNodes.empty();
}

bool SceneManager::SaveJSON(const std::wstring& path) const
{
    std::ofstream file(WideToNarrow(path));
    if (!file.is_open())
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"SceneManager: Cannot save scene: " + path, L"ERROR");
        return false;
    }

    file << "# SparkEngine Scene v1.0\n";
    file << "# name: " << m_metadata.sceneName << "\n";
    file << "# gravity: " << m_metadata.gravityX << " " << m_metadata.gravityY << " " << m_metadata.gravityZ << "\n";
    file << "\n";

    for (size_t i = 0; i < m_sceneNodes.size(); ++i)
    {
        const auto& node = m_sceneNodes[i];
        if (node.type.empty())
            continue; // Skip tombstoned nodes

        file << node.type << " " << node.name << " " << std::fixed << std::setprecision(3) << node.position.x << " "
             << node.position.y << " " << node.position.z << " " << node.rotation.x << " " << node.rotation.y << " "
             << node.rotation.z << " " << node.scale.x << " " << node.scale.y << " " << node.scale.z << " "
             << node.parentIndex << "\n";
    }

    file.close();
    LOG_TO_CONSOLE_IMMEDIATE(L"Scene saved: " + std::to_wstring(m_sceneNodes.size()) + L" nodes", L"SUCCESS");
    return true;
}

void SceneManager::InstantiateNodes()
{
    if (!m_graphics || !m_graphics->GetDevice() || !m_graphics->GetContext())
        return;

    for (const auto& node : m_sceneNodes)
    {
        if (node.type.empty())
            continue;

        std::unique_ptr<GameObject> obj;

        if (node.type == "Cube" || node.type == "cube")
            obj = std::make_unique<CubeObject>(node.scale.x);
        else if (node.type == "Plane" || node.type == "plane")
            obj = std::make_unique<PlaneObject>(node.scale.x, node.scale.z);
        else if (node.type == "Sphere" || node.type == "sphere")
            obj = std::make_unique<SphereObject>(node.scale.x * 0.5f, 16, 16);
        else if (node.type == "Pyramid" || node.type == "pyramid")
            obj = std::make_unique<PyramidObject>(node.scale.x);
        else if (node.type == "Ramp" || node.type == "ramp")
            obj = std::make_unique<RampObject>(node.scale.x, node.scale.y);
        else if (node.type == "Wall" || node.type == "wall")
            obj = std::make_unique<WallObject>(node.scale.x, node.scale.y);
        else
            continue;

        if (obj)
        {
            HRESULT hr = obj->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
            if (SUCCEEDED(hr))
            {
                std::wstring wtype(node.type.begin(), node.type.end());
                LoadOrPlaceholderMesh(*obj->GetMesh(), m_graphics->GetDevice(), m_graphics->GetContext(),
                                      L"Assets\\Models\\" + wtype + L".obj");
                obj->SetPosition(node.position);
                obj->SetRotation(node.rotation);
                obj->SetName(node.name);
                m_objects.push_back(std::move(obj));
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
    if (!m_graphics || !m_graphics->GetDevice() || !m_graphics->GetContext())
    {
        ASSERT_MSG(false, "SceneManager: Graphics device/context is null");
        return false;
    }

    Clear();

    std::wifstream file(WideToNarrow(path));
    if (!file.is_open())
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"SceneManager: Could not open scene file: " + path, L"ERROR");
        return false;
    }

    std::wstring line;
    int lineNum = 0;
    while (std::getline(file, line))
    {
        ++lineNum;
        if (line.empty() || line[0] == L'#')
            continue;

        std::wstringstream ss(line);
        std::wstring type;
        ss >> type;
        float x = 0, y = 0, z = 0;
        ss >> x >> y >> z;

        std::unique_ptr<GameObject> obj;

        if (type == L"Cube")
        {
            float size;
            ss >> size;
            obj = std::make_unique<CubeObject>(size);
        }
        else if (type == L"Plane")
        {
            float width, depth;
            ss >> width >> depth;
            obj = std::make_unique<PlaneObject>(width, depth);
        }
        else if (type == L"Sphere")
        {
            float radius;
            int slices, stacks;
            ss >> radius >> slices >> stacks;
            obj = std::make_unique<SphereObject>(radius, slices, stacks);
        }
        else if (type == L"Pyramid")
        {
            float size;
            ss >> size;
            obj = std::make_unique<PyramidObject>(size);
        }
        else if (type == L"Ramp")
        {
            float length, height;
            ss >> length >> height;
            obj = std::make_unique<RampObject>(length, height);
        }
        else if (type == L"Wall")
        {
            float width, height;
            ss >> width >> height;
            obj = std::make_unique<WallObject>(width, height);
        }
        else
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"SceneManager: Unknown type on line " + std::to_wstring(lineNum) + L": " + type,
                                     L"ERROR");
            continue;
        }

        ASSERT(obj);
        HRESULT hr = obj->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
        ASSERT_MSG(SUCCEEDED(hr), "SceneManager: object Initialize failed");
        LoadOrPlaceholderMesh(*obj->GetMesh(), m_graphics->GetDevice(), m_graphics->GetContext(),
                              L"Assets\\Models\\" + type + L".obj");
        obj->SetPosition({x, y, z});
        m_objects.push_back(std::move(obj));

        // Also track in scene hierarchy
        SceneNode node;
        node.type = std::string(type.begin(), type.end());
        node.name = node.type + "_" + std::to_string(lineNum);
        node.position = {x, y, z};
        m_sceneNodes.push_back(node);
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"SceneManager: Loaded " + std::to_wstring(m_objects.size()) + L" objects", L"SUCCESS");
    return !m_objects.empty();
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
    auto* node = GetNode(index);
    if (!node)
        return false;
    node->name = newName;
    m_dirty = true;
    return true;
}
