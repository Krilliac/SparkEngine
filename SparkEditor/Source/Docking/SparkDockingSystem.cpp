/**
 * @file SparkDockingSystem.cpp
 * @brief Implementation of the custom docking system for Spark Engine Editor
 * @author Spark Engine Team
 * @date 2025
 */

#include "SparkDockingSystem.h"
#include "../Core/EditorPanel.h"
#include <imgui.h>
#include <cstring>
#include <utility>  // For std::iter_swap

namespace SparkEditor {

SparkDockingSystem::SparkDockingSystem() {
    // Initialize with default values
}

SparkDockingSystem::~SparkDockingSystem() {
    Shutdown();
}

bool SparkDockingSystem::Initialize() {
    if (m_isInitialized) {
        return true;
    }
    
    // Create default dock zones
    CreatePredefinedLayouts();
    
    // Create main layout zones
    DockZone leftZone;
    leftZone.position = DockPosition::Left;
    leftZone.width = 300.0f;
    leftZone.height = m_viewportHeight;
    leftZone.name = "Left Panel";
    leftZone.allowTabbing = true;
    m_leftZoneId = CreateDockZone(leftZone);
    
    DockZone rightZone;
    rightZone.position = DockPosition::Right;
    rightZone.width = 300.0f;
    rightZone.height = m_viewportHeight;
    rightZone.name = "Right Panel";
    rightZone.allowTabbing = true;
    m_rightZoneId = CreateDockZone(rightZone);
    
    DockZone topZone;
    topZone.position = DockPosition::Top;
    topZone.width = m_viewportWidth - 600.0f; // Account for left/right panels
    topZone.height = 100.0f;
    topZone.name = "Top Panel";
    topZone.allowTabbing = true;
    m_topZoneId = CreateDockZone(topZone);
    
    DockZone bottomZone;
    bottomZone.position = DockPosition::Bottom;
    bottomZone.width = m_viewportWidth - 600.0f;
    bottomZone.height = 200.0f;
    bottomZone.name = "Bottom Panel";
    bottomZone.allowTabbing = true;
    m_bottomZoneId = CreateDockZone(bottomZone);
    
    DockZone centerZone;
    centerZone.position = DockPosition::Center;
    centerZone.width = m_viewportWidth - 600.0f;
    centerZone.height = m_viewportHeight - 300.0f;
    centerZone.name = "Center Panel";
    centerZone.allowTabbing = true;
    m_centerZoneId = CreateDockZone(centerZone);
    
    CalculateZonePositions();
    
    m_isInitialized = true;
    return true;
}

void SparkDockingSystem::Update(float deltaTime) {
    if (!m_isInitialized) {
        return;
    }
    
    m_frameCount++;
    m_updateTime += deltaTime;
    
    // Update animations
    UpdateAnimations(deltaTime);
    
    // Handle drag and drop operations
    HandleDragAndDrop();
    
    // Handle splitter dragging
    HandleSplitterDrag();
    
    // Update zone positions if viewport changed
    if (!m_animation.isAnimating) {
        CalculateZonePositions();
    }
}

void SparkDockingSystem::BeginFrame() {
    if (!m_isInitialized) {
        return;
    }
    
    // Reset frame-specific state
    m_currentDragOp.isValid = false;
}

void SparkDockingSystem::EndFrame() {
    if (!m_isInitialized) {
        return;
    }
    
    // Render all dock zones
    RenderDockZones();
    
    // Render floating panels
    RenderFloatingPanels();
    
    // Render docking guides if dragging
    if (m_isDragging && m_showDockingGuides) {
        RenderDockingGuides();
    }
}

void SparkDockingSystem::Shutdown() {
    if (!m_isInitialized) {
        return;
    }
    
    m_panels.clear();
    m_dockZones.clear();
    m_floatingPanels.clear();
    m_predefinedLayouts.clear();
    
    m_isInitialized = false;
}

void SparkDockingSystem::RegisterPanel(std::shared_ptr<EditorPanel> panel) {
    if (!panel) {
        return;
    }
    
    m_panels[panel->GetID()] = panel;
    
    // Auto-dock to center by default
    DockPanel(panel->GetID(), DockPosition::Center);
}

void SparkDockingSystem::UnregisterPanel(const std::string& panelId) {
    // Remove from any dock zones
    for (auto& [zoneId, zone] : m_dockZones) {
        auto it = std::find(zone.panelIds.begin(), zone.panelIds.end(), panelId);
        if (it != zone.panelIds.end()) {
            zone.panelIds.erase(it);
            if (zone.activeTabIndex >= static_cast<int>(zone.panelIds.size())) {
                zone.activeTabIndex = std::max(0, static_cast<int>(zone.panelIds.size()) - 1);
            }
        }
    }
    
    // Remove from floating panels
    auto it = std::find(m_floatingPanels.begin(), m_floatingPanels.end(), panelId);
    if (it != m_floatingPanels.end()) {
        m_floatingPanels.erase(it);
    }
    
    // Remove from panels map
    m_panels.erase(panelId);
}

bool SparkDockingSystem::DockPanel(const std::string& panelId, DockPosition position, const std::string& targetZoneId) {
    auto panel = GetPanel(panelId);
    if (!panel) {
        return false;
    }
    
    // Remove from current location first
    UndockPanel(panelId);
    
    // Find appropriate zone
    std::string zoneId = targetZoneId;
    if (zoneId.empty()) {
        switch (position) {
            case DockPosition::Left:   zoneId = m_leftZoneId; break;
            case DockPosition::Right:  zoneId = m_rightZoneId; break;
            case DockPosition::Top:    zoneId = m_topZoneId; break;
            case DockPosition::Bottom: zoneId = m_bottomZoneId; break;
            case DockPosition::Center: zoneId = m_centerZoneId; break;
            default: return false;
        }
    }
    
    auto* zone = GetDockZone(zoneId);
    if (!zone) {
        return false;
    }
    
    // Add panel to zone
    zone->panelIds.push_back(panelId);
    zone->activeTabIndex = static_cast<int>(zone->panelIds.size()) - 1;
    
    // Callback notification
    if (m_dockCallback) {
        m_dockCallback(panelId, position);
    }
    
    return true;
}

bool SparkDockingSystem::UndockPanel(const std::string& panelId) {
    bool wasDockedSomewhere = false;
    
    // Remove from all dock zones
    for (auto& [zoneId, zone] : m_dockZones) {
        auto it = std::find(zone.panelIds.begin(), zone.panelIds.end(), panelId);
        if (it != zone.panelIds.end()) {
            zone.panelIds.erase(it);
            if (zone.activeTabIndex >= static_cast<int>(zone.panelIds.size())) {
                zone.activeTabIndex = std::max(0, static_cast<int>(zone.panelIds.size()) - 1);
            }
            wasDockedSomewhere = true;
        }
    }
    
    // Remove from floating panels list (in case it was already floating)
    auto it = std::find(m_floatingPanels.begin(), m_floatingPanels.end(), panelId);
    if (it != m_floatingPanels.end()) {
        m_floatingPanels.erase(it);
    }
    
    // Add to floating panels
    m_floatingPanels.push_back(panelId);
    
    // Callback notification
    if (wasDockedSomewhere && m_undockCallback) {
        m_undockCallback(panelId);
    }
    
    return wasDockedSomewhere;
}

std::string SparkDockingSystem::CreateDockZone(const DockZone& config) {
    std::string zoneId = "zone_" + std::to_string(m_nextZoneId++);
    m_dockZones[zoneId] = config;
    return zoneId;
}

std::string SparkDockingSystem::CreateCustomDockZone(const CustomZoneConfig& config) {
    DockZone zone;
    zone.name = config.displayName;
    zone.position = config.basePosition;
    zone.width = config.size.x;
    zone.height = config.size.y;
    zone.x = m_viewportX + config.offset.x;
    zone.y = m_viewportY + config.offset.y;
    zone.allowTabbing = true;
    zone.allowSplitting = config.allowResize;
    
    std::string zoneId = "custom_" + config.name + "_" + std::to_string(m_nextZoneId++);
    m_dockZones[zoneId] = zone;
    m_customZones[zoneId] = config;
    
    return zoneId;
}

bool SparkDockingSystem::RemoveDockZone(const std::string& zoneId) {
    auto it = m_dockZones.find(zoneId);
    if (it == m_dockZones.end()) {
        return false;
    }
    
    // Move all panels in this zone to floating
    for (const auto& panelId : it->second.panelIds) {
        m_floatingPanels.push_back(panelId);
    }
    
    m_dockZones.erase(it);
    m_customZones.erase(zoneId);
    return true;
}

DockZone* SparkDockingSystem::GetDockZone(const std::string& zoneId) {
    auto it = m_dockZones.find(zoneId);
    return (it != m_dockZones.end()) ? &it->second : nullptr;
}

void SparkDockingSystem::SetViewportBounds(float x, float y, float width, float height) {
    m_viewportX = x;
    m_viewportY = y;
    m_viewportWidth = width;
    m_viewportHeight = height;
    
    CalculateZonePositions();
}

bool SparkDockingSystem::IsPanelBeingDragged(const std::string& panelId) const {
    return m_isDragging && m_draggedPanelId == panelId;
}

void SparkDockingSystem::RenderDockZones() {
    for (auto& [zoneId, zone] : m_dockZones) {
        if (zone.isActive && !zone.panelIds.empty()) {
            RenderDockZone(zoneId, zone);
        }
    }
}

void SparkDockingSystem::RenderDockZone(const std::string& zoneId, DockZone& zone) {
    if (zone.panelIds.empty()) {
        return;
    }
    
    // Set next window position and size
    ImGui::SetNextWindowPos(ImVec2(zone.x, zone.y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(zone.width, zone.height), ImGuiCond_Always);
    
    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | 
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar;
    
    if (zone.allowTabbing && zone.panelIds.size() > 1) {
        RenderTabbedZone(zoneId, zone);
    } else if (!zone.panelIds.empty()) {
        // Single panel in zone
        auto panel = GetPanel(zone.panelIds[0]);
        if (panel && panel->IsVisible()) {
            if (ImGui::Begin((zone.name + "##" + zoneId).c_str(), nullptr, windowFlags)) {
                panel->Render();
            }
            ImGui::End();
        }
    }
}

void SparkDockingSystem::RenderTabbedZone(const std::string& zoneId, DockZone& zone) {
    ImGui::SetNextWindowPos(ImVec2(zone.x, zone.y), ImGuiCond_Always);
    
    // Adjust size based on minimization state
    float currentHeight = zone.isMinimized ? zone.minimizedHeight : zone.height;
    ImGui::SetNextWindowSize(ImVec2(zone.width, currentHeight), ImGuiCond_Always);
    
    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | 
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar;
    
    if (ImGui::Begin((zone.name + "##" + zoneId).c_str(), nullptr, windowFlags)) {
        // Render tab bar with minimize/restore button
        if (ImGui::BeginTabBar(("TabBar##" + zoneId).c_str())) {
            
            // Add minimize/restore button
            ImGui::SameLine(ImGui::GetWindowWidth() - 80);
            if (ImGui::SmallButton(zone.isMinimized ? "?##restore" : "?##minimize")) {
                ToggleZoneMinimization(zoneId);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("?##options")) {
                // Show zone options menu
                ImGui::OpenPopup("ZoneOptions");
            }
            
            // Zone options popup
            if (ImGui::BeginPopup("ZoneOptions")) {
                if (ImGui::MenuItem("Minimize All Tabs")) {
                    MinimizeAllZones();
                }
                if (ImGui::MenuItem("Restore All Tabs")) {
                    RestoreAllZones();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Reset Zone Size")) {
                    ResetZoneSize(zoneId);
                }
                ImGui::EndPopup();
            }
            
            // Only render tab content if not minimized
            if (!zone.isMinimized) {
                for (int i = 0; i < static_cast<int>(zone.panelIds.size()); ++i) {
                    const auto& panelId = zone.panelIds[i];
                    auto panel = GetPanel(panelId);
                    if (!panel || !panel->IsVisible()) {
                        continue;
                    }
                    
                    bool isSelected = (i == zone.activeTabIndex);
                    ImGuiTabItemFlags tabFlags = 0;
                    if (isSelected) {
                        tabFlags |= ImGuiTabItemFlags_SetSelected;
                    }
                    
                    bool tabOpen = true;
                    bool hasCloseButton = panel->IsClosable() && m_tabFeatures.showCloseButtons;
                    
                    if (ImGui::BeginTabItem((panel->GetName() + "##" + panelId).c_str(), 
                                          hasCloseButton ? &tabOpen : nullptr, tabFlags)) {
                        zone.activeTabIndex = i;
                        
                        // Show unsaved indicator if enabled
                        if (m_tabFeatures.enableUnsavedIndicators && panel->IsModified()) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "*");
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip("Panel has unsaved changes");
                            }
                        }
                        
                        // Render panel content
                        panel->Render();
                        
                        // Handle tab dragging for reordering
                        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                            if (!m_isDragging && m_tabFeatures.enableTabReordering) {
                                m_isDragging = true;
                                m_draggedPanelId = panelId;
                                m_dragStartPos = ImGui::GetMousePos();
                            }
                        }
                        
                        // Enhanced tab tooltips
                        if (m_tabFeatures.enableTabTooltips && ImGui::IsItemHovered()) {
                            RenderTabTooltip(panel, zoneId);
                        }
                        
                        // Enhanced right-click context menu for tabs
                        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                            ImGui::OpenPopup(("AdvancedTabContext##" + panelId).c_str());
                        }
                        
                        if (ImGui::BeginPopup(("AdvancedTabContext##" + panelId).c_str())) {
                            RenderAdvancedTabContextMenu(zoneId, panelId, panel);
                            ImGui::EndPopup();
                        }
                        
                        ImGui::EndTabItem();
                    }
                    
                    // Handle tab close
                    if (!tabOpen) {
                        zone.panelIds.erase(zone.panelIds.begin() + i);
                        if (zone.activeTabIndex >= static_cast<int>(zone.panelIds.size())) {
                            zone.activeTabIndex = std::max(0, static_cast<int>(zone.panelIds.size()) - 1);
                        }
                        break;
                    }
                }
            } else {
                // Minimized state - show tab titles only
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), 
                                  "Zone minimized - %zu panels", zone.panelIds.size());
            }
            
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

