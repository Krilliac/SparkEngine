/**
 * @file AssetDependencyPanel.cpp
 * @brief Asset dependency graph viewer for Spark Engine Editor
 * @author Spark Engine Team
 * @date 2025
 */

#include "AssetDependencyPanel.h"
#include "../Core/EditorIcons.h"
#include <imgui.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cmath>
#include <algorithm>
#include <stack>
#include <queue>
#include <random>
#include <cstdio>

namespace fs = std::filesystem;

namespace SparkEditor
{

    // =========================================================================
    // Construction / Destruction
    // =========================================================================

    AssetDependencyPanel::AssetDependencyPanel() : EditorPanel("Asset Dependencies", "asset_dependency_panel") {}

    AssetDependencyPanel::~AssetDependencyPanel() = default;

    // =========================================================================
    // EditorPanel interface
    // =========================================================================

    bool AssetDependencyPanel::Initialize()
    {
        std::cout << "Initializing Asset Dependency panel\n";
        m_filter = DependencyFilter{};
        m_graphOffset = ImVec2(0.0f, 0.0f);
        m_graphZoom = 1.0f;
        return true;
    }

    void AssetDependencyPanel::Update(float deltaTime)
    {
        if (m_needsRelayout)
        {
            ForceDirectedLayout(50);
            m_needsRelayout = false;
        }
    }

    void AssetDependencyPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            RenderToolbar();
            ImGui::Separator();

            if (m_showAnalysis)
            {
                RenderAnalysisResults();
                ImGui::Separator();
            }

            // Main layout: optional filter panel on the left, graph in the center, inspector on the right
            float filterWidth = m_showFilterPanel ? 200.0f : 0.0f;
            float inspectorWidth = m_showInspector ? 260.0f : 0.0f;

            if (m_showFilterPanel)
            {
                ImGui::BeginChild("FilterPanel", ImVec2(filterWidth, 0), true);
                RenderFilterPanel();
                ImGui::EndChild();
                ImGui::SameLine();
            }

            float centerWidth = ImGui::GetContentRegionAvail().x - inspectorWidth;
            if (centerWidth < 100.0f)
                centerWidth = 100.0f;

            ImGui::BeginChild("GraphArea", ImVec2(centerWidth, 0), true, ImGuiWindowFlags_NoScrollbar);
            if (m_showListView)
            {
                RenderAssetList();
            }
            else
            {
                RenderGraphView();
            }
            ImGui::EndChild();

