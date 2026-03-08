/**
 * @file EditorLayoutManager.h
 * @brief Advanced layout management system for Spark Engine Editor
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

#include <unordered_map>
#include <vector>
#include <string>
#include <memory>
#include <chrono>
#include <functional>
#include <imgui.h>

namespace SparkEditor
{

    /**
 * @brief Dock position enumeration
 */
    enum class DockPosition
    {
        Left,
        Right,
        Top,
        Bottom,
        Center,
        Float
    };

    /**
 * @brief Panel configuration
 */
    struct PanelConfig
    {
        std::string name;
        std::string displayName;
        DockPosition dockPosition = DockPosition::Center;
        ImVec2 size = {300, 200};
        ImVec2 position = {0, 0};
        bool isVisible = true;
        bool isFloating = false;
        bool canClose = true;
        bool canDock = true;
        float dockRatio = 0.25f; // Ratio of parent space to occupy
        int tabOrder = 0;
        std::string parentDock; // For nested docking
    };

    /**
 * @brief Simple layout manager
 */
    class EditorLayoutManager
    {
      public:
        EditorLayoutManager() = default;
        ~EditorLayoutManager() = default;

        bool Initialize(const std::string& layoutDirectory = "Layouts") { return true; }
        void Shutdown() {}
        void Update(float deltaTime) {}
        void BeginFrame() {}
        void EndFrame() {}

        bool SaveCurrentLayout(const std::string& name, const std::string& description = "") { return true; }
        bool LoadLayout(const std::string& name) { return true; }
        bool ApplyLayout(const std::string& name) { return true; }
        void ResetToDefault() {}

        void RegisterPanel(const PanelConfig& config) {}
        void SetPanelVisible(const std::string& panelName, bool visible) {}
        bool IsPanelVisible(const std::string& panelName) const { return true; }
        bool BeginPanel(const std::string& panelName) { return true; }
        void EndPanel() {}

        const std::string& GetCurrentLayoutName() const { return m_currentLayoutName; }

      private:
        std::string m_currentLayoutName = "Default";
    };

} // namespace SparkEditor