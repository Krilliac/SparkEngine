/**
 * @file TilemapEditorPanel.cpp
 * @brief Implementation of the Tilemap Editor panel
 * @author Spark Engine Team
 * @date 2026
 */

#include "TilemapEditorPanel.h"
#include "../Core/EditorIcons.h"
#include "../Core/EditorFonts.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"
#include <imgui.h>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <queue>
#include "Utils/LogMacros.h"

#include "SparkUI/Runtime.h"
#include "SparkUI/Widgets.h"

namespace SparkEditor
{

    TilemapEditorPanel::TilemapEditorPanel() : EditorPanel("Tilemap Editor", "tilemap_editor_panel") {}

    bool TilemapEditorPanel::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Initializing Tilemap Editor panel");
        return true;
    }

    void TilemapEditorPanel::Update(float /*deltaTime*/)
    {
        // Nothing frame-rate dependent
    }

    void TilemapEditorPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            if (m_inspectedObjectID == INVALID_OBJECT_ID)
            {
                float avail = ImGui::GetContentRegionAvail().y;
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + avail * 0.3f);
                Spark::Ui::EmptyState(ICON_FA_TH, "Select a tilemap entity to edit");
            }
            else
            {
                RenderToolbar();
                ImGui::Separator();

                // Split layout: tile palette on left, map canvas on right
                float panelWidth = ImGui::GetContentRegionAvail().x;
                float paletteWidth = (std::min)(panelWidth * 0.25f, Spark::Ui::Px(200.0f));

                ImGui::BeginChild("TilePalette", ImVec2(paletteWidth, 0), true);
                RenderTilePalette();
                ImGui::EndChild();

                ImGui::SameLine();

                ImGui::BeginChild("MapCanvas", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
                RenderMapCanvas();
                ImGui::EndChild();
            }
        }
        // ImGui::Begin was called even when BeginPanel() reports a collapsed or hidden-tab window.
        EndPanel();
    }

    void TilemapEditorPanel::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Shutting down Tilemap Editor panel");
    }

    void TilemapEditorPanel::RenderToolbar()
    {
        // Tool selection
        auto toolButton = [this](const char* icon, TileTool tool, const char* tooltip)
        {
            if (Spark::Ui::IconButton(tooltip, icon, tooltip, m_currentTool == tool))
                m_currentTool = tool;
        };

        toolButton(ICON_FA_PAINT_BRUSH, TileTool::Paint, "Paint (B)");
        ImGui::SameLine();
        toolButton(ICON_FA_ERASER, TileTool::Erase, "Erase (E)");
        ImGui::SameLine();
        toolButton(ICON_FA_FILL_DRIP, TileTool::Fill, "Flood Fill (G)");
        ImGui::SameLine();
        toolButton(ICON_FA_VECTOR_SQUARE, TileTool::Rectangle, "Rectangle (R)");
        ImGui::SameLine();
        toolButton(ICON_FA_EYE_DROPPER, TileTool::Eyedropper, "Eyedropper (I)");
        ImGui::SameLine();
        toolButton(ICON_FA_SHIELD_ALT, TileTool::CollisionPaint, "Collision Paint (C)");

        Spark::Ui::ToolbarSeparator();

        // Zoom
        if (Spark::Ui::IconButton("ZoomIn", ICON_FA_SEARCH_PLUS, "Zoom in"))
            m_canvasZoom *= 1.25f;
        ImGui::SameLine();
        if (Spark::Ui::IconButton("ZoomOut", ICON_FA_SEARCH_MINUS, "Zoom out"))
            m_canvasZoom *= 0.8f;
        m_canvasZoom = std::clamp(m_canvasZoom, 0.25f, 8.0f);

        ImGui::SameLine();
        ImGui::Checkbox("Grid", &m_showGrid);
        ImGui::SameLine();
        ImGui::Checkbox("Collision", &m_showCollision);

        // Undo/Redo
        Spark::Ui::ToolbarSeparator();
        {
            bool canUndo = !m_undoStack.empty();
            if (!canUndo)
                ImGui::BeginDisabled();
            if (ImGui::Button(ICON_FA_UNDO " Undo"))
            {
                PerformUndo();
            }
            if (!canUndo)
                ImGui::EndDisabled();
        }
        ImGui::SameLine();
        {
            bool canRedo = !m_redoStack.empty();
            if (!canRedo)
                ImGui::BeginDisabled();
            if (ImGui::Button(ICON_FA_REDO " Redo"))
            {
                PerformRedo();
            }
            if (!canRedo)
                ImGui::EndDisabled();
        }
    }

    void TilemapEditorPanel::RenderTilePalette()
    {
        Spark::Ui::SubHeader(ICON_FA_PALETTE " Tile Palette");

        // Tileset properties
        static int tilesetCols = 8;
        static int tilesetRows = 8;
        ImGui::DragInt("Cols", &tilesetCols, 0.1f, 1, 64);
        ImGui::DragInt("Rows", &tilesetRows, 0.1f, 1, 64);

        ImGui::Spacing();
        Spark::Ui::KeyValue("Selected:", "%d", m_selectedTileID);

        ImGui::Separator();

        // Draw tile palette grid
        float availW = ImGui::GetContentRegionAvail().x;
        float tileSize = (std::max)(availW / static_cast<float>(tilesetCols), Spark::Ui::Px(16.0f));
        const ImU32 selectionCol = Spark::Ui::Col(Spark::Ui::Role::AccentText);
        const ImU32 gridCol = Spark::Ui::Col(Spark::Ui::Role::BorderSubtle);
        const float selectionThickness = Spark::Ui::Px(2.0f);

        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        for (int y = 0; y < tilesetRows; ++y)
        {
            for (int x = 0; x < tilesetCols; ++x)
            {
                int tileID = y * tilesetCols + x + 1;

                float px = canvasPos.x + x * tileSize;
                float py = canvasPos.y + y * tileSize;

                // Tile color (pseudo-visualization without real texture)
                ImU32 tileColor =
                    IM_COL32((tileID * 37) % 200 + 55, (tileID * 73) % 200 + 55, (tileID * 113) % 200 + 55, 255);

                drawList->AddRectFilled(ImVec2(px, py), ImVec2(px + tileSize - 1, py + tileSize - 1), tileColor);

                // Highlight selected
                if (tileID == m_selectedTileID)
                {
                    drawList->AddRect(ImVec2(px - 1, py - 1), ImVec2(px + tileSize, py + tileSize), selectionCol, 0.0f,
                                      0, selectionThickness);
                }

                // Grid lines
                drawList->AddRect(ImVec2(px, py), ImVec2(px + tileSize - 1, py + tileSize - 1), gridCol);
            }
        }

        float totalHeight = tilesetRows * tileSize;
        ImGui::Dummy(ImVec2(availW, totalHeight));

        // Handle clicks on palette
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            ImVec2 mousePos = ImGui::GetIO().MousePos;
            int clickX = static_cast<int>((mousePos.x - canvasPos.x) / tileSize);
            int clickY = static_cast<int>((mousePos.y - canvasPos.y) / tileSize);
            if (clickX >= 0 && clickX < tilesetCols && clickY >= 0 && clickY < tilesetRows)
            {
                m_selectedTileID = clickY * tilesetCols + clickX + 1;
            }
        }

        ImGui::Spacing();
        RenderMapProperties();
        RenderAutoTileSettings();
    }

    void TilemapEditorPanel::PushUndoSnapshot(const std::vector<int32_t>& tiles, const std::vector<bool>& collision,
                                              int width, int height)
    {
        TilemapSnapshot snapshot;
        snapshot.tiles = tiles;
        snapshot.collisionFlags = collision;
        snapshot.width = width;
        snapshot.height = height;
        m_undoStack.push_back(std::move(snapshot));
        if (m_undoStack.size() > MAX_UNDO_HISTORY)
        {
            m_undoStack.erase(m_undoStack.begin());
        }
        m_redoStack.clear();
    }

    void TilemapEditorPanel::EnsureMapData()
    {
        if (m_tiles.empty() || static_cast<int>(m_tiles.size()) != m_mapWidth * m_mapHeight)
        {
            m_tiles.resize(static_cast<size_t>(m_mapWidth) * m_mapHeight, 0);
            m_collision.resize(static_cast<size_t>(m_mapWidth) * m_mapHeight, false);
        }
    }

    void TilemapEditorPanel::RenderMapCanvas()
    {
        EnsureMapData();

        float tileSize = Spark::Ui::Px(16.0f) * m_canvasZoom;
        float canvasW = m_mapWidth * tileSize;
        float canvasH = m_mapHeight * tileSize;
        const ImU32 gridCol = Spark::Ui::Col(Spark::Ui::Role::BorderSubtle);
        const ImU32 collisionFill = Spark::Ui::Col(Spark::Ui::Role::ErrorText, 0.3f);
        const ImU32 collisionEdge = Spark::Ui::Col(Spark::Ui::Role::ErrorText, 0.8f);
        const float collisionInset = Spark::Ui::Px(2.0f);
        const float collisionEdgeInset = Spark::Ui::Px(1.0f);

        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        canvasPos.x += m_canvasPanX;
        canvasPos.y += m_canvasPanY;
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Draw tiles
        for (int y = 0; y < m_mapHeight; ++y)
        {
            for (int x = 0; x < m_mapWidth; ++x)
            {
                float px = canvasPos.x + x * tileSize;
                float py = canvasPos.y + y * tileSize;

                int32_t tileID = m_tiles[static_cast<size_t>(y) * m_mapWidth + x];

                if (tileID > 0)
                {
                    ImU32 col =
                        IM_COL32((tileID * 37) % 200 + 55, (tileID * 73) % 200 + 55, (tileID * 113) % 200 + 55, 255);
                    drawList->AddRectFilled(ImVec2(px, py), ImVec2(px + tileSize, py + tileSize), col);
                }

                // Grid
                if (m_showGrid)
                {
                    drawList->AddRect(ImVec2(px, py), ImVec2(px + tileSize, py + tileSize), gridCol);
                }

                // Collision overlay
                if (m_showCollision && m_collision[static_cast<size_t>(y) * m_mapWidth + x])
                {
                    drawList->AddRectFilled(ImVec2(px + collisionInset, py + collisionInset),
                                            ImVec2(px + tileSize - collisionInset, py + tileSize - collisionInset),
                                            collisionFill);
                    drawList->AddRect(ImVec2(px + collisionEdgeInset, py + collisionEdgeInset),
                                      ImVec2(px + tileSize - collisionEdgeInset, py + tileSize - collisionEdgeInset),
                                      collisionEdge);
                }
            }
        }

        // Map border
        drawList->AddRect(canvasPos, ImVec2(canvasPos.x + canvasW, canvasPos.y + canvasH),
                          Spark::Ui::Col(Spark::Ui::Role::BorderStrong), 0.0f, 0, Spark::Ui::Px(2.0f));

        ImGui::Dummy(ImVec2(canvasW + Spark::Ui::Px(20.0f), canvasH + Spark::Ui::Px(20.0f)));

        // Handle painting
        if (ImGui::IsItemHovered())
        {
            ImVec2 mousePos = ImGui::GetIO().MousePos;
            int tileX = static_cast<int>((mousePos.x - canvasPos.x) / tileSize);
            int tileY = static_cast<int>((mousePos.y - canvasPos.y) / tileSize);

            // Highlight hovered tile
            if (tileX >= 0 && tileX < m_mapWidth && tileY >= 0 && tileY < m_mapHeight)
            {
                float hx = canvasPos.x + tileX * tileSize;
                float hy = canvasPos.y + tileY * tileSize;
                drawList->AddRect(ImVec2(hx, hy), ImVec2(hx + tileSize, hy + tileSize),
                                  Spark::Ui::Col(Spark::Ui::Role::AccentText, 0.8f), 0.0f, 0, Spark::Ui::Px(2.0f));

                if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
                {
                    // Capture undo snapshot on first click
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    {
                        PushUndoSnapshot(m_tiles, m_collision, m_mapWidth, m_mapHeight);
                    }

                    size_t idx = static_cast<size_t>(tileY) * m_mapWidth + tileX;
                    switch (m_currentTool)
                    {
                    case TileTool::Paint:
                        m_tiles[idx] = m_selectedTileID;
                        break;
                    case TileTool::Erase:
                        m_tiles[idx] = 0;
                        break;
                    case TileTool::Fill:
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                            FloodFill(m_tiles, m_mapWidth, m_mapHeight, tileX, tileY, m_selectedTileID);
                        break;
                    case TileTool::Eyedropper:
                        m_selectedTileID = m_tiles[idx];
                        break;
                    case TileTool::CollisionPaint:
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                            m_collision[idx] = !m_collision[idx];
                        break;
                    case TileTool::Rectangle:
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                        {
                            m_rectDragging = true;
                            m_rectStartX = tileX;
                            m_rectStartY = tileY;
                        }
                        break;
                    }
                }

                // Rectangle tool: commit on release
                if (m_currentTool == TileTool::Rectangle && m_rectDragging &&
                    ImGui::IsMouseReleased(ImGuiMouseButton_Left))
                {
                    m_rectDragging = false;
                    int x0 = (std::min)(m_rectStartX, tileX);
                    int y0 = (std::min)(m_rectStartY, tileY);
                    int x1 = (std::max)(m_rectStartX, tileX);
                    int y1 = (std::max)(m_rectStartY, tileY);
                    for (int fy = y0; fy <= y1; ++fy)
                    {
                        for (int fx = x0; fx <= x1; ++fx)
                        {
                            if (fx >= 0 && fx < m_mapWidth && fy >= 0 && fy < m_mapHeight)
                                m_tiles[static_cast<size_t>(fy) * m_mapWidth + fx] = m_selectedTileID;
                        }
                    }
                }
            }

            // Pan with middle mouse
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
            {
                ImVec2 delta = ImGui::GetIO().MouseDelta;
                m_canvasPanX += delta.x;
                m_canvasPanY += delta.y;
            }

            // Zoom with scroll
            float wheel = ImGui::GetIO().MouseWheel;
            if (std::abs(wheel) > 0.01f)
            {
                m_canvasZoom *= (wheel > 0) ? 1.1f : 0.9f;
                m_canvasZoom = std::clamp(m_canvasZoom, 0.25f, 8.0f);
            }
        }

        // Status bar
        Spark::Ui::SecondaryText("Map: %dx%d | Zoom: %.1fx | Tool: %s | Tile: %d", m_mapWidth, m_mapHeight,
                                 m_canvasZoom,
                                 m_currentTool == TileTool::Paint            ? "Paint"
                                 : m_currentTool == TileTool::Erase          ? "Erase"
                                 : m_currentTool == TileTool::Fill           ? "Fill"
                                 : m_currentTool == TileTool::Rectangle      ? "Rectangle"
                                 : m_currentTool == TileTool::Eyedropper     ? "Eyedropper"
                                 : m_currentTool == TileTool::CollisionPaint ? "Collision"
                                                                             : "Unknown",
                                 m_selectedTileID);
    }

    void TilemapEditorPanel::RenderMapProperties()
    {
        if (Spark::Ui::SectionHeader("Map Properties", ICON_FA_COGS, false))
        {
            static int newWidth = 20;
            static int newHeight = 15;
            ImGui::DragInt("Width", &newWidth, 0.1f, 1, 256);
            ImGui::DragInt("Height", &newHeight, 0.1f, 1, 256);

            const bool sizeChanged = newWidth != m_mapWidth || newHeight != m_mapHeight;
            ImGui::BeginDisabled(!sizeChanged);
            if (ImGui::Button("Resize Map"))
            {
                ResizeMap(newWidth, newHeight);
            }
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Keeps the overlapping tiles; Undo restores the previous size.");

            ImGui::Spacing();

            static int tileW = 16;
            static int tileH = 16;
            ImGui::DragInt("Tile Width (px)", &tileW, 0.1f, 1, 256);
            ImGui::DragInt("Tile Height (px)", &tileH, 0.1f, 1, 256);

            static float ppu = 100.0f;
            ImGui::DragFloat("Pixels Per Unit", &ppu, 1.0f, 1.0f, 1000.0f);

            static int sortLayer = 0;
            ImGui::DragInt("Sorting Layer", &sortLayer);

            static bool genCollision = true;
            ImGui::Checkbox("Generate Collision", &genCollision);
        }
    }

    void TilemapEditorPanel::ResizeMap(int width, int height)
    {
        width = std::clamp(width, 1, 256);
        height = std::clamp(height, 1, 256);
        if (width == m_mapWidth && height == m_mapHeight)
            return;

        EnsureMapData();
        PushUndoSnapshot(m_tiles, m_collision, m_mapWidth, m_mapHeight);

        std::vector<int32_t> tiles(static_cast<size_t>(width) * height, 0);
        std::vector<bool> collision(static_cast<size_t>(width) * height, false);
        const int keepWidth = (std::min)(width, m_mapWidth);
        const int keepHeight = (std::min)(height, m_mapHeight);
        for (int y = 0; y < keepHeight; ++y)
        {
            for (int x = 0; x < keepWidth; ++x)
            {
                const size_t from = static_cast<size_t>(y) * m_mapWidth + x;
                const size_t to = static_cast<size_t>(y) * width + x;
                tiles[to] = m_tiles[from];
                collision[to] = m_collision[from];
            }
        }

        m_tiles = std::move(tiles);
        m_collision = std::move(collision);
        m_mapWidth = width;
        m_mapHeight = height;
        SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "TilemapEditorPanel: resized map to %dx%d", width, height);
    }

    void TilemapEditorPanel::RenderAutoTileSettings()
    {
        if (Spark::Ui::SectionHeader("Auto-Tile Rules", ICON_FA_MAGIC, false))
        {
            Spark::Ui::UnavailableFeature("Auto-tiling",
                                          "Tile variants are not chosen from neighbouring tiles yet "
                                          "(Wang tiles / bitmask); painted tiles are placed as selected.");
        }
    }

    void TilemapEditorPanel::PerformUndo()
    {
        if (m_undoStack.empty())
            return;

        // Save current state to redo stack
        TilemapSnapshot current;
        current.tiles = m_tiles;
        current.collisionFlags = m_collision;
        current.width = m_mapWidth;
        current.height = m_mapHeight;
        m_redoStack.push_back(std::move(current));

        // Restore from undo stack
        const auto& snapshot = m_undoStack.back();
        m_tiles = snapshot.tiles;
        m_collision = snapshot.collisionFlags;
        m_mapWidth = snapshot.width;
        m_mapHeight = snapshot.height;
        m_undoStack.pop_back();
    }

    void TilemapEditorPanel::PerformRedo()
    {
        if (m_redoStack.empty())
            return;

        // Save current state to undo stack
        TilemapSnapshot current;
        current.tiles = m_tiles;
        current.collisionFlags = m_collision;
        current.width = m_mapWidth;
        current.height = m_mapHeight;
        m_undoStack.push_back(std::move(current));

        // Restore from redo stack
        const auto& snapshot = m_redoStack.back();
        m_tiles = snapshot.tiles;
        m_collision = snapshot.collisionFlags;
        m_mapWidth = snapshot.width;
        m_mapHeight = snapshot.height;
        m_redoStack.pop_back();
    }

    void TilemapEditorPanel::FloodFill(std::vector<int32_t>& tiles, int mapW, int mapH, int startX, int startY,
                                       int32_t newTile)
    {
        if (startX < 0 || startX >= mapW || startY < 0 || startY >= mapH)
            return;

        int32_t targetTile = tiles[static_cast<size_t>(startY) * mapW + startX];
        if (targetTile == newTile)
            return;

        std::queue<std::pair<int, int>> queue;
        queue.push({startX, startY});

        while (!queue.empty())
        {
            auto [x, y] = queue.front();
            queue.pop();

            if (x < 0 || x >= mapW || y < 0 || y >= mapH)
                continue;

            size_t idx = static_cast<size_t>(y) * mapW + x;
            if (tiles[idx] != targetTile)
                continue;

            tiles[idx] = newTile;

            queue.push({x + 1, y});
            queue.push({x - 1, y});
            queue.push({x, y + 1});
            queue.push({x, y - 1});
        }
    }

} // namespace SparkEditor