void SparkDockingSystem::ToggleZoneMinimization(const std::string& zoneId) {
    auto* zone = GetDockZone(zoneId);
    if (!zone) return;
    
    if (zone->isMinimized) {
        // Restore zone
        zone->isMinimized = false;
        zone->height = zone->restoredHeight;
        
        // Animate restoration if enabled
        if (m_enableAnimations) {
            StartMinimizationAnimation(zoneId, false);
        }
    } else {
        // Minimize zone
        zone->restoredHeight = zone->height; // Save current height
        zone->isMinimized = true;
        zone->height = zone->minimizedHeight;
        
        // Animate minimization if enabled
        if (m_enableAnimations) {
            StartMinimizationAnimation(zoneId, true);
        }
    }
    
    // Recalculate layout
    CalculateZonePositions();
}

void SparkDockingSystem::MinimizeAllZones() {
    for (auto& [zoneId, zone] : m_dockZones) {
        if (!zone.isMinimized && !zone.panelIds.empty()) {
            zone.restoredHeight = zone.height;
            zone.isMinimized = true;
            zone.height = zone.minimizedHeight;
        }
    }
    CalculateZonePositions();
}

void SparkDockingSystem::RestoreAllZones() {
    for (auto& [zoneId, zone] : m_dockZones) {
        if (zone.isMinimized) {
            zone.isMinimized = false;
            zone.height = zone.restoredHeight;
        }
    }
    CalculateZonePositions();
}

