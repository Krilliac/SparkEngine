/**
 * @file SearchPanel.cpp
 * @brief Implementation of the search panel
 * @author Spark Engine Team
 * @date 2025
 */

#include "SearchPanel.h"
#include "../Core/EditorIcons.h"
#include <imgui.h>
#include <algorithm>
#include <cctype>

namespace SparkEditor
{

    SearchPanel::SearchPanel() : EditorPanel("Search", "Search") {}

    bool SearchPanel::Initialize()
    {
        InitializeSampleData();
        m_isInitialized = true;
        return true;
    }

    void SearchPanel::Update(float /*deltaTime*/) {}

    void SearchPanel::Render()
    {
        if (!m_isVisible)
        {
            return;
        }

        if (!BeginPanel())
        {
            EndPanel();
            return;
        }

        RenderSearchBar();
        RenderFilterButtons();
        ImGui::Separator();

        if (m_currentQuery.empty())
        {
            RenderRecentSearches();
        }
        else
        {
            RenderResults();
        }

        EndPanel();
    }

    void SearchPanel::Shutdown() {}

    void SearchPanel::RenderSearchBar()
    {
        ImGui::SetNextItemWidth(-1);
        bool changed =
            ImGui::InputTextWithHint("##SearchInput", ICON_FA_SEARCH " Search entities, components, assets...",
                                     m_searchBuffer, sizeof(m_searchBuffer), ImGuiInputTextFlags_EnterReturnsTrue);

        if (changed || (ImGui::IsItemEdited()))
        {
            m_currentQuery = m_searchBuffer;
            if (!m_currentQuery.empty())
            {
                PerformSearch();
                if (changed)
                {
                    AddToRecentSearches(m_currentQuery);
                }
            }
            else
            {
                m_results.clear();
            }
        }

        // Navigate results with arrow keys
        if (ImGui::IsItemActive())
        {
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && !m_results.empty())
            {
                m_selectedResult = std::min(m_selectedResult + 1, static_cast<int>(m_results.size()) - 1);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
            {
                m_selectedResult = std::max(m_selectedResult - 1, 0);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Enter) && m_selectedResult >= 0 &&
                m_selectedResult < static_cast<int>(m_results.size()))
            {
                NavigateToResult(m_results[static_cast<size_t>(m_selectedResult)]);
            }
        }
    }

    void SearchPanel::RenderFilterButtons()
    {
        auto FilterButton = [this](const char* label, SearchFilter filter)
        {
            bool isActive = (m_filter == filter);
            if (isActive)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            }
            if (ImGui::SmallButton(label))
            {
                m_filter = filter;
                if (!m_currentQuery.empty())
                {
                    PerformSearch();
                }
            }
            if (isActive)
            {
                ImGui::PopStyleColor();
            }
        };

        FilterButton("All", SearchFilter::All);
        ImGui::SameLine();
        FilterButton(ICON_FA_CUBE " Entities", SearchFilter::Entities);
        ImGui::SameLine();
        FilterButton(ICON_FA_COG " Components", SearchFilter::Components);
        ImGui::SameLine();
        FilterButton(ICON_FA_FOLDER " Assets", SearchFilter::Assets);
    }

    void SearchPanel::RenderResults()
    {
        ImGui::Text("%zu results for \"%s\"", m_results.size(), m_currentQuery.c_str());
        ImGui::Spacing();

        ImGui::BeginChild("SearchResults", ImVec2(0, 0), false);

        for (size_t i = 0; i < m_results.size(); ++i)
        {
            const auto& result = m_results[i];
            bool isSelected = (static_cast<int>(i) == m_selectedResult);

            ImGui::PushID(static_cast<int>(i));

            // Icon based on type
            const char* icon = ICON_FA_CUBE;
            ImVec4 iconColor = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
            switch (result.type)
            {
            case SearchResultType::Entity:
                icon = ICON_FA_CUBE;
                iconColor = ImVec4(0.3f, 0.7f, 1.0f, 1.0f);
                break;
            case SearchResultType::Component:
                icon = ICON_FA_COG;
                iconColor = ImVec4(0.3f, 0.9f, 0.3f, 1.0f);
                break;
            case SearchResultType::Asset:
                icon = ICON_FA_FILE;
                iconColor = ImVec4(1.0f, 0.8f, 0.3f, 1.0f);
                break;
            }

            if (ImGui::Selectable(("##Result" + std::to_string(i)).c_str(), isSelected, 0, ImVec2(0, 36)))
            {
                m_selectedResult = static_cast<int>(i);
                NavigateToResult(result);
            }

            ImGui::SameLine(8);
            ImGui::TextColored(iconColor, "%s", icon);
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::Text("%s", result.name.c_str());
            ImGui::TextDisabled("%s", result.description.c_str());
            ImGui::EndGroup();

            ImGui::PopID();
        }

        ImGui::EndChild();
    }

    void SearchPanel::RenderRecentSearches()
    {
        if (m_recentSearches.empty())
        {
            ImGui::TextDisabled("Start typing to search across the scene and project");
            ImGui::Spacing();
            ImGui::TextDisabled("Tips:");
            ImGui::BulletText("Search for entity names");
            ImGui::BulletText("Search for component types (e.g., \"RigidBody\")");
            ImGui::BulletText("Search for asset files");
            return;
        }

        ImGui::Text(ICON_FA_SEARCH " Recent Searches");
        ImGui::Separator();

        for (size_t i = 0; i < m_recentSearches.size(); ++i)
        {
            if (ImGui::Selectable(m_recentSearches[i].c_str()))
            {
                std::string query = m_recentSearches[i];
                std::copy(query.begin(), query.begin() + std::min(query.size(), sizeof(m_searchBuffer) - 1),
                          m_searchBuffer);
                m_searchBuffer[std::min(query.size(), sizeof(m_searchBuffer) - 1)] = '\0';
                m_currentQuery = query;
                PerformSearch();
            }
        }

        ImGui::Spacing();
        if (ImGui::SmallButton("Clear History"))
        {
            m_recentSearches.clear();
        }
    }

    void SearchPanel::PerformSearch()
    {
        m_results.clear();
        m_selectedResult = -1;

        if (m_currentQuery.empty())
        {
            return;
        }

        std::string lowerQuery = m_currentQuery;
        std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(), ::tolower);

        // Search entities
        if (m_filter == SearchFilter::All || m_filter == SearchFilter::Entities)
        {
            for (const auto& entity : m_sampleEntities)
            {
                float score = CalculateRelevance(entity.name, lowerQuery);
                if (score > 0.0f)
                {
                    SearchResult result;
                    result.type = SearchResultType::Entity;
                    result.name = entity.name;
                    result.description = entity.hierarchyPath;
                    result.entityId = entity.id;
                    result.relevanceScore = score;
                    m_results.push_back(std::move(result));
                }
            }
        }

        // Search components
        if (m_filter == SearchFilter::All || m_filter == SearchFilter::Components)
        {
            for (const auto& entity : m_sampleEntities)
            {
                for (const auto& comp : entity.components)
                {
                    float score = CalculateRelevance(comp, lowerQuery);
                    if (score > 0.0f)
                    {
                        SearchResult result;
                        result.type = SearchResultType::Component;
                        result.name = comp;
                        result.description = "on " + entity.name;
                        result.entityId = entity.id;
                        result.relevanceScore = score;
                        m_results.push_back(std::move(result));
                    }
                }
            }
        }

        // Search assets
        if (m_filter == SearchFilter::All || m_filter == SearchFilter::Assets)
        {
            for (const auto& asset : m_sampleAssets)
            {
                float score = CalculateRelevance(asset.name, lowerQuery);
                if (score > 0.0f)
                {
                    SearchResult result;
                    result.type = SearchResultType::Asset;
                    result.name = asset.name;
                    result.description = asset.type + " - " + asset.path;
                    result.path = asset.path;
                    result.relevanceScore = score;
                    m_results.push_back(std::move(result));
                }
            }
        }

        // Sort by relevance
        std::sort(m_results.begin(), m_results.end(),
                  [](const SearchResult& a, const SearchResult& b) { return a.relevanceScore > b.relevanceScore; });

        // Limit results
        if (m_results.size() > 50)
        {
            m_results.resize(50);
        }
    }

    float SearchPanel::CalculateRelevance(const std::string& text, const std::string& query) const
    {
        std::string lowerText = text;
        std::transform(lowerText.begin(), lowerText.end(), lowerText.begin(), ::tolower);

        // Exact match
        if (lowerText == query)
        {
            return 1.0f;
        }

        // Starts with
        if (lowerText.find(query) == 0)
        {
            return 0.9f;
        }

        // Contains
        if (lowerText.find(query) != std::string::npos)
        {
            return 0.7f;
        }

        // Fuzzy: check if all characters in query appear in order in text
        size_t qi = 0;
        for (size_t ti = 0; ti < lowerText.size() && qi < query.size(); ++ti)
        {
            if (lowerText[ti] == query[qi])
            {
                ++qi;
            }
        }
        if (qi == query.size())
        {
            return 0.3f + 0.2f * (static_cast<float>(query.size()) / static_cast<float>(lowerText.size()));
        }

        return 0.0f;
    }

    void SearchPanel::AddToRecentSearches(const std::string& query)
    {
        // Remove if already exists
        auto it = std::find(m_recentSearches.begin(), m_recentSearches.end(), query);
        if (it != m_recentSearches.end())
        {
            m_recentSearches.erase(it);
        }

        // Add to front
        m_recentSearches.insert(m_recentSearches.begin(), query);

        // Trim
        if (m_recentSearches.size() > MAX_RECENT_SEARCHES)
        {
            m_recentSearches.resize(MAX_RECENT_SEARCHES);
        }
    }

    void SearchPanel::NavigateToResult(const SearchResult& result)
    {
        // In a full implementation, this would:
        // - Entity: Select entity in hierarchy, focus scene view on it
        // - Component: Select entity, expand component in inspector
        // - Asset: Navigate to asset in asset browser, preview it
    }

    void SearchPanel::InitializeSampleData()
    {
        // Sample entities for search demonstration
        m_sampleEntities.clear();
        // clang-format off
        auto e = [&](std::string name, uint64_t id, std::vector<std::string> components, std::string path) {
            m_sampleEntities.push_back({std::move(name), id, std::move(components), std::move(path)});
        };
        e("Player", 1, {"Transform", "Camera", "RigidBody", "Collider", "AudioSource"}, "Root/Player");
        e("MainCamera", 2, {"Transform", "Camera"}, "Root/MainCamera");
        e("DirectionalLight", 3, {"Transform", "Light"}, "Root/Lighting/DirectionalLight");
        e("PointLight_01", 4, {"Transform", "Light"}, "Root/Lighting/PointLight_01");
        e("PointLight_02", 5, {"Transform", "Light"}, "Root/Lighting/PointLight_02");
        e("SpotLight_01", 6, {"Transform", "Light"}, "Root/Lighting/SpotLight_01");
        e("Ground", 7, {"Transform", "MeshRenderer", "Collider"}, "Root/Environment/Ground");
        e("Wall_North", 8, {"Transform", "MeshRenderer", "Collider"}, "Root/Environment/Walls/Wall_North");
        e("Wall_South", 9, {"Transform", "MeshRenderer", "Collider"}, "Root/Environment/Walls/Wall_South");
        e("Wall_East", 10, {"Transform", "MeshRenderer", "Collider"}, "Root/Environment/Walls/Wall_East");
        e("Wall_West", 11, {"Transform", "MeshRenderer", "Collider"}, "Root/Environment/Walls/Wall_West");
        e("Crate_01", 12, {"Transform", "MeshRenderer", "RigidBody", "Collider"}, "Root/Props/Crate_01");
        e("Crate_02", 13, {"Transform", "MeshRenderer", "RigidBody", "Collider"}, "Root/Props/Crate_02");
        e("Barrel_01", 14, {"Transform", "MeshRenderer", "RigidBody", "Collider"}, "Root/Props/Barrel_01");
        e("WeaponPickup", 15, {"Transform", "MeshRenderer", "Collider", "AudioSource"}, "Root/Pickups/WeaponPickup");
        e("HealthPack", 16, {"Transform", "MeshRenderer", "Collider"}, "Root/Pickups/HealthPack");
        e("AmmoBox", 17, {"Transform", "MeshRenderer", "Collider"}, "Root/Pickups/AmmoBox");
        e("SpawnPoint_A", 18, {"Transform"}, "Root/GameLogic/SpawnPoint_A");
        e("SpawnPoint_B", 19, {"Transform"}, "Root/GameLogic/SpawnPoint_B");
        e("TriggerZone", 20, {"Transform", "Collider"}, "Root/GameLogic/TriggerZone");
        e("ParticleSmoke", 21, {"Transform", "ParticleSystem"}, "Root/Effects/ParticleSmoke");
        e("ParticleFire", 22, {"Transform", "ParticleSystem", "Light"}, "Root/Effects/ParticleFire");
        e("AmbientSound", 23, {"Transform", "AudioSource"}, "Root/Audio/AmbientSound");
        e("MusicPlayer", 24, {"Transform", "AudioSource"}, "Root/Audio/MusicPlayer");
        // clang-format on

        m_sampleAssets = {
            {"PlayerModel.fbx", "Model", "Assets/Models/PlayerModel.fbx"},
            {"Crate.fbx", "Model", "Assets/Models/Crate.fbx"},
            {"Barrel.fbx", "Model", "Assets/Models/Barrel.fbx"},
            {"WeaponPickup.fbx", "Model", "Assets/Models/WeaponPickup.fbx"},
            {"Ground.fbx", "Model", "Assets/Models/Ground.fbx"},
            {"Wall.fbx", "Model", "Assets/Models/Wall.fbx"},
            {"concrete_diffuse.png", "Texture", "Assets/Textures/concrete_diffuse.png"},
            {"concrete_normal.png", "Texture", "Assets/Textures/concrete_normal.png"},
            {"metal_diffuse.png", "Texture", "Assets/Textures/metal_diffuse.png"},
            {"wood_diffuse.png", "Texture", "Assets/Textures/wood_diffuse.png"},
            {"skybox_hdr.hdr", "Texture", "Assets/Textures/skybox_hdr.hdr"},
            {"gunshot.wav", "Audio", "Assets/Audio/gunshot.wav"},
            {"explosion.wav", "Audio", "Assets/Audio/explosion.wav"},
            {"footstep.wav", "Audio", "Assets/Audio/footstep.wav"},
            {"ambient_wind.ogg", "Audio", "Assets/Audio/ambient_wind.ogg"},
            {"music_combat.ogg", "Audio", "Assets/Audio/music_combat.ogg"},
            {"PBR_Standard.hlsl", "Shader", "Assets/Shaders/PBR_Standard.hlsl"},
            {"PostProcess.hlsl", "Shader", "Assets/Shaders/PostProcess.hlsl"},
            {"ParticleShader.hlsl", "Shader", "Assets/Shaders/ParticleShader.hlsl"},
            {"MainScene.spark", "Scene", "Assets/Scenes/MainScene.spark"},
            {"TestScene.spark", "Scene", "Assets/Scenes/TestScene.spark"},
        };
    }

} // namespace SparkEditor