            if (m_showInspector)
            {
                ImGui::SameLine();
                ImGui::BeginChild("InspectorPanel", ImVec2(0, 0), true);
                RenderInspector();
                ImGui::EndChild();
            }
        }
        EndPanel();
    }

    void AssetDependencyPanel::Shutdown()
    {
        std::cout << "Shutting down Asset Dependency panel\n";
        m_nodes.clear();
        m_edges.clear();
    }

    // =========================================================================
    // Public API
    // =========================================================================

    void AssetDependencyPanel::ScanProject(const std::string& projectPath)
    {
        m_nodes.clear();
        m_edges.clear();
        m_selectedAssetPath.clear();
        m_hoveredAssetPath.clear();

        if (!fs::exists(projectPath) || !fs::is_directory(projectPath))
        {
            std::cout << "AssetDependencyPanel: project path does not exist: " << projectPath << "\n";
            return;
        }

        // Recursively collect asset files
        for (const auto& entry : fs::recursive_directory_iterator(projectPath))
        {
            if (!entry.is_regular_file())
                continue;

            std::string relativePath = fs::relative(entry.path(), projectPath).string();
            // Normalize separators
            std::replace(relativePath.begin(), relativePath.end(), '\\', '/');

            AssetType type = ClassifyAsset(relativePath);
            if (type == AssetType::Unknown)
                continue;

            AssetNode node;
            node.path = relativePath;
            node.name = entry.path().stem().string();
            node.type = type;
            node.fileSizeBytes = static_cast<uint64_t>(entry.file_size());
            m_nodes[relativePath] = std::move(node);
        }

        BuildGraph();
        LayoutGraph();
        m_analysisStale = true;
    }

    DependencyAnalysis AssetDependencyPanel::RunAnalysis()
    {
        DetectCycles();
        ComputeDepths();

        m_lastAnalysis = DependencyAnalysis{};
        m_lastAnalysis.totalAssets = m_nodes.size();
        m_lastAnalysis.totalEdges = m_edges.size();

        size_t maxTransitiveDeps = 0;

        for (auto& [path, node] : m_nodes)
        {
            m_lastAnalysis.totalSizeBytes += node.fileSizeBytes;

            // Orphan: no dependents and not a Scene (scenes are roots)
            if (node.dependents.empty() && node.type != AssetType::Scene)
            {
                node.isOrphan = true;
                m_lastAnalysis.orphanedAssets++;
                m_lastAnalysis.orphanPaths.push_back(path);
            }
            else
            {
                node.isOrphan = false;
            }

            if (node.depthFromRoot >= 0 && static_cast<size_t>(node.depthFromRoot) > m_lastAnalysis.maxDependencyDepth)
            {
                m_lastAnalysis.maxDependencyDepth = static_cast<size_t>(node.depthFromRoot);
            }

            // Count transitive deps for heaviest asset
            size_t transitive = node.dependencies.size();
            if (transitive > maxTransitiveDeps)
            {
                maxTransitiveDeps = transitive;
                m_lastAnalysis.heaviestAsset = path;
            }
        }

        m_lastAnalysis.circularDependencies = m_lastAnalysis.cycles.size();
        m_analysisStale = false;
        return m_lastAnalysis;
    }

    void AssetDependencyPanel::SelectAsset(const std::string& assetPath)
    {
        ClearHighlights();
        m_selectedAssetPath = assetPath;

        auto it = m_nodes.find(assetPath);
        if (it != m_nodes.end())
        {
            it->second.isSelected = true;
            HighlightDependencyChain(assetPath);
        }
    }

    void AssetDependencyPanel::SetFilter(const DependencyFilter& filter)
    {
        m_filter = filter;
    }

    bool AssetDependencyPanel::ExportReport(const std::string& filePath, const std::string& format)
    {
        if (m_analysisStale)
        {
            RunAnalysis();
        }

        std::ofstream out(filePath);
        if (!out.is_open())
        {
            std::cout << "AssetDependencyPanel: failed to open export file: " << filePath << "\n";
            return false;
        }

        if (format == "csv")
        {
            // CSV header
            out << "Path,Name,Type,SizeBytes,DependencyCount,DependentCount,Depth,IsOrphan,IsInCycle\n";
            for (const auto& [path, node] : m_nodes)
            {
                out << "\"" << node.path << "\"," << "\"" << node.name << "\"," << "\"" << GetAssetTypeName(node.type)
                    << "\"," << node.fileSizeBytes << "," << node.dependencies.size() << "," << node.dependents.size()
                    << "," << node.depthFromRoot << "," << (node.isOrphan ? "true" : "false") << ","
                    << (node.isInCycle ? "true" : "false") << "\n";
            }
        }
        else
        {
            // JSON format
            out << "{\n";
            out << "  \"analysis\": {\n";
            out << "    \"totalAssets\": " << m_lastAnalysis.totalAssets << ",\n";
            out << "    \"totalEdges\": " << m_lastAnalysis.totalEdges << ",\n";
            out << "    \"orphanedAssets\": " << m_lastAnalysis.orphanedAssets << ",\n";
            out << "    \"circularDependencies\": " << m_lastAnalysis.circularDependencies << ",\n";
            out << "    \"maxDependencyDepth\": " << m_lastAnalysis.maxDependencyDepth << ",\n";
            out << "    \"heaviestAsset\": \"" << m_lastAnalysis.heaviestAsset << "\",\n";
            out << "    \"totalSizeBytes\": " << m_lastAnalysis.totalSizeBytes << "\n";
            out << "  },\n";

            out << "  \"assets\": [\n";
            bool first = true;
            for (const auto& [path, node] : m_nodes)
            {
                if (!first)
                    out << ",\n";
                first = false;

                out << "    {\n";
                out << "      \"path\": \"" << node.path << "\",\n";
                out << "      \"name\": \"" << node.name << "\",\n";
                out << "      \"type\": \"" << GetAssetTypeName(node.type) << "\",\n";
                out << "      \"sizeBytes\": " << node.fileSizeBytes << ",\n";
                out << "      \"depthFromRoot\": " << node.depthFromRoot << ",\n";
                out << "      \"isOrphan\": " << (node.isOrphan ? "true" : "false") << ",\n";
                out << "      \"isInCycle\": " << (node.isInCycle ? "true" : "false") << ",\n";

                out << "      \"dependencies\": [";
                for (size_t i = 0; i < node.dependencies.size(); i++)
                {
                    if (i > 0)
                        out << ", ";
                    out << "\"" << node.dependencies[i] << "\"";
                }
                out << "],\n";

                out << "      \"dependents\": [";
                for (size_t i = 0; i < node.dependents.size(); i++)
                {
                    if (i > 0)
                        out << ", ";
                    out << "\"" << node.dependents[i] << "\"";
                }
                out << "]\n";
                out << "    }";
            }
            out << "\n  ],\n";

            // Cycles
            out << "  \"cycles\": [\n";
            for (size_t ci = 0; ci < m_lastAnalysis.cycles.size(); ci++)
            {
                if (ci > 0)
                    out << ",\n";
                out << "    [";
                const auto& cycle = m_lastAnalysis.cycles[ci];
                for (size_t j = 0; j < cycle.size(); j++)
                {
                    if (j > 0)
                        out << ", ";
                    out << "\"" << cycle[j] << "\"";
                }
                out << "]";
            }
            out << "\n  ]\n";
            out << "}\n";
        }

        out.close();
        return true;
    }

    // =========================================================================
    // Render sub-sections
    // =========================================================================

    void AssetDependencyPanel::RenderToolbar()
    {
        if (ImGui::Button(ICON_FA_SEARCH " Scan"))
        {
            ScanProject("Assets");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Scan project assets and build dependency graph");

        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_COG " Analyze"))
        {
            RunAnalysis();
            m_showAnalysis = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Run dependency analysis (orphans, cycles, depth)");

        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_SYNC_ALT " Re-layout"))
        {
            m_needsRelayout = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Re-run force-directed layout");

        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_DOWNLOAD " Export"))
        {
            ImGui::OpenPopup("ExportPopup");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Export dependency report");

        if (ImGui::BeginPopup("ExportPopup"))
        {
            if (ImGui::MenuItem(ICON_FA_FILE_CODE " Export JSON"))
            {
                ExportReport("dependency_report.json", "json");
            }
            if (ImGui::MenuItem(ICON_FA_FILE " Export CSV"))
            {
                ExportReport("dependency_report.csv", "csv");
            }
            ImGui::EndPopup();
        }

        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();

        ImGui::Checkbox(ICON_FA_SLIDERS " Filters", &m_showFilterPanel);
        ImGui::SameLine();
        ImGui::Checkbox(ICON_FA_SLIDERS " Inspector", &m_showInspector);
        ImGui::SameLine();
        ImGui::Checkbox(ICON_FA_CHART_BAR " Analysis", &m_showAnalysis);
        ImGui::SameLine();
        ImGui::Checkbox(ICON_FA_LIST_ALT " List", &m_showListView);

        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
        ImGui::Text(ICON_FA_SITEMAP " Assets: %zu  Edges: %zu", m_nodes.size(), m_edges.size());
    }

    void AssetDependencyPanel::RenderGraphView()
    {
        HandleGraphInput();

        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        ImVec2 canvasSize = ImGui::GetContentRegionAvail();
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Background
        drawList->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                                IM_COL32(30, 30, 30, 255));

        // Grid lines
        float gridStep = 50.0f * m_graphZoom;
        if (gridStep > 5.0f)
        {
            float offsetX = fmodf(m_graphOffset.x * m_graphZoom, gridStep);
            float offsetY = fmodf(m_graphOffset.y * m_graphZoom, gridStep);

            for (float x = offsetX; x < canvasSize.x; x += gridStep)
            {
                drawList->AddLine(ImVec2(canvasPos.x + x, canvasPos.y),
                                  ImVec2(canvasPos.x + x, canvasPos.y + canvasSize.y), IM_COL32(50, 50, 50, 255));
            }
            for (float y = offsetY; y < canvasSize.y; y += gridStep)
            {
                drawList->AddLine(ImVec2(canvasPos.x, canvasPos.y + y),
                                  ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + y), IM_COL32(50, 50, 50, 255));
            }
        }

        // Clip to canvas
        drawList->PushClipRect(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y), true);

        RenderEdges();

        for (auto& [path, node] : m_nodes)
        {
            if (!PassesFilter(node))
                continue;
            RenderAssetNode(node);
        }

        drawList->PopClipRect();

        // Dummy to fill the area for input handling
        ImGui::Dummy(canvasSize);
    }

    void AssetDependencyPanel::RenderAssetNode(AssetNode& node)
    {
        ImVec2 screenPos = GraphToScreen(node.graphPosition);
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        float nodeWidth = 120.0f * m_graphZoom;
        float nodeHeight = 40.0f * m_graphZoom;
        ImVec2 nodeMin = ImVec2(screenPos.x - nodeWidth * 0.5f, screenPos.y - nodeHeight * 0.5f);
        ImVec2 nodeMax = ImVec2(screenPos.x + nodeWidth * 0.5f, screenPos.y + nodeHeight * 0.5f);

        // Determine colors
        ImVec4 typeColor = GetAssetTypeColor(node.type);
        ImU32 bgColor = IM_COL32(static_cast<int>(typeColor.x * 60), static_cast<int>(typeColor.y * 60),
                                 static_cast<int>(typeColor.z * 60), 200);
        ImU32 borderColor = IM_COL32(static_cast<int>(typeColor.x * 255), static_cast<int>(typeColor.y * 255),
                                     static_cast<int>(typeColor.z * 255), 255);

        if (node.isSelected)
        {
            bgColor = IM_COL32(60, 80, 120, 230);
            borderColor = IM_COL32(100, 160, 255, 255);
        }
        else if (node.isHighlighted)
        {
            bgColor = IM_COL32(80, 60, 20, 220);
            borderColor = IM_COL32(255, 200, 80, 255);
        }
        else if (node.isInCycle)
        {
            borderColor = IM_COL32(255, 60, 60, 255);
        }
        else if (node.isOrphan)
        {
            borderColor = IM_COL32(150, 150, 150, 150);
            bgColor = IM_COL32(40, 40, 40, 160);
        }

        // Node body
        drawList->AddRectFilled(nodeMin, nodeMax, bgColor, 6.0f * m_graphZoom);
        drawList->AddRect(nodeMin, nodeMax, borderColor, 6.0f * m_graphZoom, 0, 2.0f * m_graphZoom);

        // Label text
        float fontSize = 12.0f * m_graphZoom;
        if (fontSize > 6.0f)
        {
            const char* typeName = GetAssetTypeName(node.type);
            char label[128];
            snprintf(label, sizeof(label), "%s", node.name.c_str());

            ImVec2 textSize = ImGui::CalcTextSize(label);
            float scale = (fontSize / ImGui::GetFontSize());
            ImVec2 textPos = ImVec2(screenPos.x - textSize.x * scale * 0.5f,
                                    screenPos.y - textSize.y * scale * 0.5f - 2.0f * m_graphZoom);
            drawList->AddText(ImGui::GetFont(), fontSize, textPos, IM_COL32(240, 240, 240, 255), label);

            // Type name below
            float smallFont = 9.0f * m_graphZoom;
            if (smallFont > 5.0f)
            {
                ImVec2 typeSize = ImGui::CalcTextSize(typeName);
                float typeScale = (smallFont / ImGui::GetFontSize());
                ImVec2 typePos = ImVec2(screenPos.x - typeSize.x * typeScale * 0.5f, screenPos.y + 2.0f * m_graphZoom);
                drawList->AddText(ImGui::GetFont(), smallFont, typePos,
                                  IM_COL32(static_cast<int>(typeColor.x * 255), static_cast<int>(typeColor.y * 255),
                                           static_cast<int>(typeColor.z * 255), 200),
                                  typeName);
            }
        }

        // Hit testing for mouse interaction
        ImVec2 mousePos = ImGui::GetMousePos();
        bool hovered =
            (mousePos.x >= nodeMin.x && mousePos.x <= nodeMax.x && mousePos.y >= nodeMin.y && mousePos.y <= nodeMax.y);

        if (hovered)
        {
            m_hoveredAssetPath = node.path;

            // Tooltip
            ImGui::BeginTooltip();
            ImGui::Text("%s (%s)", node.name.c_str(), GetAssetTypeName(node.type));
            ImGui::Text("Path: %s", node.path.c_str());
            ImGui::Text("Size: %llu bytes", static_cast<unsigned long long>(node.fileSizeBytes));
            ImGui::Text("Dependencies: %zu", node.dependencies.size());
            ImGui::Text("Dependents: %zu", node.dependents.size());
            if (node.isOrphan)
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Orphan (unused)");
            if (node.isInCycle)
                ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "In circular dependency");
            ImGui::EndTooltip();

            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                SelectAsset(node.path);
            }

            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left) && m_selectedAssetPath == node.path)
            {
                ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
                node.graphPosition.x += delta.x / m_graphZoom;
                node.graphPosition.y += delta.y / m_graphZoom;
                ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
                m_isDraggingNode = true;
            }
        }
    }

    void AssetDependencyPanel::RenderEdges()
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        for (const auto& edge : m_edges)
        {
            auto fromIt = m_nodes.find(edge.fromPath);
            auto toIt = m_nodes.find(edge.toPath);
            if (fromIt == m_nodes.end() || toIt == m_nodes.end())
                continue;

            const auto& fromNode = fromIt->second;
            const auto& toNode = toIt->second;

            if (!PassesFilter(fromNode) || !PassesFilter(toNode))
                continue;

            ImVec2 fromScreen = GraphToScreen(fromNode.graphPosition);
            ImVec2 toScreen = GraphToScreen(toNode.graphPosition);

            // Bezier control points for a curved edge
            float dx = toScreen.x - fromScreen.x;
            float tangentStrength = std::min(std::abs(dx) * 0.5f, 80.0f * m_graphZoom);
            ImVec2 cp1 = ImVec2(fromScreen.x + tangentStrength, fromScreen.y);
            ImVec2 cp2 = ImVec2(toScreen.x - tangentStrength, toScreen.y);

            ImU32 edgeColor = IM_COL32(120, 120, 120, 100);
            float thickness = 1.5f * m_graphZoom;

            if (edge.isHighlighted)
            {
                edgeColor = IM_COL32(255, 200, 80, 200);
                thickness = 2.5f * m_graphZoom;
            }

            // Check if this edge connects nodes in a cycle
            if (fromNode.isInCycle && toNode.isInCycle)
            {
                edgeColor = IM_COL32(255, 80, 80, 180);
            }

            drawList->AddBezierCubic(fromScreen, cp1, cp2, toScreen, edgeColor, thickness);

            // Arrowhead at the target end
            float arrowSize = 6.0f * m_graphZoom;
            ImVec2 dir = ImVec2(toScreen.x - cp2.x, toScreen.y - cp2.y);
            float len = sqrtf(dir.x * dir.x + dir.y * dir.y);
            if (len > 0.0f)
            {
                dir.x /= len;
                dir.y /= len;
                ImVec2 perp = ImVec2(-dir.y, dir.x);
                ImVec2 arrowTip = toScreen;
                ImVec2 arrowLeft = ImVec2(arrowTip.x - dir.x * arrowSize + perp.x * arrowSize * 0.5f,
                                          arrowTip.y - dir.y * arrowSize + perp.y * arrowSize * 0.5f);
                ImVec2 arrowRight = ImVec2(arrowTip.x - dir.x * arrowSize - perp.x * arrowSize * 0.5f,
                                           arrowTip.y - dir.y * arrowSize - perp.y * arrowSize * 0.5f);
                drawList->AddTriangleFilled(arrowTip, arrowLeft, arrowRight, edgeColor);
            }
        }
    }

    void AssetDependencyPanel::RenderFilterPanel()
    {
        ImGui::Text(ICON_FA_SLIDERS " Filters");
        ImGui::Separator();

        char searchBuf[256];
        snprintf(searchBuf, sizeof(searchBuf), "%s", m_filter.searchFilter.c_str());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText(ICON_FA_SEARCH "##Search", searchBuf, sizeof(searchBuf)))
        {
            m_filter.searchFilter = searchBuf;
        }

        ImGui::Spacing();
        ImGui::Text("Asset Types:");
        ImGui::Checkbox(ICON_FA_IMAGE " Textures", &m_filter.showTextures);
        ImGui::Checkbox(ICON_FA_CUBE " Meshes", &m_filter.showMeshes);
        ImGui::Checkbox(ICON_FA_PALETTE " Materials", &m_filter.showMaterials);
        ImGui::Checkbox(ICON_FA_CODE " Shaders", &m_filter.showShaders);
        ImGui::Checkbox(ICON_FA_VOLUME_UP " Audio", &m_filter.showAudio);
        ImGui::Checkbox(ICON_FA_RUNNING " Animations", &m_filter.showAnimations);
        ImGui::Checkbox(ICON_FA_BOX " Prefabs", &m_filter.showPrefabs);
        ImGui::Checkbox(ICON_FA_MAP " Scenes", &m_filter.showScenes);
        ImGui::Checkbox(ICON_FA_FILE_CODE " Scripts", &m_filter.showScripts);
        ImGui::Checkbox(ICON_FA_FILE " Other", &m_filter.showOther);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Show Only:");
        ImGui::Checkbox("Orphans", &m_filter.showOrphansOnly);
        ImGui::Checkbox("Cycles", &m_filter.showCyclesOnly);

        ImGui::Spacing();
        ImGui::Text("Max Depth:");
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderInt("##MaxDepth", &m_filter.maxDepth, -1, 20, m_filter.maxDepth < 0 ? "Unlimited" : "%d");

        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Button("Reset Filters", ImVec2(-1, 0)))
        {
            m_filter = DependencyFilter{};
        }
    }

    void AssetDependencyPanel::RenderInspector()
    {
        ImGui::Text(ICON_FA_SLIDERS " Inspector");
        ImGui::Separator();

        if (m_selectedAssetPath.empty())
        {
            ImGui::TextWrapped("Select an asset node to inspect its dependencies.");
            return;
        }

        auto it = m_nodes.find(m_selectedAssetPath);
        if (it == m_nodes.end())
        {
            ImGui::Text("Asset not found.");
            return;
        }

        const auto& node = it->second;
        ImVec4 typeColor = GetAssetTypeColor(node.type);

        ImGui::TextColored(typeColor, "%s", GetAssetTypeName(node.type));
        ImGui::Text("%s", node.name.c_str());
        ImGui::TextDisabled("%s", node.path.c_str());

        ImGui::Spacing();
        ImGui::Separator();

        ImGui::Text("File Size: %llu bytes", static_cast<unsigned long long>(node.fileSizeBytes));
        ImGui::Text("Depth: %d", node.depthFromRoot);

        if (node.isOrphan)
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), ICON_FA_EXCLAMATION " Orphan (no dependents)");
        if (node.isInCycle)
            ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), ICON_FA_EXCLAMATION " Circular dependency");

        ImGui::Spacing();
        ImGui::Separator();

        // Dependencies (outgoing)
        if (ImGui::CollapsingHeader(ICON_FA_CHEVRON_RIGHT " Dependencies", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (node.dependencies.empty())
            {
                ImGui::TextDisabled("  (none)");
            }
            else
            {
                for (const auto& depPath : node.dependencies)
                {
                    auto depIt = m_nodes.find(depPath);
                    if (depIt != m_nodes.end())
                    {
                        ImVec4 depColor = GetAssetTypeColor(depIt->second.type);
                        if (ImGui::Selectable(depIt->second.name.c_str(), depPath == m_selectedAssetPath))
                        {
                            SelectAsset(depPath);
                        }
                        if (ImGui::IsItemHovered())
                        {
                            ImGui::BeginTooltip();
                            ImGui::TextColored(depColor, "%s", GetAssetTypeName(depIt->second.type));
                            ImGui::Text("%s", depPath.c_str());
                            ImGui::EndTooltip();
                        }
                    }
                    else
                    {
                        ImGui::TextDisabled("  %s (missing)", depPath.c_str());
                    }
                }
            }
        }

        // Dependents (incoming)
        if (ImGui::CollapsingHeader(ICON_FA_CHEVRON_LEFT " Dependents", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (node.dependents.empty())
            {
                ImGui::TextDisabled("  (none)");
            }
            else
            {
                for (const auto& refPath : node.dependents)
                {
                    auto refIt = m_nodes.find(refPath);
                    if (refIt != m_nodes.end())
                    {
                        if (ImGui::Selectable(refIt->second.name.c_str(), refPath == m_selectedAssetPath))
                        {
                            SelectAsset(refPath);
                        }
                    }
                    else
                    {
                        ImGui::TextDisabled("  %s (missing)", refPath.c_str());
                    }
                }
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Layout");
        ImGui::Text("Repulsion:");
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##Repulsion", &m_layoutRepulsion, 100.0f, 2000.0f);
        ImGui::Text("Attraction:");
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##Attraction", &m_layoutAttraction, 0.001f, 0.1f, "%.3f");
        ImGui::Text("Damping:");
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##Damping", &m_layoutDamping, 0.5f, 0.99f, "%.2f");
    }

    void AssetDependencyPanel::RenderAnalysisResults()
    {
        ImGui::Text(ICON_FA_CHART_BAR " Analysis Results");

        if (m_analysisStale)
        {
            ImGui::TextDisabled("Analysis is stale. Click 'Analyze' to update.");
            return;
        }

        ImGui::Columns(4, "AnalysisColumns", true);
        ImGui::Text("Total Assets");
        ImGui::Text("%zu", m_lastAnalysis.totalAssets);
        ImGui::NextColumn();
        ImGui::Text("Total Edges");
        ImGui::Text("%zu", m_lastAnalysis.totalEdges);
        ImGui::NextColumn();
        ImGui::Text("Orphans");
        ImGui::TextColored(m_lastAnalysis.orphanedAssets > 0 ? ImVec4(1.0f, 0.6f, 0.2f, 1.0f)
                                                             : ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                           "%zu", m_lastAnalysis.orphanedAssets);
        ImGui::NextColumn();
        ImGui::Text("Cycles");
        ImGui::TextColored(m_lastAnalysis.circularDependencies > 0 ? ImVec4(1.0f, 0.2f, 0.2f, 1.0f)
                                                                   : ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                           "%zu", m_lastAnalysis.circularDependencies);
        ImGui::Columns(1);

        ImGui::Text("Max Depth: %zu | Heaviest: %s | Total Size: %llu bytes", m_lastAnalysis.maxDependencyDepth,
                    m_lastAnalysis.heaviestAsset.c_str(),
                    static_cast<unsigned long long>(m_lastAnalysis.totalSizeBytes));

        // Show cycle details if any
        if (!m_lastAnalysis.cycles.empty() && ImGui::CollapsingHeader(ICON_FA_EXCLAMATION " Cycle Details"))
        {
            for (size_t ci = 0; ci < m_lastAnalysis.cycles.size(); ci++)
            {
                const auto& cycle = m_lastAnalysis.cycles[ci];
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Cycle %zu:", ci + 1);
                ImGui::SameLine();
                std::string cycleStr;
                for (size_t j = 0; j < cycle.size(); j++)
                {
                    if (j > 0)
                        cycleStr += " -> ";
                    // Extract just the filename for readability
                    auto slash = cycle[j].rfind('/');
                    cycleStr += (slash != std::string::npos) ? cycle[j].substr(slash + 1) : cycle[j];
                }
                cycleStr += " -> (back to start)";
                ImGui::TextWrapped("%s", cycleStr.c_str());
            }
        }
    }

    void AssetDependencyPanel::RenderAssetList()
    {
        ImGui::Text(ICON_FA_LIST_ALT " Asset List (%zu assets)", m_nodes.size());
        ImGui::Separator();

        // Column headers
        ImGui::Columns(5, "AssetListColumns", true);
        ImGui::SetColumnWidth(0, 200.0f);
        ImGui::SetColumnWidth(1, 80.0f);
        ImGui::SetColumnWidth(2, 80.0f);
        ImGui::SetColumnWidth(3, 60.0f);
        ImGui::SetColumnWidth(4, 60.0f);

        ImGui::Text("Name");
        ImGui::NextColumn();
        ImGui::Text("Type");
        ImGui::NextColumn();
        ImGui::Text("Size");
        ImGui::NextColumn();
        ImGui::Text("Deps");
        ImGui::NextColumn();
        ImGui::Text("Refs");
        ImGui::NextColumn();
        ImGui::Separator();

        for (const auto& [path, node] : m_nodes)
        {
            if (!PassesFilter(node))
                continue;

            bool isSelected = (path == m_selectedAssetPath);
            ImVec4 typeColor = GetAssetTypeColor(node.type);

            if (ImGui::Selectable(node.name.c_str(), isSelected, ImGuiSelectableFlags_SpanAllColumns))
            {
                SelectAsset(path);
            }
            ImGui::NextColumn();

            ImGui::TextColored(typeColor, "%s", GetAssetTypeName(node.type));
            ImGui::NextColumn();

            // Display size in human-readable format
            if (node.fileSizeBytes < 1024)
                ImGui::Text("%llu B", static_cast<unsigned long long>(node.fileSizeBytes));
            else if (node.fileSizeBytes < 1024 * 1024)
                ImGui::Text("%.1f KB", static_cast<float>(node.fileSizeBytes) / 1024.0f);
            else
                ImGui::Text("%.1f MB", static_cast<float>(node.fileSizeBytes) / (1024.0f * 1024.0f));
            ImGui::NextColumn();

            ImGui::Text("%zu", node.dependencies.size());
            ImGui::NextColumn();

            ImGui::Text("%zu", node.dependents.size());
            ImGui::NextColumn();
        }

        ImGui::Columns(1);
    }

    // =========================================================================
    // Graph helpers
    // =========================================================================

    void AssetDependencyPanel::BuildGraph()
    {
        m_edges.clear();

        // Clear previous dependency/dependent links
        for (auto& [path, node] : m_nodes)
        {
            node.dependencies.clear();
            node.dependents.clear();
        }

        // Build dependency edges by scanning file contents for references to other assets.
        // We look for string occurrences of known asset filenames within each file.
        std::vector<std::string> allPaths;
        allPaths.reserve(m_nodes.size());
        for (const auto& [path, node] : m_nodes)
        {
            allPaths.push_back(path);
        }

        for (auto& [path, node] : m_nodes)
        {
            // Read file content and look for references to other assets
            std::ifstream file(path, std::ios::in);
            if (!file.is_open())
                continue;

            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            file.close();

            for (const auto& candidatePath : allPaths)
            {
                if (candidatePath == path)
                    continue;

                // Search for the filename (with extension) in this file's content
                auto candidateIt = m_nodes.find(candidatePath);
                if (candidateIt == m_nodes.end())
                    continue;

                // Build search string: filename with extension
                fs::path fsPath(candidatePath);
                std::string filename = fsPath.filename().string();

                if (content.find(filename) != std::string::npos)
                {
                    node.dependencies.push_back(candidatePath);
                    candidateIt->second.dependents.push_back(path);

                    AssetEdge edge;
                    edge.fromPath = path;
                    edge.toPath = candidatePath;
                    m_edges.push_back(std::move(edge));
                }
            }
        }
    }

    void AssetDependencyPanel::LayoutGraph()
    {
        if (m_nodes.empty())
            return;

        // Initial placement: arrange nodes in a grid
        int cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(m_nodes.size()))));
        float spacing = 180.0f;
        int idx = 0;

        for (auto& [path, node] : m_nodes)
        {
            int row = idx / cols;
            int col = idx % cols;
            node.graphPosition = ImVec2(static_cast<float>(col) * spacing, static_cast<float>(row) * spacing);
            idx++;
        }

        // Run force-directed layout to spread nodes nicely
        ForceDirectedLayout(100);
    }

    void AssetDependencyPanel::ForceDirectedLayout(int iterations)
    {
        if (m_nodes.size() < 2)
            return;

        // Collect pointers for indexed access
        std::vector<AssetNode*> nodeList;
        nodeList.reserve(m_nodes.size());
        for (auto& [path, node] : m_nodes)
        {
            nodeList.push_back(&node);
        }

        // Map path -> index for edge lookups
        std::unordered_map<std::string, size_t> pathToIndex;
        for (size_t i = 0; i < nodeList.size(); i++)
        {
            pathToIndex[nodeList[i]->path] = i;
        }

        std::vector<ImVec2> velocities(nodeList.size(), ImVec2(0.0f, 0.0f));

        for (int iter = 0; iter < iterations; iter++)
        {
            std::vector<ImVec2> forces(nodeList.size(), ImVec2(0.0f, 0.0f));

            // Repulsive forces between all pairs
            for (size_t i = 0; i < nodeList.size(); i++)
            {
                for (size_t j = i + 1; j < nodeList.size(); j++)
                {
                    float dx = nodeList[i]->graphPosition.x - nodeList[j]->graphPosition.x;
                    float dy = nodeList[i]->graphPosition.y - nodeList[j]->graphPosition.y;
                    float distSq = dx * dx + dy * dy;
                    if (distSq < 1.0f)
                        distSq = 1.0f;

                    float repForce = m_layoutRepulsion / distSq;
                    float dist = sqrtf(distSq);
                    float fx = (dx / dist) * repForce;
                    float fy = (dy / dist) * repForce;

                    forces[i].x += fx;
                    forces[i].y += fy;
                    forces[j].x -= fx;
                    forces[j].y -= fy;
                }
            }

            // Attractive forces along edges (spring model)
            for (const auto& edge : m_edges)
            {
                auto fromIdx = pathToIndex.find(edge.fromPath);
                auto toIdx = pathToIndex.find(edge.toPath);
                if (fromIdx == pathToIndex.end() || toIdx == pathToIndex.end())
                    continue;

                size_t fi = fromIdx->second;
                size_t ti = toIdx->second;

                float dx = nodeList[fi]->graphPosition.x - nodeList[ti]->graphPosition.x;
                float dy = nodeList[fi]->graphPosition.y - nodeList[ti]->graphPosition.y;
                float dist = sqrtf(dx * dx + dy * dy);
                if (dist < 1.0f)
                    dist = 1.0f;

                float attrForce = m_layoutAttraction * dist;
                float fx = (dx / dist) * attrForce;
                float fy = (dy / dist) * attrForce;

                forces[fi].x -= fx;
                forces[fi].y -= fy;
                forces[ti].x += fx;
                forces[ti].y += fy;
            }

            // Apply forces with damping
            for (size_t i = 0; i < nodeList.size(); i++)
            {
                velocities[i].x = (velocities[i].x + forces[i].x) * m_layoutDamping;
                velocities[i].y = (velocities[i].y + forces[i].y) * m_layoutDamping;

                // Clamp velocity
                float maxVel = 50.0f;
                float vel = sqrtf(velocities[i].x * velocities[i].x + velocities[i].y * velocities[i].y);
                if (vel > maxVel)
                {
                    velocities[i].x = (velocities[i].x / vel) * maxVel;
                    velocities[i].y = (velocities[i].y / vel) * maxVel;
                }

                nodeList[i]->graphPosition.x += velocities[i].x;
                nodeList[i]->graphPosition.y += velocities[i].y;
            }
        }
    }

    void AssetDependencyPanel::HighlightDependencyChain(const std::string& assetPath)
    {
        // BFS forward through dependencies
        std::unordered_set<std::string> visited;
        std::queue<std::string> bfsQueue;
        bfsQueue.push(assetPath);
        visited.insert(assetPath);

        while (!bfsQueue.empty())
        {
            std::string current = bfsQueue.front();
            bfsQueue.pop();

            auto it = m_nodes.find(current);
            if (it == m_nodes.end())
                continue;

            it->second.isHighlighted = true;

            for (const auto& depPath : it->second.dependencies)
            {
                if (visited.find(depPath) == visited.end())
                {
                    visited.insert(depPath);
                    bfsQueue.push(depPath);
                }
            }
        }

        // Also highlight reverse chain (dependents)
        std::queue<std::string> revQueue;
        revQueue.push(assetPath);

        while (!revQueue.empty())
        {
            std::string current = revQueue.front();
            revQueue.pop();

            auto it = m_nodes.find(current);
            if (it == m_nodes.end())
                continue;

            it->second.isHighlighted = true;

            for (const auto& refPath : it->second.dependents)
            {
                if (visited.find(refPath) == visited.end())
                {
                    visited.insert(refPath);
                    revQueue.push(refPath);
                }
            }
        }

        // Highlight matching edges
        for (auto& edge : m_edges)
        {
            if (visited.count(edge.fromPath) && visited.count(edge.toPath))
            {
                edge.isHighlighted = true;
            }
        }
    }

    void AssetDependencyPanel::ClearHighlights()
    {
        for (auto& [path, node] : m_nodes)
        {
            node.isSelected = false;
            node.isHighlighted = false;
        }
        for (auto& edge : m_edges)
        {
            edge.isHighlighted = false;
        }
    }

    void AssetDependencyPanel::DetectCycles()
    {
        // Standard DFS-based cycle detection using coloring (WHITE=0, GRAY=1, BLACK=2)
        m_lastAnalysis.cycles.clear();

        for (auto& [path, node] : m_nodes)
        {
            node.isInCycle = false;
        }

        enum class Color
        {
            White,
            Gray,
            Black
        };

        std::unordered_map<std::string, Color> color;
        std::unordered_map<std::string, std::string> parent;

        for (const auto& [path, node] : m_nodes)
        {
            color[path] = Color::White;
        }

        // DFS with back-edge detection
        std::function<void(const std::string&, std::vector<std::string>&)> dfs =
            [&](const std::string& u, std::vector<std::string>& currentPath)
        {
            color[u] = Color::Gray;
            currentPath.push_back(u);

            auto nodeIt = m_nodes.find(u);
            if (nodeIt != m_nodes.end())
            {
                for (const auto& dep : nodeIt->second.dependencies)
                {
                    if (color.find(dep) == color.end())
                        continue;

                    if (color[dep] == Color::Gray)
                    {
                        // Found a back edge — extract the cycle
                        std::vector<std::string> cycle;
                        bool collecting = false;
                        for (const auto& node : currentPath)
                        {
                            if (node == dep)
                                collecting = true;
                            if (collecting)
                                cycle.push_back(node);
                        }

                        if (!cycle.empty())
                        {
                            m_lastAnalysis.cycles.push_back(cycle);

                            // Mark nodes in cycle
                            for (const auto& cyclePath : cycle)
                            {
                                auto cycleIt = m_nodes.find(cyclePath);
                                if (cycleIt != m_nodes.end())
                                {
                                    cycleIt->second.isInCycle = true;
                                }
                            }
                        }
                    }
                    else if (color[dep] == Color::White)
                    {
                        dfs(dep, currentPath);
                    }
                }
            }

            currentPath.pop_back();
            color[u] = Color::Black;
        };

        for (const auto& [path, node] : m_nodes)
        {
            if (color[path] == Color::White)
            {
                std::vector<std::string> currentPath;
                dfs(path, currentPath);
            }
        }
    }

    void AssetDependencyPanel::ComputeDepths()
    {
        // BFS from all scene nodes (root assets) to compute depth
        for (auto& [path, node] : m_nodes)
        {
            node.depthFromRoot = -1;
        }

        std::queue<std::string> bfsQueue;

        // Scenes are roots (depth 0)
        for (auto& [path, node] : m_nodes)
        {
            if (node.type == AssetType::Scene)
            {
                node.depthFromRoot = 0;
                bfsQueue.push(path);
            }
        }

        // Also treat any asset with no dependents (true root) as depth 0
        for (auto& [path, node] : m_nodes)
        {
            if (node.dependents.empty() && node.depthFromRoot < 0)
            {
                node.depthFromRoot = 0;
                bfsQueue.push(path);
            }
        }

        while (!bfsQueue.empty())
        {
            std::string current = bfsQueue.front();
            bfsQueue.pop();

            auto it = m_nodes.find(current);
            if (it == m_nodes.end())
                continue;

            int nextDepth = it->second.depthFromRoot + 1;

            for (const auto& depPath : it->second.dependencies)
            {
                auto depIt = m_nodes.find(depPath);
                if (depIt != m_nodes.end() && depIt->second.depthFromRoot < 0)
                {
                    depIt->second.depthFromRoot = nextDepth;
                    bfsQueue.push(depPath);
                }
            }
        }
    }

    // =========================================================================
    // Canvas helpers
    // =========================================================================

    ImVec2 AssetDependencyPanel::ScreenToGraph(ImVec2 screenPos) const
    {
        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        return ImVec2((screenPos.x - canvasPos.x) / m_graphZoom - m_graphOffset.x,
                      (screenPos.y - canvasPos.y) / m_graphZoom - m_graphOffset.y);
    }

    ImVec2 AssetDependencyPanel::GraphToScreen(ImVec2 graphPos) const
    {
        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        return ImVec2(canvasPos.x + (graphPos.x + m_graphOffset.x) * m_graphZoom,
                      canvasPos.y + (graphPos.y + m_graphOffset.y) * m_graphZoom);
    }

    void AssetDependencyPanel::HandleGraphInput()
    {
        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        ImVec2 canvasSize = ImGui::GetContentRegionAvail();

        ImVec2 mousePos = ImGui::GetMousePos();
        bool isHovered = (mousePos.x >= canvasPos.x && mousePos.x <= canvasPos.x + canvasSize.x &&
                          mousePos.y >= canvasPos.y && mousePos.y <= canvasPos.y + canvasSize.y);

        if (!isHovered)
            return;

        // Zoom with scroll wheel
        float scroll = ImGui::GetIO().MouseWheel;
        if (scroll != 0.0f)
        {
            float zoomFactor = 1.0f + scroll * 0.1f;
            float newZoom = m_graphZoom * zoomFactor;
            newZoom = std::clamp(newZoom, 0.1f, 5.0f);

            // Zoom towards mouse position
            ImVec2 mouseGraphPos = ScreenToGraph(mousePos);
            m_graphZoom = newZoom;
            // Adjust offset so the point under the mouse stays fixed
            m_graphOffset.x = (mousePos.x - canvasPos.x) / m_graphZoom - mouseGraphPos.x;
            m_graphOffset.y = (mousePos.y - canvasPos.y) / m_graphZoom - mouseGraphPos.y;
        }

        // Pan with middle mouse button or right mouse button
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
            (ImGui::IsMouseDragging(ImGuiMouseButton_Right) && !m_isDraggingNode))
        {
            ImVec2 delta = ImGui::GetIO().MouseDelta;
            m_graphOffset.x += delta.x / m_graphZoom;
            m_graphOffset.y += delta.y / m_graphZoom;
        }

        // Release drag state
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            m_isDraggingNode = false;
        }

        // Reset hovered state each frame
        m_hoveredAssetPath.clear();
    }

    ImVec4 AssetDependencyPanel::GetAssetTypeColor(AssetType type) const
    {
        switch (type)
        {
        case AssetType::Texture:
            return ImVec4(0.2f, 0.8f, 0.4f, 1.0f);
        case AssetType::Mesh:
            return ImVec4(0.4f, 0.6f, 1.0f, 1.0f);
        case AssetType::Material:
            return ImVec4(0.9f, 0.4f, 0.7f, 1.0f);
        case AssetType::Shader:
            return ImVec4(1.0f, 0.8f, 0.2f, 1.0f);
        case AssetType::Audio:
            return ImVec4(0.3f, 0.9f, 0.9f, 1.0f);
        case AssetType::Animation:
            return ImVec4(0.9f, 0.5f, 0.2f, 1.0f);
        case AssetType::Prefab:
            return ImVec4(0.6f, 0.4f, 1.0f, 1.0f);
        case AssetType::Scene:
            return ImVec4(1.0f, 1.0f, 0.4f, 1.0f);
        case AssetType::Script:
            return ImVec4(0.4f, 1.0f, 0.6f, 1.0f);
        case AssetType::Font:
            return ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
        case AssetType::ParticleEffect:
            return ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
        case AssetType::Dialogue:
            return ImVec4(0.7f, 0.9f, 0.3f, 1.0f);
        case AssetType::Config:
            return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
        case AssetType::Unknown:
        default:
            return ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
        }
    }

    const char* AssetDependencyPanel::GetAssetTypeName(AssetType type) const
    {
        switch (type)
        {
        case AssetType::Texture:
            return "Texture";
        case AssetType::Mesh:
            return "Mesh";
        case AssetType::Material:
            return "Material";
        case AssetType::Shader:
            return "Shader";
        case AssetType::Audio:
            return "Audio";
        case AssetType::Animation:
            return "Animation";
        case AssetType::Prefab:
            return "Prefab";
        case AssetType::Scene:
            return "Scene";
        case AssetType::Script:
            return "Script";
        case AssetType::Font:
            return "Font";
        case AssetType::ParticleEffect:
            return "ParticleEffect";
        case AssetType::Dialogue:
            return "Dialogue";
        case AssetType::Config:
            return "Config";
        case AssetType::Unknown:
        default:
            return "Unknown";
        }
    }

    AssetType AssetDependencyPanel::ClassifyAsset(const std::string& path) const
    {
        // Extract extension (lowercase)
        auto dotPos = path.rfind('.');
        if (dotPos == std::string::npos)
            return AssetType::Unknown;

        std::string ext = path.substr(dotPos);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".dds" || ext == ".tga" || ext == ".bmp")
            return AssetType::Texture;
        if (ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb")
            return AssetType::Mesh;
        if (ext == ".sparkmat")
            return AssetType::Material;
        if (ext == ".hlsl" || ext == ".glsl" || ext == ".vert" || ext == ".frag" || ext == ".comp")
            return AssetType::Shader;
        if (ext == ".wav" || ext == ".ogg" || ext == ".mp3" || ext == ".flac")
            return AssetType::Audio;
        if (ext == ".sparkanim")
            return AssetType::Animation;
        if (ext == ".sparkprefab")
            return AssetType::Prefab;
        if (ext == ".sparkscene")
            return AssetType::Scene;
        if (ext == ".as")
            return AssetType::Script;
        if (ext == ".ttf" || ext == ".otf")
            return AssetType::Font;
        if (ext == ".sparkfx")
            return AssetType::ParticleEffect;
        if (ext == ".sparkdlg")
            return AssetType::Dialogue;
        if (ext == ".json" || ext == ".xml" || ext == ".ini")
            return AssetType::Config;

        return AssetType::Unknown;
    }

    // =========================================================================
    // Internal filter helper
    // =========================================================================

    bool AssetDependencyPanel::PassesFilter(const AssetNode& node) const
    {
        // Type filter
        switch (node.type)
        {
        case AssetType::Texture:
            if (!m_filter.showTextures)
                return false;
            break;
        case AssetType::Mesh:
            if (!m_filter.showMeshes)
                return false;
            break;
        case AssetType::Material:
            if (!m_filter.showMaterials)
                return false;
            break;
        case AssetType::Shader:
            if (!m_filter.showShaders)
                return false;
            break;
        case AssetType::Audio:
            if (!m_filter.showAudio)
                return false;
            break;
        case AssetType::Animation:
            if (!m_filter.showAnimations)
                return false;
            break;
        case AssetType::Prefab:
            if (!m_filter.showPrefabs)
                return false;
            break;
        case AssetType::Scene:
            if (!m_filter.showScenes)
                return false;
            break;
        case AssetType::Script:
            if (!m_filter.showScripts)
                return false;
            break;
        default:
            if (!m_filter.showOther)
                return false;
            break;
        }

        // Orphan-only filter
        if (m_filter.showOrphansOnly && !node.isOrphan)
            return false;

        // Cycle-only filter
        if (m_filter.showCyclesOnly && !node.isInCycle)
            return false;

        // Depth filter
        if (m_filter.maxDepth >= 0 && node.depthFromRoot > m_filter.maxDepth)
            return false;

        // Search filter
        if (!m_filter.searchFilter.empty())
        {
            std::string lowerName = node.name;
            std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::string lowerFilter = m_filter.searchFilter;
            std::transform(lowerFilter.begin(), lowerFilter.end(), lowerFilter.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            if (lowerName.find(lowerFilter) == std::string::npos &&
                node.path.find(m_filter.searchFilter) == std::string::npos)
            {
                return false;
            }
        }

        return true;
    }

} // namespace SparkEditor