void SparkDockingSystem::ResetZoneSize(const std::string& zoneId) {
    auto* zone = GetDockZone(zoneId);
    if (!zone) return;
    
    // Reset to default size based on position
    switch (zone->position) {
        case DockPosition::Left:
        case DockPosition::Right:
            zone->width = 300.0f;
            zone->height = m_viewportHeight;
            break;
        case DockPosition::Top:
        case DockPosition::Bottom:
            zone->width = m_viewportWidth - 600.0f;
            zone->height = zone->position == DockPosition::Top ? 100.0f : 200.0f;
            break;
        case DockPosition::Center:
            zone->width = m_viewportWidth - 600.0f;
            zone->height = m_viewportHeight - 300.0f;
            break;
    }
    
    zone->isMinimized = false;
    zone->restoredHeight = zone->height;
    CalculateZonePositions();
}

void SparkDockingSystem::CloseOtherTabsInZone(const std::string& zoneId, const std::string& keepPanelId) {
    auto* zone = GetDockZone(zoneId);
    if (!zone) return;
    
    // Close all panels except the specified one
    for (const auto& panelId : zone->panelIds) {
        if (panelId != keepPanelId) {
            auto panel = GetPanel(panelId);
            if (panel) {
                panel->SetVisible(false);
            }
        }
    }
    
    // Remove closed panels from zone
    zone->panelIds.erase(
        std::remove_if(zone->panelIds.begin(), zone->panelIds.end(),
            [this](const std::string& panelId) {
                auto panel = GetPanel(panelId);
                return !panel || !panel->IsVisible();
            }),
        zone->panelIds.end()
    );
    
    // Reset active tab index
    zone->activeTabIndex = 0;
}

