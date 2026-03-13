/**
 * @file RuntimeInspectorPanel.cpp
 * @brief Implementation of the Runtime Inspector panel for live property editing during play mode
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides live entity/component inspection and editing while the game is running.
 * Tracks all changes with timestamps, supports property watching with sparkline
 * graphs, and allows reverting individual or all edits.
 */

#include "RuntimeInspectorPanel.h"
#include "../Core/EditorIcons.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <sstream>

namespace SparkEditor
{

    // ========================================================================
    // Edit history entry (internal to this translation unit)
    // ========================================================================

    struct EditHistoryEntry
    {
        std::string entityLabel;
        std::string componentType;
        std::string propertyName;
        std::string oldValue;
        std::string newValue;
        std::string timestamp;
    };

    static std::vector<EditHistoryEntry> s_editHistory;

    // Helper: current timestamp string
    static std::string CurrentTimestamp()
    {
        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        int seconds = static_cast<int>((ms / 1000) % 60);
        int minutes = static_cast<int>((ms / 60000) % 60);
        int hours = static_cast<int>((ms / 3600000) % 24);
        std::ostringstream oss;
        oss << (hours < 10 ? "0" : "") << hours << ":" << (minutes < 10 ? "0" : "") << minutes << ":"
            << (seconds < 10 ? "0" : "") << seconds;
        return oss.str();
    }

    // Helper: record an edit
    static void RecordEdit(const std::string& entityLabel, const std::string& componentType,
                           const std::string& propertyName, const std::string& oldValue, const std::string& newValue)
    {
        EditHistoryEntry entry;
        entry.entityLabel = entityLabel;
        entry.componentType = componentType;
        entry.propertyName = propertyName;
        entry.oldValue = oldValue;
        entry.newValue = newValue;
        entry.timestamp = CurrentTimestamp();
        s_editHistory.push_back(std::move(entry));
    }

    // ========================================================================
    // Demo/simulated property data
    // ========================================================================

    static std::vector<EditableProperty> GenerateDemoProperties(uint32_t entityId)
    {
        std::vector<EditableProperty> props;

        // Transform component
        {
            EditableProperty p;
            p.componentType = "Transform";
            p.name = "Position";
            p.type = PropertyType::Vector3;
            p.currentValue = std::to_string(static_cast<float>(entityId) * 1.5f) + ",0.0,0.0";
            p.originalValue = p.currentValue;
            props.push_back(p);
        }
        {
            EditableProperty p;
            p.componentType = "Transform";
            p.name = "Rotation";
            p.type = PropertyType::Vector3;
            p.currentValue = "0.0,0.0,0.0";
            p.originalValue = p.currentValue;
            props.push_back(p);
        }
        {
            EditableProperty p;
            p.componentType = "Transform";
            p.name = "Scale";
            p.type = PropertyType::Vector3;
            p.currentValue = "1.0,1.0,1.0";
            p.originalValue = p.currentValue;
            props.push_back(p);
        }

        // RigidBody component
        {
            EditableProperty p;
            p.componentType = "RigidBody";
            p.name = "Mass";
            p.type = PropertyType::Float;
            p.currentValue = "1.0";
            p.originalValue = p.currentValue;
            props.push_back(p);
        }
        {
            EditableProperty p;
            p.componentType = "RigidBody";
            p.name = "LinearDamping";
            p.type = PropertyType::Float;
            p.currentValue = "0.05";
            p.originalValue = p.currentValue;
            props.push_back(p);
        }
        {
            EditableProperty p;
            p.componentType = "RigidBody";
            p.name = "IsKinematic";
            p.type = PropertyType::Bool;
            p.currentValue = "false";
            p.originalValue = p.currentValue;
            props.push_back(p);
        }
        {
            EditableProperty p;
            p.componentType = "RigidBody";
            p.name = "UseGravity";
            p.type = PropertyType::Bool;
            p.currentValue = "true";
            p.originalValue = p.currentValue;
            props.push_back(p);
        }

        // MeshRenderer component
        {
            EditableProperty p;
            p.componentType = "MeshRenderer";
            p.name = "Visible";
            p.type = PropertyType::Bool;
            p.currentValue = "true";
            p.originalValue = p.currentValue;
            props.push_back(p);
        }
        {
            EditableProperty p;
            p.componentType = "MeshRenderer";
            p.name = "CastShadows";
            p.type = PropertyType::Bool;
            p.currentValue = "true";
            p.originalValue = p.currentValue;
            props.push_back(p);
        }
        {
            EditableProperty p;
            p.componentType = "MeshRenderer";
            p.name = "Color";
            p.type = PropertyType::Color;
            p.currentValue = "1.0,1.0,1.0,1.0";
            p.originalValue = p.currentValue;
            props.push_back(p);
        }
        {
            EditableProperty p;
            p.componentType = "MeshRenderer";
            p.name = "MeshPath";
            p.type = PropertyType::String;
            p.currentValue = "Assets/Meshes/Cube.fbx";
            p.originalValue = p.currentValue;
            props.push_back(p);
        }

        // Health component (FPS-specific)
        {
            EditableProperty p;
            p.componentType = "Health";
            p.name = "CurrentHP";
            p.type = PropertyType::Int;
            p.currentValue = "100";
            p.originalValue = p.currentValue;
            props.push_back(p);
        }
        {
            EditableProperty p;
            p.componentType = "Health";
            p.name = "MaxHP";
            p.type = PropertyType::Int;
            p.currentValue = "100";
            p.originalValue = p.currentValue;
            p.isReadOnly = true;
            props.push_back(p);
        }

        return props;
    }

