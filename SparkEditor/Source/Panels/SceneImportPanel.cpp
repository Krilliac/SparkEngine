/**
 * @file SceneImportPanel.cpp
 * @brief One-way import of game INI .scene files into the editor's live ECS World (W10)
 * @author Spark Engine Team
 * @date 2026
 *
 * The parser is a standalone re-implementation of the INI dialect read by
 * SceneManager::LoadCustom (engine) and TFWorldCollision::ParseScene (game
 * module): '#'/';' comments, [Section] headers, key=value pairs, and
 * comma-separated float triples parsed with strtof. No game code is linked.
 *
 * Import execution follows the W9 SceneEditTools pattern: one LambdaCommand
 * through Spark::Editor::CommandHistory holding a shared created-ids vector;
 * undo destroys the entities but KEEPS the ids so redo recreates the exact
 * same identifiers via registry.create(hint) — later commands that captured
 * those ids keep resolving across undo/redo cycles. The raw ::World* capture
 * is safe because EditorUI::SwapWorld() clears the CommandHistory BEFORE
 * freeing the old World.
 *
 * Contains: construction/lifecycle, discovery, parsing, and import execution.
 * The ImGui drawing lives in the sibling SceneImportPanelDrawing.cpp.
 */

#include "SceneImportPanel.h"

#include "../CommandHistory.h"
#include "../Core/EditorUI.h"
#include "Engine/ECS/Components.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>

namespace fs = std::filesystem;

namespace SparkEditor
{

    namespace
    {
        /// @brief Parse "a,b,c" into out[3] (strtof, same as TFWorldCollision).
        ///        Missing/malformed components keep their prior values.
        void ParseFloat3(const std::string& value, float out[3])
        {
            const char* c = value.c_str();
            char* end = nullptr;
            for (int i = 0; i < 3; ++i)
            {
                out[i] = std::strtof(c, &end);
                if (end == c)
                    return; // malformed tail: keep defaults for the rest
                c = end;
                while (*c == ',' || *c == ' ')
                    ++c;
            }
        }

        /// @brief The mesh path used for cube primitives. The file does not exist;
        ///        WorldMeshCache::GetOrLoad -> LoadOrPlaceholderMesh falls back to
        ///        Mesh::CreateCube(1.0f) — the same centered unit cube the game
        ///        instantiates for cube [Object]s — so a scaled cube looks
        ///        identical in the editor viewport and the runtime.
        constexpr const char* kCubeMeshPath = "Assets/Models/cube.obj";
    } // namespace

    SceneImportPanel::SceneImportPanel() : EditorPanel("Scene Import", "scene_import_panel")
    {
        m_category = PanelCategory::Tool;
    }