void SparkDockingSystem::StartMinimizationAnimation(const std::string& zoneId, bool minimize) {
    if (!m_enableAnimations) return;
    
    // Create a quick animation for minimization
    AnimationState::ZoneAnimation anim;
    auto* zone = GetDockZone(zoneId);
    if (!zone) return;
    
    anim.startPos = ImVec2(zone->x, zone->y);
    anim.startSize = ImVec2(zone->width, minimize ? zone->restoredHeight : zone->minimizedHeight);
    anim.endPos = ImVec2(zone->x, zone->y);
    anim.endSize = ImVec2(zone->width, minimize ? zone->minimizedHeight : zone->restoredHeight);
    
    m_animation.zoneAnimations[zoneId] = anim;
    m_animation.animationTime = 0.0f;
    m_animation.animationDuration = 0.15f; // Quick animation
    m_animation.isAnimating = true;
}

void SparkDockingSystem::CreatePredefinedLayouts() {
    // Create some predefined layout configurations
    m_predefinedLayouts["Default"] = ""; // Implement layout serialization
    m_predefinedLayouts["Code"] = "";    // Code-focused layout
    m_predefinedLayouts["Art"] = "";     // Art-focused layout
    m_predefinedLayouts["Debug"] = "";   // Debug-focused layout
}

std::shared_ptr<EditorPanel> SparkDockingSystem::GetPanel(const std::string& panelId) {
    auto it = m_panels.find(panelId);
    return (it != m_panels.end()) ? it->second : nullptr;
}

std::string SparkDockingSystem::SaveLayout() const {
    // Implementation for layout serialization
    return "{}";
}

bool SparkDockingSystem::LoadLayout(const std::string& layoutData) {
    // Implementation for layout deserialization
    return true;
}