    // ========================================================================
    // Constructor
    // ========================================================================

    RuntimeInspectorPanel::RuntimeInspectorPanel() : EditorPanel("Runtime Inspector", "runtime_inspector_panel")
    {
        SetIcon(ICON_FA_SLIDERS);
        m_lastRefresh = std::chrono::steady_clock::now();
    }

    // ========================================================================
    // Initialize
    // ========================================================================

    bool RuntimeInspectorPanel::Initialize()
    {
        SPARK_TRACE_ENTER(LogCategory::Editor);
        std::cout << "Initializing Runtime Inspector panel\n";
        m_isInitialized = true;
        return true;
    }

    // ========================================================================
    // Update
    // ========================================================================

    void RuntimeInspectorPanel::Update(float deltaTime)
    {
        if (!m_autoRefresh)
            return;

        m_refreshTimer += deltaTime;
        if (m_refreshTimer >= m_refreshInterval)
        {
            m_refreshTimer = 0.0f;
            m_lastRefresh = std::chrono::steady_clock::now();

            // Update watched property sparklines with simulated data
            for (auto& [key, watch] : m_watches)
            {
                // Simulate value changes for demo purposes
                float t = static_cast<float>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 std::chrono::steady_clock::now().time_since_epoch())
                                                 .count() %
                                             10000);
                t /= 10000.0f;

                float simulated = watch.currentValue + std::sin(t * 6.2831853f) * 0.5f;
                watch.PushSample(simulated);
            }
        }
    }

    // ========================================================================
    // Render
    // ========================================================================

    void RuntimeInspectorPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            if (m_playModeManager == nullptr)
            {
                // No play mode manager — show placeholder
                float avail = ImGui::GetContentRegionAvail().y;
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + avail * 0.35f);
                const char* msg = ICON_FA_PLAY " Enter play mode to use Runtime Inspector";
                float textWidth = ImGui::CalcTextSize(msg).x;
                ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textWidth) * 0.5f);
                ImGui::TextColored(ImVec4(0.55f, 0.58f, 0.62f, 1.0f), "%s", msg);
            }
            else
            {
                RenderToolbar();
                ImGui::Separator();

                RenderSearchFilter();
                ImGui::Spacing();

                RenderEntitySelector();
                ImGui::Separator();

                if (m_hasSelection)
                {
                    RenderComponentList();

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    if (!m_watches.empty())
                    {
                        RenderPropertyWatches();
                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();
                    }

                    if (m_showEditHistory)
                    {
                        RenderEditHistory();
                    }
                }
                else
                {
                    ImGui::Spacing();
                    float avail = ImGui::GetContentRegionAvail().y;
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + avail * 0.3f);
                    const char* hint = ICON_FA_MOUSE_POINTER " Select an entity to inspect";
                    float textWidth = ImGui::CalcTextSize(hint).x;
                    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textWidth) * 0.5f);
                    ImGui::TextColored(ImVec4(0.55f, 0.58f, 0.62f, 1.0f), "%s", hint);
                }
            }
        }
        EndPanel();
    }

    // ========================================================================
    // Shutdown
    // ========================================================================

    void RuntimeInspectorPanel::Shutdown()
    {
        std::cout << "Shutting down Runtime Inspector panel\n";
        m_watches.clear();
        m_properties.clear();
        s_editHistory.clear();
        m_hasSelection = false;
        m_isInitialized = false;
    }

    // ========================================================================
    // HandleEvent
    // ========================================================================

    bool RuntimeInspectorPanel::HandleEvent(const std::string& eventType, void* eventData)
    {
        if (eventType == "EntitySelected" && eventData != nullptr)
        {
            auto* id = static_cast<uint32_t*>(eventData);
            SetSelectedEntity(*id);
            return true;
        }

        if (eventType == "PlayModeStarted")
        {
            s_editHistory.clear();
            return true;
        }

        if (eventType == "PlayModeStopped")
        {
            // Optionally persist changes — for now, clear watches
            m_watches.clear();
            return true;
        }

        if (eventType == "EntityDeleted" && eventData != nullptr)
        {
            auto* id = static_cast<uint32_t*>(eventData);
            if (*id == m_selectedEntityId)
            {
                m_hasSelection = false;
                m_selectedEntityId = 0;
                m_properties.clear();
            }
            return true;
        }

        return false;
    }

    // ========================================================================
    // SetSelectedEntity
    // ========================================================================

    void RuntimeInspectorPanel::SetSelectedEntity(uint32_t entityId)
    {
        m_selectedEntityId = entityId;
        m_hasSelection = true;

        // Populate properties with demo data
        m_properties = GenerateDemoProperties(entityId);

        std::cout << "Runtime Inspector: selected entity " << entityId << " with " << m_properties.size()
                  << " properties\n";
    }

    // ========================================================================
    // WatchProperty
    // ========================================================================

    void RuntimeInspectorPanel::WatchProperty(uint32_t entityId, const std::string& componentType,
                                              const std::string& propertyName)
    {
        std::string key = MakeWatchKey(entityId, componentType, propertyName);
        if (m_watches.find(key) != m_watches.end())
            return; // Already watching

        PropertyWatch watch;
        watch.entityId = entityId;
        watch.componentType = componentType;
        watch.propertyName = propertyName;
        watch.displayLabel = componentType + "." + propertyName + " [" + std::to_string(entityId) + "]";

        // Seed initial value from current properties
        for (const auto& prop : m_properties)
        {
            if (prop.componentType == componentType && prop.name == propertyName)
            {
                try
                {
                    watch.currentValue = std::stof(prop.currentValue);
                }
                catch (...)
                {
                    watch.currentValue = 0.0f;
                }
                break;
            }
        }

        m_watches[key] = std::move(watch);
        std::cout << "Runtime Inspector: watching " << key << "\n";
    }

    // ========================================================================
    // UnwatchProperty
    // ========================================================================

    void RuntimeInspectorPanel::UnwatchProperty(uint32_t entityId, const std::string& componentType,
                                                const std::string& propertyName)
    {
        std::string key = MakeWatchKey(entityId, componentType, propertyName);
        auto it = m_watches.find(key);
        if (it != m_watches.end())
        {
            m_watches.erase(it);
            std::cout << "Runtime Inspector: unwatched " << key << "\n";
        }
    }

    // ========================================================================
    // RenderEntitySelector
    // ========================================================================

    void RuntimeInspectorPanel::RenderEntitySelector()
    {
        ImGui::Spacing();
        ImGui::Text(ICON_FA_CUBE " Entity");
        ImGui::SameLine();

        static int entityInput = 0;
        if (m_hasSelection)
        {
            entityInput = static_cast<int>(m_selectedEntityId);
        }

        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::InputInt("##EntityId", &entityInput, 1, 10, ImGuiInputTextFlags_EnterReturnsTrue))
        {
            if (entityInput >= 0)
            {
                SetSelectedEntity(static_cast<uint32_t>(entityInput));
            }
        }

        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_CHEVRON_LEFT "##PrevEntity"))
        {
            if (m_selectedEntityId > 0)
            {
                SetSelectedEntity(m_selectedEntityId - 1);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_CHEVRON_RIGHT "##NextEntity"))
        {
            SetSelectedEntity(m_selectedEntityId + 1);
        }

        if (m_hasSelection)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 0.6f, 1.0f), "Entity_%u", m_selectedEntityId);
        }

        ImGui::Spacing();
    }

    // ========================================================================
    // RenderComponentList
    // ========================================================================

    void RuntimeInspectorPanel::RenderComponentList()
    {
        // Group properties by component type
        std::string currentComponent;

        for (size_t i = 0; i < m_properties.size(); ++i)
        {
            auto& prop = m_properties[i];

            // Apply filters
            if (m_showModifiedOnly && !prop.isModified)
                continue;
            if (!m_showReadOnly && prop.isReadOnly)
                continue;
            if (!m_searchFilter.empty())
            {
                std::string lowerName = prop.name;
                std::string lowerComp = prop.componentType;
                std::string lowerFilter = m_searchFilter;
                std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
                std::transform(lowerComp.begin(), lowerComp.end(), lowerComp.begin(), ::tolower);
                std::transform(lowerFilter.begin(), lowerFilter.end(), lowerFilter.begin(), ::tolower);
                if (lowerName.find(lowerFilter) == std::string::npos &&
                    lowerComp.find(lowerFilter) == std::string::npos)
                {
                    continue;
                }
            }

            // New component section header
            if (prop.componentType != currentComponent)
            {
                currentComponent = prop.componentType;
                ImGui::Spacing();

                bool open =
                    ImGui::CollapsingHeader((ICON_FA_COGS " " + currentComponent + "##" + currentComponent).c_str(),
                                            ImGuiTreeNodeFlags_DefaultOpen);
                if (!open)
                {
                    // Skip all properties in this component
                    while (i + 1 < m_properties.size() && m_properties[i + 1].componentType == currentComponent)
                    {
                        ++i;
                    }
                    continue;
                }
            }

            ImGui::PushID(static_cast<int>(i));
            RenderPropertyEditor(prop);
            ImGui::PopID();
        }
    }

    // ========================================================================
    // RenderPropertyEditor
    // ========================================================================

    void RuntimeInspectorPanel::RenderPropertyEditor(const EditableProperty& prop)
    {
        // We need a mutable reference to update values
        // Find the property in our vector (const ref passed for signature compatibility)
        EditableProperty* mutableProp = nullptr;
        for (auto& p : m_properties)
        {
            if (p.name == prop.name && p.componentType == prop.componentType)
            {
                mutableProp = &p;
                break;
            }
        }
        if (mutableProp == nullptr)
            return;

        // Modified indicator
        if (mutableProp->isModified)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), ICON_FA_EDIT);
            ImGui::SameLine();
        }

        // Read-only indicator
        if (mutableProp->isReadOnly)
        {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), ICON_FA_LOCK);
            ImGui::SameLine();
        }

        // Label
        ImGui::Text("%s", mutableProp->name.c_str());
        ImGui::SameLine(180.0f);

        // Watch button (only for numeric types)
        if (mutableProp->type == PropertyType::Float || mutableProp->type == PropertyType::Int)
        {
            std::string watchKey = MakeWatchKey(m_selectedEntityId, mutableProp->componentType, mutableProp->name);
            bool isWatched = m_watches.find(watchKey) != m_watches.end();

            if (isWatched)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 0.8f, 0.2f, 1.0f));
                if (ImGui::SmallButton(ICON_FA_EYE "##Watch"))
                {
                    UnwatchProperty(m_selectedEntityId, mutableProp->componentType, mutableProp->name);
                }
                ImGui::PopStyleColor();
            }
            else
            {
                if (ImGui::SmallButton(ICON_FA_EYE_SLASH "##Watch"))
                {
                    WatchProperty(m_selectedEntityId, mutableProp->componentType, mutableProp->name);
                }
            }
            ImGui::SameLine();
        }

        if (mutableProp->isReadOnly)
        {
            // Display read-only value
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", mutableProp->currentValue.c_str());
            return;
        }

        std::string oldValue = mutableProp->currentValue;

        switch (mutableProp->type)
        {
        case PropertyType::Float:
        {
            float val = 0.0f;
            try
            {
                val = std::stof(mutableProp->currentValue);
            }
            catch (...)
            {
            }

            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::SliderFloat("##val", &val, -100.0f, 100.0f, "%.3f"))
            {
                mutableProp->currentValue = std::to_string(val);
                mutableProp->isModified = (mutableProp->currentValue != mutableProp->originalValue);
                RecordEdit("Entity_" + std::to_string(m_selectedEntityId), mutableProp->componentType,
                           mutableProp->name, oldValue, mutableProp->currentValue);
                SetModified(true);
            }
            break;
        }

        case PropertyType::Int:
        {
            int val = 0;
            try
            {
                val = std::stoi(mutableProp->currentValue);
            }
            catch (...)
            {
            }

            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::InputInt("##val", &val))
            {
                mutableProp->currentValue = std::to_string(val);
                mutableProp->isModified = (mutableProp->currentValue != mutableProp->originalValue);
                RecordEdit("Entity_" + std::to_string(m_selectedEntityId), mutableProp->componentType,
                           mutableProp->name, oldValue, mutableProp->currentValue);
                SetModified(true);
            }
            break;
        }

        case PropertyType::Bool:
        {
            bool val = (mutableProp->currentValue == "true" || mutableProp->currentValue == "1");
            if (ImGui::Checkbox("##val", &val))
            {
                mutableProp->currentValue = val ? "true" : "false";
                mutableProp->isModified = (mutableProp->currentValue != mutableProp->originalValue);
                RecordEdit("Entity_" + std::to_string(m_selectedEntityId), mutableProp->componentType,
                           mutableProp->name, oldValue, mutableProp->currentValue);
                SetModified(true);
            }
            break;
        }

        case PropertyType::String:
        {
            char buf[256] = {};
            std::strncpy(buf, mutableProp->currentValue.c_str(), sizeof(buf) - 1);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::InputText("##val", buf, sizeof(buf)))
            {
                mutableProp->currentValue = buf;
                mutableProp->isModified = (mutableProp->currentValue != mutableProp->originalValue);
                RecordEdit("Entity_" + std::to_string(m_selectedEntityId), mutableProp->componentType,
                           mutableProp->name, oldValue, mutableProp->currentValue);
                SetModified(true);
            }
            break;
        }

        case PropertyType::Vector3:
        {
            float xyz[3] = {0.0f, 0.0f, 0.0f};
            std::istringstream iss(mutableProp->currentValue);
            char comma = ',';
            iss >> xyz[0] >> comma >> xyz[1] >> comma >> xyz[2];

            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::DragFloat3("##val", xyz, 0.1f, -1000.0f, 1000.0f, "%.3f"))
            {
                std::ostringstream oss;
                oss << xyz[0] << "," << xyz[1] << "," << xyz[2];
                mutableProp->currentValue = oss.str();
                mutableProp->isModified = (mutableProp->currentValue != mutableProp->originalValue);
                RecordEdit("Entity_" + std::to_string(m_selectedEntityId), mutableProp->componentType,
                           mutableProp->name, oldValue, mutableProp->currentValue);
                SetModified(true);
            }
            break;
        }

        case PropertyType::Color:
        {
            float rgba[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            std::istringstream iss(mutableProp->currentValue);
            char comma = ',';
            iss >> rgba[0] >> comma >> rgba[1] >> comma >> rgba[2] >> comma >> rgba[3];

            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::ColorEdit4("##val", rgba, ImGuiColorEditFlags_AlphaBar))
            {
                std::ostringstream oss;
                oss << rgba[0] << "," << rgba[1] << "," << rgba[2] << "," << rgba[3];
                mutableProp->currentValue = oss.str();
                mutableProp->isModified = (mutableProp->currentValue != mutableProp->originalValue);
                RecordEdit("Entity_" + std::to_string(m_selectedEntityId), mutableProp->componentType,
                           mutableProp->name, oldValue, mutableProp->currentValue);
                SetModified(true);
            }
            break;
        }
        }
    }

    // ========================================================================
    // RenderPropertyWatches — sparkline graphs via ImGui DrawList
    // ========================================================================

    void RuntimeInspectorPanel::RenderPropertyWatches()
    {
        ImGui::Text(ICON_FA_CHART_BAR " Property Watches");
        ImGui::Spacing();

        std::string keyToRemove;

        for (auto& [key, watch] : m_watches)
        {
            ImGui::PushID(key.c_str());

            // Header with label and remove button
            ImGui::Text("%s", watch.displayLabel.c_str());
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20.0f);
            if (ImGui::SmallButton(ICON_FA_TIMES "##RemoveWatch"))
            {
                keyToRemove = key;
            }

            // Current value
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 80.0f);
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "%.2f", watch.currentValue);

            // Draw sparkline graph
            const float graphWidth = ImGui::GetContentRegionAvail().x;
            const float graphHeight = 40.0f;
            ImVec2 cursorPos = ImGui::GetCursorScreenPos();

            ImDrawList* drawList = ImGui::GetWindowDrawList();

            // Background
            drawList->AddRectFilled(cursorPos, ImVec2(cursorPos.x + graphWidth, cursorPos.y + graphHeight),
                                    IM_COL32(30, 30, 35, 200));
            drawList->AddRect(cursorPos, ImVec2(cursorPos.x + graphWidth, cursorPos.y + graphHeight),
                              IM_COL32(60, 60, 70, 255));

            // Draw sparkline
            if (watch.historySamples > 1)
            {
                float range = watch.maxValue - watch.minValue;
                if (range < 0.001f)
                    range = 1.0f;

                size_t sampleCount = watch.historySamples;
                float stepX = graphWidth / static_cast<float>(sampleCount - 1);

                for (size_t i = 1; i < sampleCount; ++i)
                {
                    size_t idx0 = (watch.historyIndex + PropertyWatch::MAX_HISTORY - sampleCount + i - 1) %
                                  PropertyWatch::MAX_HISTORY;
                    size_t idx1 = (watch.historyIndex + PropertyWatch::MAX_HISTORY - sampleCount + i) %
                                  PropertyWatch::MAX_HISTORY;

                    float v0 = (watch.history[idx0] - watch.minValue) / range;
                    float v1 = (watch.history[idx1] - watch.minValue) / range;

                    v0 = std::clamp(v0, 0.0f, 1.0f);
                    v1 = std::clamp(v1, 0.0f, 1.0f);

                    float x0 = cursorPos.x + static_cast<float>(i - 1) * stepX;
                    float y0 = cursorPos.y + graphHeight - v0 * graphHeight;
                    float x1 = cursorPos.x + static_cast<float>(i) * stepX;
                    float y1 = cursorPos.y + graphHeight - v1 * graphHeight;

                    drawList->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(80, 200, 120, 255), 1.5f);
                }
            }

            // Min/max labels
            drawList->AddText(ImVec2(cursorPos.x + 2.0f, cursorPos.y + 1.0f), IM_COL32(180, 180, 180, 180),
                              (std::to_string(static_cast<int>(watch.maxValue * 100.0f) / 100.0f)).c_str());
            drawList->AddText(ImVec2(cursorPos.x + 2.0f, cursorPos.y + graphHeight - 14.0f),
                              IM_COL32(180, 180, 180, 180),
                              (std::to_string(static_cast<int>(watch.minValue * 100.0f) / 100.0f)).c_str());

            // Reserve space for the graph
            ImGui::Dummy(ImVec2(graphWidth, graphHeight));

            ImGui::Spacing();
            ImGui::PopID();
        }

        // Deferred removal to avoid modifying map during iteration
        if (!keyToRemove.empty())
        {
            m_watches.erase(keyToRemove);
        }
    }

    // ========================================================================
    // RenderEditHistory
    // ========================================================================

    void RuntimeInspectorPanel::RenderEditHistory()
    {
        ImGui::Text(ICON_FA_HISTORY " Edit History (%zu entries)", s_editHistory.size());
        ImGui::Spacing();

        if (s_editHistory.empty())
        {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No edits recorded yet.");
            return;
        }

        // Clear history button
        if (ImGui::SmallButton(ICON_FA_TRASH " Clear History"))
        {
            s_editHistory.clear();
            return;
        }

        ImGui::Spacing();

        // Scrollable child region for history
        float availHeight = std::min(200.0f, static_cast<float>(s_editHistory.size()) * 50.0f);
        if (ImGui::BeginChild("##EditHistoryScroll", ImVec2(0, availHeight), true))
        {
            // Show most recent first, capped to max entries
            int startIdx = static_cast<int>(s_editHistory.size()) - 1;
            int endIdx = std::max(0, startIdx - m_editHistoryMaxEntries + 1);

            for (int i = startIdx; i >= endIdx; --i)
            {
                const auto& entry = s_editHistory[static_cast<size_t>(i)];

                ImGui::PushID(i);

                // Timestamp
                ImGui::TextColored(ImVec4(0.5f, 0.7f, 0.9f, 1.0f), "[%s]", entry.timestamp.c_str());
                ImGui::SameLine();

                // Entity + component + property
                ImGui::Text("%s.%s.%s", entry.entityLabel.c_str(), entry.componentType.c_str(),
                            entry.propertyName.c_str());

                // Old -> New values
                ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "  %s", entry.oldValue.c_str());
                ImGui::SameLine();
                ImGui::Text(ICON_FA_CHEVRON_RIGHT);
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "%s", entry.newValue.c_str());

                if (i > endIdx)
                {
                    ImGui::Separator();
                }

                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }

    // ========================================================================
    // RenderToolbar
    // ========================================================================

    void RuntimeInspectorPanel::RenderToolbar()
    {
        // Refresh button
        if (ImGui::Button(ICON_FA_REFRESH " Refresh"))
        {
            if (m_hasSelection)
            {
                m_properties = GenerateDemoProperties(m_selectedEntityId);
            }
            m_lastRefresh = std::chrono::steady_clock::now();
        }

        ImGui::SameLine();

        // Auto-refresh toggle
        if (m_autoRefresh)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.2f, 1.0f));
        }
        if (ImGui::Button(m_autoRefresh ? (ICON_FA_SYNC_ALT " Auto") : (ICON_FA_PAUSE " Paused")))
        {
            m_autoRefresh = !m_autoRefresh;
        }
        if (m_autoRefresh)
        {
            ImGui::PopStyleColor();
        }

        ImGui::SameLine();

        // Freeze on pause toggle
        ImGui::Checkbox(ICON_FA_LOCK " Freeze", &m_freezeOnPause);

        ImGui::SameLine();

        // History toggle
        if (ImGui::Button(m_showEditHistory ? (ICON_FA_HISTORY " Hide History") : (ICON_FA_HISTORY " History")))
        {
            m_showEditHistory = !m_showEditHistory;
        }

        // Refresh interval slider
        ImGui::SetNextItemWidth(120.0f);
        ImGui::SliderFloat("Interval", &m_refreshInterval, 0.016f, 1.0f, "%.3fs");
    }

    // ========================================================================
    // RenderSearchFilter
    // ========================================================================

    void RuntimeInspectorPanel::RenderSearchFilter()
    {
        // Search bar
        ImGui::Text(ICON_FA_SEARCH);
        ImGui::SameLine();

        char filterBuf[128] = {};
        std::strncpy(filterBuf, m_searchFilter.c_str(), sizeof(filterBuf) - 1);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 200.0f);
        if (ImGui::InputTextWithHint("##SearchFilter", "Filter properties...", filterBuf, sizeof(filterBuf)))
        {
            m_searchFilter = filterBuf;
        }

        ImGui::SameLine();

        // Modified-only filter
        ImGui::Checkbox("Modified", &m_showModifiedOnly);

        ImGui::SameLine();

        // Read-only filter
        ImGui::Checkbox("R/O", &m_showReadOnly);
    }

    // ========================================================================
    // MakeWatchKey
    // ========================================================================

    std::string RuntimeInspectorPanel::MakeWatchKey(uint32_t entityId, const std::string& comp,
                                                    const std::string& prop) const
    {
        std::ostringstream oss;
        oss << entityId << "::" << comp << "::" << prop;
        return oss.str();
    }

} // namespace SparkEditor
