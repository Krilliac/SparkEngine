// Bounded reflected-editor -> legacy gameplay adapter. The outer LoadScene
// transaction retains the previous graph/objects until this entire load succeeds.
#include "SceneManager.h"
#include "SceneManager/ReflectedSceneSerializer.h"
#include "Engine/ECS/Components.h"
#include "Game/CubeObject.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/Mesh.h"
#include "Graphics/ProjectAssetPath.h"
#include "Utils/LogMacros.h"

#include <nlohmann_json.h>
#include <fstream>
#include <set>

bool SceneManager::LoadReflected(const std::wstring& filepath)
{
    const auto reject = [](const char* reason)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Scene, "Reflected gameplay scene rejected: %s", reason);
        return false;
    };
    const std::filesystem::path path(filepath);
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return reject("primary scene is unreadable");
    const auto size = input.tellg();
    if (size <= 0 || static_cast<uint64_t>(size) > Spark::kMaxSceneDocumentBytes)
        return reject("primary scene size is invalid");
    std::string text(static_cast<size_t>(size), '\0');
    input.seekg(0);
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (!input || input.peek() != std::char_traits<char>::eof())
        return reject("primary scene changed while reading");

    // Deliberately read only the primary bytes: LoadWorld's .bak recovery is
    // useful to an editor, but cannot stand in for a selected packaged scene.
    World world(World::EntityEventCleanupMode::Suppressed);
    std::string error;
    if (!Spark::DeserializeInto(world, text, Spark::SceneDeserializeMode::StrictRecovery, &error))
        return reject("primary scene is invalid or has unsupported reflected fields");
    const auto document = nlohmann::json::parse(text);
    for (const auto& entity : document.at("entities"))
    {
        for (const auto& component : entity.at("components"))
        {
            const auto type = component.at("type").get<std::string>();
            if (type != "Transform" && type != "MeshRenderer" && type != "Camera")
                return reject("unsupported component");
        }
    }

    auto project = path.parent_path();
    if (project.filename() == "Scenes")
        project = project.parent_path();
    const auto projectU8 = project.u8string();
    const std::string projectRoot(projectU8.begin(), projectU8.end());
    std::vector<SceneNode> nodes;
    std::vector<std::filesystem::path> meshes;
    std::set<std::string> names;
    size_t cameras = 0;
    for (auto entity : world.GetEntitiesWith<Transform>())
    {
        const auto* transform = world.GetComponent<Transform>(entity);
        const auto* name = world.GetComponent<NameComponent>(entity);
        const auto* mesh = world.GetComponent<MeshRenderer>(entity);
        const auto* camera = world.GetComponent<Camera>(entity);
        if (!name || name->name.empty() || !names.insert(name->name).second)
            return reject("entity names must be nonempty and unique");
        if (transform->parent != entt::null || !transform->children.empty())
            return reject("hierarchy is not supported by the gameplay adapter");
        if ((mesh == nullptr) == (camera == nullptr))
            return reject("each entity needs exactly one mesh or camera");
        SceneNode node;
        node.name = name->name;
        node.position = transform->position;
        node.rotation = transform->rotation;
        node.scale = transform->scale;
        if (mesh)
        {
            if (!mesh->materialPath.empty() || !mesh->visible || !mesh->castShadows || !mesh->receiveShadows ||
                mesh->emissive != 0.0f)
                return reject("only default mesh material and rendering flags are supported");
            const auto resolved = Spark::ResolveProjectAssetPath(projectRoot, mesh->meshPath);
            std::error_code ec;
            if (!resolved || resolved->nativePath.extension() != ".obj" ||
                !std::filesystem::is_regular_file(resolved->nativePath, ec) || ec)
                return reject("mesh must be an existing project-confined OBJ");
            node.type = "model";
            node.modelPath = resolved->cacheKey;
            meshes.push_back(resolved->nativePath);
        }
        else
        {
            if (!camera->isMainCamera || ++cameras != 1 || camera->fov < 10.0f || camera->fov > 170.0f ||
                camera->nearPlane < 0.01f || camera->nearPlane > 10.0f || camera->farPlane < 100.0f ||
                camera->farPlane > 10000.0f || camera->nearPlane >= camera->farPlane || node.rotation.x < -89.0f ||
                node.rotation.x > 89.0f || node.scale.x != 1.0f || node.scale.y != 1.0f || node.scale.z != 1.0f)
                return reject("requires one main perspective camera with supported clipping, pitch and unit scale");
            node.type = "Camera";
            node.properties = {{"projection", "perspective"},
                               {"isMain", "true"},
                               {"fov", std::to_string(camera->fov)},
                               {"nearPlane", std::to_string(camera->nearPlane)},
                               {"farPlane", std::to_string(camera->farPlane)}};
            meshes.emplace_back();
        }
        nodes.push_back(std::move(node));
    }
    if (nodes.size() != world.GetEntityCount() || cameras != 1 || nodes.size() < 2)
        return reject("requires a main camera and at least one mesh, all with transforms");

    std::vector<std::unique_ptr<GameObject>> objects;
    const bool rendering = m_graphics && m_graphics->GetDevice() && m_graphics->GetContext();
    for (size_t i = 0; i < nodes.size(); ++i)
    {
        if (!rendering || nodes[i].type == "Camera")
        {
            objects.push_back(nullptr);
            continue;
        }
        auto object = std::make_unique<CubeObject>(1.0f);
        if (FAILED(object->Initialize(m_graphics->GetDevice(), m_graphics->GetContext())))
            return reject("mesh object initialization failed");
            // Never use LoadOrPlaceholderMesh: a missing or malformed authored OBJ
            // must not be replaced by a plausible cube and reported as consumed.
#ifdef _WIN32
        const auto meshPath = meshes[i].wstring();
#else
        const auto meshU8 = meshes[i].u8string();
        const std::wstring meshPath(meshU8.begin(), meshU8.end());
#endif
        if (!object->GetMesh()->LoadFromFile(meshPath))
            return reject("authored OBJ could not be loaded");
        object->SetName(nodes[i].name);
        object->SetPosition(nodes[i].position);
        constexpr float degreesToRadians = 3.14159265358979323846f / 180.0f;
        const auto rotation = nodes[i].rotation;
        object->SetRotation(
            {rotation.x * degreesToRadians, rotation.y * degreesToRadians, rotation.z * degreesToRadians});
        object->SetScale(nodes[i].scale);
        objects.push_back(std::move(object));
    }
    // Commit only after the whole scene, every reference and every GPU mesh pass.
    Clear();
    m_metadata = SceneMetadata{};
    m_metadata.sceneName = "Reflected Startup";
    m_sceneNodes = std::move(nodes);
    m_objects = std::move(objects);
    m_nodeNameIndex.clear();
    for (size_t i = 0; i < m_sceneNodes.size(); ++i)
        m_nodeNameIndex[m_sceneNodes[i].name] = static_cast<int>(i);
    return true;
}