void SparkDockingSystem::UpdateAnimations(float deltaTime) {
    if (!m_animation.isAnimating) {
        return;
    }
    
    m_animation.animationTime += deltaTime;
    float t = m_animation.animationTime / m_animation.animationDuration;
    
    if (t >= 1.0f) {
        // Animation complete
        t = 1.0f;
        m_animation.isAnimating = false;
        
        // Ensure final positions are exact
        CalculateZonePositions();
    }
    
    // Apply easing function for smooth animation
    float easedT = EaseInOutCubic(t);
    
    // Interpolate zone positions and sizes
    for (auto& [zoneId, zone] : m_dockZones) {
        auto animIt = m_animation.zoneAnimations.find(zoneId);
        if (animIt != m_animation.zoneAnimations.end()) {
            const auto& anim = animIt->second;
            
            // Interpolate position
            zone.x = Lerp(anim.startPos.x, anim.endPos.x, easedT);
            zone.y = Lerp(anim.startPos.y, anim.endPos.y, easedT);
            
            // Interpolate size
            zone.width = Lerp(anim.startSize.x, anim.endSize.x, easedT);
            zone.height = Lerp(anim.startSize.y, anim.endSize.y, easedT);
        }
    }
}

float SparkDockingSystem::EaseInOutCubic(float t) {
    return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) / 2.0f;
}

float SparkDockingSystem::Lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

bool SparkDockingSystem::ApplyPredefinedLayout(const std::string& layoutName) {
    auto it = m_predefinedLayouts.find(layoutName);
    if (it == m_predefinedLayouts.end()) {
        return false;
    }
    
    if (m_enableAnimations && !m_animation.isAnimating) {
        StartLayoutTransition(m_currentLayout, layoutName);
    }
    
    m_currentLayout = layoutName;
    return LoadLayout(it->second);
}

void SparkDockingSystem::StartLayoutTransition(const std::string& fromLayout, const std::string& toLayout) {
    if (!m_enableAnimations) {
        return;
    }
    
    // Capture current positions as start state
    m_animation.fromLayout = fromLayout;
    m_animation.toLayout = toLayout;
    m_animation.animationTime = 0.0f;
    m_animation.zoneAnimations.clear();
    
    for (const auto& [zoneId, zone] : m_dockZones) {
        AnimationState::ZoneAnimation anim;
        anim.startPos = ImVec2(zone.x, zone.y);
        anim.startSize = ImVec2(zone.width, zone.height);
        
        m_animation.zoneAnimations[zoneId] = anim;
    }
    
    // Calculate target positions (simplified)
    CalculateTargetPositions(toLayout);
    
    // Set end positions for animation
    for (const auto& [zoneId, zone] : m_dockZones) {
        auto& anim = m_animation.zoneAnimations[zoneId];
        anim.endPos = ImVec2(zone.x, zone.y);
        anim.endSize = ImVec2(zone.width, zone.height);
        
        // Restore start positions for animation
        auto& mutableZone = const_cast<DockZone&>(zone);
        mutableZone.x = anim.startPos.x;
        mutableZone.y = anim.startPos.y;
        mutableZone.width = anim.startSize.x;
        mutableZone.height = anim.startSize.y;
    }
    
    m_animation.isAnimating = true;
}

void SparkDockingSystem::CalculateTargetPositions(const std::string& layoutName) {
    // This would calculate the target positions for the new layout
    // For now, we'll use the standard position calculation
    CalculateZonePositions();
}

void SparkDockingSystem::RenderFloatingPanels() {
    for (const auto& panelId : m_floatingPanels) {
        auto panel = GetPanel(panelId);
        if (!panel || !panel->IsVisible()) {
            continue;
        }
        
        // Get panel position and size
        float x, y, width, height;
        panel->GetPosition(x, y);
        panel->GetSize(width, height);
        
        // Apply floating window constraints
        ApplyFloatingConstraints(x, y, width, height);
        
        ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_FirstUseEver);
        
        // Set size constraints
        ImGui::SetNextWindowSizeConstraints(
            m_floatingConstraints.minSize,
            m_floatingConstraints.maxSize
        );
        
        bool isOpen = true;
        ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoCollapse;
        
        // Add docking hint when dragging
        if (m_isDragging && m_draggedPanelId == panelId) {
            windowFlags |= ImGuiWindowFlags_NoMove;
        }
        
        if (ImGui::Begin((panel->GetName() + "##" + panelId).c_str(), 
                        panel->IsClosable() ? &isOpen : nullptr, windowFlags)) {
            
            // Check if window is being dragged
            if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                if (ImGui::GetIO().KeyCtrl) { // Ctrl+click to start docking
                    m_isDragging = true;
                    m_draggedPanelId = panelId;
                    m_dragStartPos = ImGui::GetMousePos();
                }
            }
            
            // Get current window position and size
            ImVec2 windowPos = ImGui::GetWindowPos();
            ImVec2 windowSize = ImGui::GetWindowSize();
            
            // Apply constraints and update position if needed
            float constrainedX = windowPos.x;
            float constrainedY = windowPos.y;
            float constrainedWidth = windowSize.x;
            float constrainedHeight = windowSize.y;
            
            bool positionChanged = ApplyFloatingConstraints(constrainedX, constrainedY, 
                                                          constrainedWidth, constrainedHeight);
            
            if (positionChanged) {
                ImGui::SetWindowPos(ImVec2(constrainedX, constrainedY));
                ImGui::SetWindowSize(ImVec2(constrainedWidth, constrainedHeight));
            }
            
            // Update panel position and size
            panel->SetPosition(constrainedX, constrainedY);
            panel->SetSize(constrainedWidth, constrainedHeight);
            
            // Show docking hint overlay when dragging
            if (m_isDragging && m_draggedPanelId == panelId) {
                RenderFloatingDockHint(windowPos, windowSize);
            }
            
            // Render panel content
            panel->Render();
        }
        ImGui::End();
        
        // Handle panel close
        if (!isOpen) {
            auto it = std::find(m_floatingPanels.begin(), m_floatingPanels.end(), panelId);
            if (it != m_floatingPanels.end()) {
                m_floatingPanels.erase(it);
            }
            panel->SetVisible(false);
        }
    }
}