    bool SceneImportPanel::Initialize()
    {
        // The editor normally runs with the repo root as cwd, but probe a few
        // parents so the panel also works when launched from a build output
        // directory (same probe as BasicMaterialEditorPanel).
        m_assetsPrefix.clear();
        for (const char* prefix : {"", "../", "../../", "../../../"})
        {
            std::error_code ec;
            if (fs::exists(fs::path(prefix) / "Assets" / "Scenes", ec))
            {
                m_assetsPrefix = prefix;
                break;
            }
        }

        ScanSceneFiles();
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "SceneImportPanel: found %d .scene file(s) under '%sAssets/Scenes'",
                       static_cast<int>(m_sceneFiles.size()), m_assetsPrefix.c_str());
        return true;
    }

    void SceneImportPanel::Update(float /*deltaTime*/) {}

    void SceneImportPanel::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Shutting down Scene Import panel");
    }

    // ========================================================================
    // Discovery
    // ========================================================================

    void SceneImportPanel::ScanSceneFiles()
    {
        m_sceneFiles.clear();
        m_selected = -1;
        m_previewValid = false;

        const fs::path sceneRoot = fs::path(m_assetsPrefix) / "Assets" / "Scenes";
        std::error_code ec;
        if (!fs::exists(sceneRoot, ec))
            return;

        fs::recursive_directory_iterator it(sceneRoot, fs::directory_options::skip_permission_denied, ec);
        const fs::recursive_directory_iterator end;
        for (; !ec && it != end; it.increment(ec))
        {
            std::error_code fec;
            if (!it->is_regular_file(fec))
                continue;
            if (it->path().extension().string() != ".scene")
                continue;

            SceneFileEntry entry;
            entry.diskPath = it->path().generic_string();
            const fs::path rel = fs::relative(it->path(), sceneRoot, fec);
            entry.displayPath = fec ? it->path().filename().generic_string() : rel.generic_string();
            m_sceneFiles.push_back(std::move(entry));
        }

        std::sort(m_sceneFiles.begin(), m_sceneFiles.end(),
                  [](const SceneFileEntry& a, const SceneFileEntry& b) { return a.displayPath < b.displayPath; });
    }

    // ========================================================================
    // Parse (standalone INI reader — mirrors the game's dialect)
    // ========================================================================

    bool SceneImportPanel::ParseSceneFile(const std::string& diskPath, ParsedScene& out) const
    {
        out = ParsedScene{};
        out.diskPath = diskPath;

        std::ifstream file(diskPath);
        if (!file.is_open())
            return false;

        std::string line;
        std::string currentSection;
        SceneObjectRecord current;
        bool inNode = false;

        auto flushNode = [&]()
        {
            if (inNode)
            {
                if (current.type == "cube" || current.type == "Cube" || current.type == "model" ||
                    current.type == "Model")
                {
                    if (current.name.empty())
                        current.name = current.type + "_" + std::to_string(out.objects.size());
                    out.objects.push_back(current);
                }
                else
                {
                    // Honest skip accounting: spawnpoints, terrain, unknown types.
                    out.skippedTypes.push_back(current.type.empty() ? "<no type> [" + currentSection + "]"
                                                                    : current.type);
                }
            }
            current = SceneObjectRecord{};
            inNode = false;
        };

        while (std::getline(file, line))
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            if (line.empty() || line[0] == '#' || line[0] == ';')
                continue;

            if (line.front() == '[' && line.back() == ']')
            {
                flushNode();
                currentSection = line.substr(1, line.size() - 2);
                // Every non-[Scene] section is a node candidate so unknown
                // sections are reported as skips instead of vanishing.
                inNode = (currentSection != "Scene");
                continue;
            }

            const size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            const std::string key = line.substr(0, eq);
            const std::string value = line.substr(eq + 1);

            if (currentSection == "Scene")
            {
                if (key == "name")
                    out.sceneName = value;
                continue;
            }
            if (!inNode)
                continue;

            if (key == "type")
                current.type = value;
            else if (key == "name")
                current.name = value;
            else if (key == "model")
                current.model = value;
            else if (key == "material")
                current.material = value;
            else if (key == "position")
                ParseFloat3(value, current.position);
            else if (key == "rotation")
                ParseFloat3(value, current.rotationDeg);
            else if (key == "scale")
                ParseFloat3(value, current.scale);
            // other keys (tag, priority, tf* terrain params) are intentionally ignored
        }
        flushNode();

        // Unresolved model paths (rendered as placeholder cubes until fixed).
        for (const SceneObjectRecord& record : out.objects)
        {
            if (record.model.empty())
                continue; // cubes: placeholder mesh is intentional
            std::error_code ec;
            if (!fs::exists(fs::path(m_assetsPrefix) / record.model, ec))
                out.unresolvedModels.push_back(record.model);
        }
        std::sort(out.unresolvedModels.begin(), out.unresolvedModels.end());
        out.unresolvedModels.erase(std::unique(out.unresolvedModels.begin(), out.unresolvedModels.end()),
                                   out.unresolvedModels.end());
        return true;
    }

    // ========================================================================
    // Import (ONE undoable command)
    // ========================================================================

    void SceneImportPanel::ImportParsed(const ParsedScene& parsed, const std::string& displayPath)
    {
        ::World* world = m_editorUI ? m_editorUI->GetWorld() : nullptr;
        if (!world || parsed.objects.empty())
            return;

        // Shared between Execute/Undo (W9 SceneEditTools pattern): the entities
        // created by the last Execute. Undo destroys them but KEEPS the ids so
        // Redo recreates the exact same identifiers via create(hint).
        auto records = std::make_shared<std::vector<SceneObjectRecord>>(parsed.objects);
        auto created = std::make_shared<std::vector<::EntityID>>();

        // Raw World pointer is safe here: SwapWorld() clears the CommandHistory
        // BEFORE freeing the old World, so this command can never outlive the
        // World it captured.
        ::World* worldPtr = world;

        const std::string fileName = fs::path(parsed.diskPath).filename().string();
        const std::string description = "Import Scene '" + fileName + "' (" + std::to_string(records->size()) +
                                        (records->size() == 1 ? " entity)" : " entities)");

        auto redo = [worldPtr, records, created]()
        {
            entt::registry& reg = worldPtr->GetRegistry();
            const std::vector<::EntityID> hints = *created; // empty on the first execute
            created->clear();
            created->reserve(records->size());

            size_t index = 0;
            for (const SceneObjectRecord& record : *records)
            {
                const ::EntityID hint = (index < hints.size()) ? hints[index] : static_cast<::EntityID>(entt::null);
                ++index;
                const ::EntityID entity = (hint == entt::null) ? reg.create() : reg.create(hint);
                created->push_back(entity);

                reg.emplace<::NameComponent>(entity, ::NameComponent{record.name});

                ::Transform& transform = reg.emplace<::Transform>(entity);
                transform.position = {record.position[0], record.position[1], record.position[2]};
                // .scene rotations are authored in degrees; the editor's
                // ::Transform stores Euler degrees — copy through unchanged.
                transform.rotation = {record.rotationDeg[0], record.rotationDeg[1], record.rotationDeg[2]};
                transform.scale = {record.scale[0], record.scale[1], record.scale[2]};

                ::MeshRenderer& meshRenderer = reg.emplace<::MeshRenderer>(entity);
                meshRenderer.meshPath = record.model.empty() ? std::string(kCubeMeshPath) : record.model;
                meshRenderer.materialPath = record.material;
            }
        };

        auto undo = [worldPtr, created]()
        {
            entt::registry& reg = worldPtr->GetRegistry();
            // Destroy in reverse creation order. The ids stay in 'created' as
            // create(hint) seeds for a subsequent Redo.
            for (auto it = created->rbegin(); it != created->rend(); ++it)
            {
                if (*it != entt::null && reg.valid(*it))
                {
                    worldPtr->DestroyEntity(*it);
                }
            }
        };

        Spark::Editor::CommandHistory::GetInstance().Execute(
            std::make_unique<Spark::Editor::LambdaCommand>(redo, undo, description));

        // Build the summary for the dialog + status line.
        m_lastImport = ImportSummary{};
        m_lastImport.sourcePath = displayPath;
        m_lastImport.imported = static_cast<int>(created->size());
        m_lastImport.skippedTotal = static_cast<int>(parsed.skippedTypes.size());
        m_lastImport.skippedCounts = AggregateSkipped(parsed.skippedTypes);
        m_lastImport.unresolvedModels = parsed.unresolvedModels;
        m_hasImported = true;
        m_openSummaryPopup = true;

        SPARK_LOG_INFO(Spark::LogCategory::Editor,
                       "SceneImportPanel: imported %d entities from '%s' (%d node(s) skipped, %d unresolved model(s))",
                       m_lastImport.imported, parsed.diskPath.c_str(), static_cast<int>(parsed.skippedTypes.size()),
                       static_cast<int>(parsed.unresolvedModels.size()));

        if (m_editorUI)
        {
            m_editorUI->ShowNotification(
                "Imported " + std::to_string(m_lastImport.imported) + " entities from " + fileName, "success", 3.0f);
        }
    }

    std::vector<std::string> SceneImportPanel::AggregateSkipped(const std::vector<std::string>& skippedTypes)
    {
        std::map<std::string, int> counts;
        for (const std::string& type : skippedTypes)
            ++counts[type];

        std::vector<std::string> out;
        out.reserve(counts.size());
        for (const auto& [type, count] : counts)
            out.push_back(type + " x" + std::to_string(count));
        return out;
    }

} // namespace SparkEditor