bool SparkDockingSystem::ApplyFloatingConstraints(float& x, float& y, float& width, float& height) {
    bool changed = false;
    
    if (!m_floatingConstraints.enableBoundaries) {
        return false;
    }
    
    // Constrain size
    if (width < m_floatingConstraints.minSize.x) {
        width = m_floatingConstraints.minSize.x;
        changed = true;
    }
    if (height < m_floatingConstraints.minSize.y) {
        height = m_floatingConstraints.minSize.y;
        changed = true;
    }
    if (width > m_floatingConstraints.maxSize.x) {
        width = m_floatingConstraints.maxSize.x;
        changed = true;
    }
    if (height > m_floatingConstraints.maxSize.y) {
        height = m_floatingConstraints.maxSize.y;
        changed = true;
    }
    
    // Prevent offscreen movement
    if (m_floatingConstraints.preventOffscreen) {
        // Ensure window title bar is always visible
        float minX = m_viewportX - width + 100.0f; // Allow 100px of window to remain visible
        float maxX = m_viewportX + m_viewportWidth - 100.0f;
        float minY = m_viewportY;
        float maxY = m_viewportY + m_viewportHeight - m_floatingConstraints.titleBarHeight;
        
        if (x < minX) {
            x = minX;
            changed = true;
        }
        if (x > maxX) {
            x = maxX;
            changed = true;
        }
        if (y < minY) {
            y = minY;
            changed = true;
        }
        if (y > maxY) {
            y = maxY;
            changed = true;
        }
    }
    
    // Snap to edges
    if (m_floatingConstraints.snapToEdges) {
        float snapDist = m_floatingConstraints.snapDistance;
        
        // Snap to viewport edges
        if (std::abs(x - m_viewportX) < snapDist) {
            x = m_viewportX;
            changed = true;
        }
        if (std::abs(y - m_viewportY) < snapDist) {
            y = m_viewportY;
            changed = true;
        }
        if (std::abs((x + width) - (m_viewportX + m_viewportWidth)) < snapDist) {
            x = m_viewportX + m_viewportWidth - width;
            changed = true;
        }
        if (std::abs((y + height) - (m_viewportY + m_viewportHeight)) < snapDist) {
            y = m_viewportY + m_viewportHeight - height;
            changed = true;
        }
        
        // Snap to other floating windows
        for (const auto& otherId : m_floatingPanels) {
            auto other = GetPanel(otherId);
            if (!other || !other->IsVisible()) continue;
            
            float otherX, otherY, otherW, otherH;
            other->GetPosition(otherX, otherY);
            other->GetSize(otherW, otherH);
            
            // Horizontal edge snapping
            if (std::abs(x - (otherX + otherW)) < snapDist && 
                y < (otherY + otherH) && (y + height) > otherY) {
                x = otherX + otherW;
                changed = true;
            }
            if (std::abs((x + width) - otherX) < snapDist && 
                y < (otherY + otherH) && (y + height) > otherY) {
                x = otherX - width;
                changed = true;
            }
            
            // Vertical edge snapping
            if (std::abs(y - (otherY + otherH)) < snapDist && 
                x < (otherX + otherW) && (x + width) > otherX) {
                y = otherY + otherH;
                changed = true;
            }
            if (std::abs((y + height) - otherY) < snapDist && 
                x < (otherX + otherW) && (x + width) > otherX) {
                y = otherY - height;
                changed = true;
            }
        }
    }
    
    return changed;
}

void SparkDockingSystem::RenderFloatingDockHint(ImVec2 windowPos, ImVec2 windowSize) {
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    
    // Draw semi-transparent overlay on the floating window
    ImU32 overlayColor = IM_COL32(100, 150, 255, 50);
    drawList->AddRectFilled(windowPos, 
                           ImVec2(windowPos.x + windowSize.x, windowPos.y + windowSize.y), 
                           overlayColor);
    
    // Draw border
    drawList->AddRect(windowPos, 
                     ImVec2(windowPos.x + windowSize.x, windowPos.y + windowSize.y), 
                     m_dockGuideColor, 0.0f, 0, 2.0f);
    
    // Draw dock hint text
    ImVec2 center(windowPos.x + windowSize.x * 0.5f, windowPos.y + windowSize.y * 0.5f);
    const char* hintText = "Ctrl+Drag to dock";
    ImVec2 textSize = ImGui::CalcTextSize(hintText);
    ImVec2 textPos(center.x - textSize.x * 0.5f, center.y - textSize.y * 0.5f);
    
    // Draw text background
    ImVec2 textBgMin(textPos.x - 10, textPos.y - 5);
    ImVec2 textBgMax(textPos.x + textSize.x + 10, textPos.y + textSize.y + 5);
    drawList->AddRectFilled(textBgMin, textBgMax, IM_COL32(0, 0, 0, 150));
    
    drawList->AddText(textPos, IM_COL32(255, 255, 255, 255), hintText);
}

void SparkDockingSystem::CloseTabsToRight(const std::string& zoneId, const std::string& panelId) {
    auto* zone = GetDockZone(zoneId);
    if (!zone) return;
    
    // Find the position of the specified panel
    auto it = std::find(zone->panelIds.begin(), zone->panelIds.end(), panelId);
    if (it == zone->panelIds.end()) return;
    
    int panelIndex = static_cast<int>(it - zone->panelIds.begin());
    
    // Close all panels to the right
    for (int i = static_cast<int>(zone->panelIds.size()) - 1; i > panelIndex; --i) {
        auto panel = GetPanel(zone->panelIds[i]);
        if panel ? panel->SetVisible(false), void() : void();
        zone->panelIds.erase(zone->panelIds.begin() + i);
    }
    
    // Adjust active tab index if needed
    if (zone->activeTabIndex >= static_cast<int>(zone->panelIds.size())) {
        zone->activeTabIndex = std::max(0, static_cast<int>(zone->panelIds.size()) - 1);
    }
}

void SparkDockingSystem::CloseAllButThisTab(const std::string& zoneId, const std::string& panelId) {
    auto* zone = GetDockZone(zoneId);
    if (!zone) return;
    
    // Close all panels except the specified one
    for (auto it = zone->panelIds.begin(); it != zone->panelIds.end();) {
        if (*it != panelId) {
            auto panel = GetPanel(*it);
            if (panel) {
                panel->SetVisible(false);
            }
            it = zone->panelIds.erase(it);
        } else {
            ++it;
        }
    }
    
    // Reset active tab index
    zone->activeTabIndex = 0;
}

void SparkDockingSystem::MoveTabLeft(const std::string& zoneId, const std::string& panelId) {
    DockZone* zone = GetDockZone(zoneId);
    if (!zone) return;
    
    // Find panel index manually
    int panelIndex = -1;
    for (size_t i = 0; i < zone->panelIds.size(); ++i) {
        if (zone->panelIds[i] == panelId) {
            panelIndex = static_cast<int>(i);
            break;
        }
    }
    
    if (panelIndex > 0) {
        std::swap(zone->panelIds[panelIndex], zone->panelIds[panelIndex - 1]);
        zone->activeTabIndex = panelIndex - 1;
    }
}

void SparkDockingSystem::MoveTabRight(const std::string& zoneId, const std::string& panelId) {
    DockZone* zone = GetDockZone(zoneId);
    if (!zone) return;
    
    // Find panel index manually
    int panelIndex = -1;
    for (size_t i = 0; i < zone->panelIds.size(); ++i) {
        if (zone->panelIds[i] == panelId) {
            panelIndex = static_cast<int>(i);
            break;
        }
    }
    
    if (panelIndex >= 0 && panelIndex < static_cast<int>(zone->panelIds.size()) - 1) {
        std::swap(zone->panelIds[panelIndex], zone->panelIds[panelIndex + 1]);
        zone->activeTabIndex = panelIndex + 1;
    }
}

void SparkDockingSystem::MoveTabToPosition(const std::string& zoneId, const std::string& panelId, int position) {
    auto* zone = GetDockZone(zoneId);
    if (!zone) return;
    
    auto it = std::find(zone->panelIds.begin(), zone->panelIds.end(), panelId);
    if (it != zone->panelIds.end()) {
        std::string panel = *it;
        zone->panelIds.erase(it);
        
        position = (position < 0) ? 0 : ((position > static_cast<int>(zone->panelIds.size())) ? static_cast<int>(zone->panelIds.size()) : position);
        zone->panelIds.insert(zone->panelIds.begin() + position, panel);
        zone->activeTabIndex = position;
    }
}

void SparkDockingSystem::MoveTabToLast(const std::string& zoneId, const std::string& panelId) {
    auto* zone = GetDockZone(zoneId);
    if (!zone) return;
    
    auto it = std::find(zone->panelIds.begin(), zone->panelIds.end(), panelId);
    if (it != zone->panelIds.end()) {
        std::string panel = *it;
        zone->panelIds.erase(it);
        zone->panelIds.push_back(panel);
        zone->activeTabIndex = static_cast<int>(zone->panelIds.size()) - 1;
    }
}

void SparkDockingSystem::MoveTabToZone(const std::string& panelId, const std::string& fromZoneId, const std::string& toZoneId) {
    auto* fromZone = GetDockZone(fromZoneId);
    auto* toZone = GetDockZone(toZoneId);
    if (!fromZone || !toZone) return;
    
    auto it = std::find(fromZone->panelIds.begin(), fromZone->panelIds.end(), panelId);
    if (it != fromZone->panelIds.end()) {
        fromZone->panelIds.erase(it);
        toZone->panelIds.push_back(panelId);
        toZone->activeTabIndex = static_cast<int>(toZone->panelIds.size()) - 1;
        
        // Adjust from zone active tab index
        if (fromZone->activeTabIndex >= static_cast<int>(fromZone->panelIds.size())) {
            fromZone->activeTabIndex = std::max(0, static_cast<int>(fromZone->panelIds.size()) - 1);
        }
    }
}

void SparkDockingSystem::CreateNewZoneWithPanel(const std::string& panelId) {
    // Create a new custom zone with the panel
    CustomZoneConfig config;
    config.name = "NewZone_" + std::to_string(m_nextZoneId);
    config.displayName = "New Zone";
    config.basePosition = DockPosition::Floating;
    config.size = ImVec2(400.0f, 300.0f);
    config.offset = ImVec2(100.0f, 100.0f);
    
    std::string newZoneId = CreateCustomDockZone(config);
    DockPanel(panelId, DockPosition::Center, newZoneId);
}

void SparkDockingSystem::CreateTabGroup(const std::string& zoneId, const std::vector<std::string>& panelIds) {
    // For now, just move all panels to the same zone
    auto* zone = GetDockZone(zoneId);
    if (!zone) return;
    
    for (const auto& panelId : panelIds) {
        if (std::find(zone->panelIds.begin(), zone->panelIds.end(), panelId) == zone->panelIds.end()) {
            zone->panelIds.push_back(panelId);
        }
    }
}

void SparkDockingSystem::ShowTabColorDialog(const std::string& panelId) {
    // TODO: Implement tab color customization
    // For now, this is a placeholder
}

void SparkDockingSystem::RefreshPanel(const std::string& panelId) {
    auto panel = GetPanel(panelId);
    if (panel) {
        // TODO: Implement panel refresh functionality
        // This would typically call panel->Refresh() or similar
    }
}

void SparkDockingSystem::SavePanel(const std::string& panelId) {
    auto panel = GetPanel(panelId);
    if (panel) {
        // TODO: Implement panel save functionality
        // This would typically call panel->Save() or similar
    }
}

void SparkDockingSystem::ResetPanel(const std::string& panelId) {
    auto panel = GetPanel(panelId);
    if (panel) {
        // TODO: Implement panel reset functionality
        // This would typically call panel->Reset() or similar
    }
}

void SparkDockingSystem::RenderZoneEditDialog() {
    if (!m_showZoneEditDialog || m_editingZoneId.empty()) {
        return;
    }
    
    auto* zone = GetDockZone(m_editingZoneId);
    if (!zone) return;
    
    if (ImGui::Begin("Edit Dock Zone")) {
        ImGui::Text("Zone: %s", zone->name.c_str());
        ImGui::Separator();
        
        // Basic settings
        char nameBuffer[256];
        strncpy_s(nameBuffer, sizeof(nameBuffer), zone->name.c_str(), sizeof(nameBuffer) - 1);
        if (ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer))) {
            zone->name = nameBuffer;
        }
        
        float position[2] = { zone->x, zone->y };
        if (ImGui::DragFloat2("Position", position, 1.0f)) {
            zone->x = position[0];
            zone->y = position[1];
        }
        
        float size[2] = { zone->width, zone->height };
        if (ImGui::DragFloat2("Size", size, 1.0f, 100.0f, 2000.0f)) {
            zone->width = size[0];
            zone->height = size[1];
        }
        
        if (ImGui::SliderFloat("Split Ratio", &zone->splitRatio, 0.1f, 0.9f)) {
            // Split ratio changed
        }
        
        if (ImGui::Checkbox("Allow Tabbing", &zone->allowTabbing)) {
            // Checkbox state changed
        }
        if (ImGui::Checkbox("Allow Splitting", &zone->allowSplitting)) {
            // Checkbox state changed
        }
        
        // Custom zone specific settings
        auto customIt = m_customZones.find(m_editingZoneId);
        if (customIt != m_customZones.end()) {
            ImGui::Separator();
            ImGui::Text("Custom Zone Settings:");
            
            auto& config = customIt->second;
            if (ImGui::ColorEdit4("Background", &config.backgroundColor.x)) {
                // Color changed
            }
            if (ImGui::ColorEdit4("Border", &config.borderColor.x)) {
                // Color changed
            }
            if (ImGui::Checkbox("Persistent", &config.isPersistent)) {
                // Checkbox state changed
            }
            if (ImGui::Checkbox("Allow Resize", &config.allowResize)) {
                // Checkbox state changed
            }
            if (ImGui::Checkbox("Allow Move", &config.allowMove)) {
                // Checkbox state changed
            }
        }
        
        ImGui::End();
    }
}

} // namespace SparkEditor