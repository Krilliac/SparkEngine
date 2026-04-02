/**
 * @file EventResponsePanel.cpp
 * @brief Implementation of the event response rule editor panel
 */

#include "EventResponsePanel.h"
#include "Engine/Gameplay/EventResponseSystem.h"
#include <imgui.h>

#include <algorithm>
#include <cstring>

namespace SparkEditor
{

    // Trigger type names for dropdown
    static constexpr const char* kTriggerNames[] = {
        "On Trigger Enter", "On Trigger Exit",   "On Damaged",     "On Killed",         "On Item Pickup",
        "On Key Press",     "On Key Release",    "On Collision",   "On Quest Complete", "On Timer",
        "On Start",         "On Weather Change", "On Time of Day", "On Entity Created", "On Entity Destroyed",
        "On Custom Event",
    };
    static constexpr int kTriggerCount = static_cast<int>(std::size(kTriggerNames));

    // Action type names for dropdown
    static constexpr const char* kActionNames[] = {
        "Spawn Entity",       "Destroy Entity",  "Enable Entity", "Disable Entity",    "Set Position",
        "Move Toward",        "Teleport Entity", "Rotate Entity", "Set Health",        "Deal Damage",
        "Heal Entity",        "Play Sound",      "Stop Sound",    "Play Animation",    "Apply Force",
        "Apply Impulse",      "Show Dialogue",   "Show Message",  "Set Weather",       "Set Time of Day",
        "Set World Variable", "Set World Flag",  "Delay",         "Fire Custom Event",
    };
    static constexpr int kActionCount = static_cast<int>(std::size(kActionNames));

    // Condition type names
    static constexpr const char* kConditionNames[] = {
        "Is Alive",
        "Is Dead",
        "Health Above",
        "Health Below",
        "Has Component",
        "Has Tag",
        "Has Aura",
        "In Area",
        "Near Entity",
        "In Cell",
        "Has Item",
        "Quest Complete",
        "Quest Active",
        "Level Above",
        "Level Below",
        "Has Ability",
        "Is Class",
        "Is Faction",
        "Time of Day Between",
        "Weather Is",
        "Flag Set",
        "Variable Equals",
        "Variable Greater Than",
        "Variable Less Than",
        "Always True",
        "Always False",
        "Custom",
    };
    static constexpr int kConditionCount = static_cast<int>(std::size(kConditionNames));

    // How many parameter fields each action type needs
    static int GetActionParamCount(int typeIndex)
    {
        switch (typeIndex)
        {
        case 0:
            return 1; // SpawnEntity: name
        case 1:
            return 0; // DestroyEntity: self
        case 2:
            return 0; // EnableEntity
        case 3:
            return 0; // DisableEntity
        case 4:
            return 3; // SetPosition: x,y,z
        case 5:
            return 4; // MoveToward: x,y,z,speed
        case 6:
            return 4; // TeleportEntity: id,x,y,z
        case 7:
            return 3; // RotateEntity: pitch,yaw,roll
        case 8:
            return 1; // SetHealth: value
        case 9:
            return 1; // DealDamage: amount
        case 10:
            return 1; // HealEntity: amount
        case 11:
            return 1; // PlaySound: path
        case 12:
            return 1; // StopSound: path
        case 13:
            return 1; // PlayAnimation: name
        case 14:
            return 3; // ApplyForce: x,y,z
        case 15:
            return 3; // ApplyImpulse: x,y,z
        case 16:
            return 1; // ShowDialogue: id
        case 17:
            return 1; // ShowMessage: text
        case 18:
            return 1; // SetWeather: type
        case 19:
            return 1; // SetTimeOfDay: hour
        case 20:
            return 2; // SetWorldVariable: name, value
        case 21:
            return 2; // SetWorldFlag: name, value
        case 22:
            return 1; // Delay: seconds
        case 23:
            return 1; // FireCustomEvent: name
        default:
            return 0;
        }
    }

    static const char* GetActionParamLabel(int typeIndex, int paramIndex)
    {
        // Common labels
        switch (typeIndex)
        {
        case 0:
            return "Name";
        case 4:
        case 14:
        case 15:
            return paramIndex == 0 ? "X" : (paramIndex == 1 ? "Y" : "Z");
        case 5:
            return paramIndex < 3 ? (paramIndex == 0 ? "X" : (paramIndex == 1 ? "Y" : "Z")) : "Speed";
        case 7:
            return paramIndex == 0 ? "Pitch" : (paramIndex == 1 ? "Yaw" : "Roll");
        case 8:
        case 9:
        case 10:
            return "Amount";
        case 11:
        case 12:
            return "Sound Path";
        case 13:
            return "Animation Name";
        case 16:
            return "Dialogue ID";
        case 17:
            return "Message";
        case 18:
            return "Weather Type";
        case 19:
            return "Hour (0-24)";
        case 20:
        case 21:
            return paramIndex == 0 ? "Name" : "Value";
        case 22:
            return "Seconds";
        case 23:
            return "Event Name";
        default:
            return "Param";
        }
    }

    EventResponsePanel::EventResponsePanel() : EditorPanel("Event Responses", "EventResponses") {}

    bool EventResponsePanel::Initialize()
    {
        SyncFromEngine();
        return true;
    }

    void EventResponsePanel::Update(float /*deltaTime*/)
    {
        auto& ers = Spark::Gameplay::EventResponseSystem::GetInstance();
        m_totalFired = ers.GetTimesFirered();
    }

    void EventResponsePanel::Render()
    {
        ImGui::Columns(2, "EventResponseColumns", true);

        // Left column: rule list
        ImGui::SetColumnWidth(0, 250.0f);
        RenderRuleList();

        ImGui::NextColumn();

        // Right column: rule editor
        if (m_selectedRule >= 0 && m_selectedRule < static_cast<int>(m_rules.size()))
        {
            RenderRuleEditor();
        }
        else
        {
            ImGui::TextDisabled("Select a rule or create a new one");
        }

        ImGui::Columns(1);
    }

    void EventResponsePanel::Shutdown()
    {
        if (m_dirty)
        {
            SyncToEngine();
        }
    }

    // ========================================================================
    // Rule List
    // ========================================================================

    void EventResponsePanel::RenderRuleList()
    {
        ImGui::Text("Rules (%zu)", m_rules.size());
        ImGui::SameLine(ImGui::GetColumnWidth() - 70.0f);
        if (ImGui::SmallButton("+ New"))
        {
            AddNewRule();
        }

        ImGui::Separator();

        // Stats
        ImGui::TextDisabled("Fired: %u", m_totalFired);

        ImGui::Separator();

        // Search
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##RuleSearch", "Search rules...", m_searchFilter, sizeof(m_searchFilter));

        ImGui::Separator();

        // Rule list
        for (int i = 0; i < static_cast<int>(m_rules.size()); ++i)
        {
            auto& rule = m_rules[i];

            // Filter
            if (m_searchFilter[0] != '\0')
            {
                std::string name(rule.name);
                std::string filter(m_searchFilter);
                std::transform(name.begin(), name.end(), name.begin(), ::tolower);
                std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);
                if (name.find(filter) == std::string::npos)
                {
                    continue;
                }
            }

            ImGui::PushID(i);

            // Enable/disable toggle
            if (ImGui::Checkbox("##enabled", &rule.enabled))
            {
                m_dirty = true;
            }
            ImGui::SameLine();

            // Selectable rule name
            bool isSelected = (m_selectedRule == i);
            std::string label = std::string(rule.name) + "##rule";
            if (ImGui::Selectable(label.c_str(), isSelected))
            {
                m_selectedRule = i;
            }

            // Delete button on right-click
            if (ImGui::BeginPopupContextItem("RuleCtx"))
            {
                if (ImGui::MenuItem("Delete"))
                {
                    m_rules.erase(m_rules.begin() + i);
                    if (m_selectedRule >= static_cast<int>(m_rules.size()))
                    {
                        m_selectedRule = static_cast<int>(m_rules.size()) - 1;
                    }
                    m_dirty = true;
                    ImGui::EndPopup();
                    ImGui::PopID();
                    break;
                }
                if (ImGui::MenuItem("Duplicate"))
                {
                    auto copy = m_rules[i];
                    std::strncat(copy.name, " (Copy)", sizeof(copy.name) - std::strlen(copy.name) - 1);
                    m_rules.push_back(std::move(copy));
                    m_dirty = true;
                    ImGui::EndPopup();
                    ImGui::PopID();
                    break;
                }
                ImGui::EndPopup();
            }

            ImGui::PopID();
        }

        // Apply button
        ImGui::Separator();
        if (m_dirty && ImGui::Button("Apply Rules"))
        {
            SyncToEngine();
            m_dirty = false;
        }
    }

    // ========================================================================
    // Rule Editor
    // ========================================================================

    void EventResponsePanel::RenderRuleEditor()
    {
        auto& rule = m_rules[m_selectedRule];

        ImGui::Text("Edit Rule");
        ImGui::Separator();

        // Name
        if (ImGui::InputText("Name", rule.name, sizeof(rule.name)))
        {
            m_dirty = true;
        }

        // Source entity
        int entityId = static_cast<int>(rule.sourceEntityId);
        if (ImGui::InputInt("Source Entity (0=Global)", &entityId))
        {
            rule.sourceEntityId = static_cast<uint32_t>(std::max(0, entityId));
            m_dirty = true;
        }

        // One-shot
        if (ImGui::Checkbox("One Shot (fire once then disable)", &rule.oneShot))
        {
            m_dirty = true;
        }

        ImGui::Separator();
        RenderTriggerSection();
        ImGui::Separator();
        RenderConditionSection();
        ImGui::Separator();
        RenderActionList();
    }

    void EventResponsePanel::RenderTriggerSection()
    {
        auto& rule = m_rules[m_selectedRule];

        ImGui::Text("WHEN:");
        ImGui::Indent();

        if (ImGui::Combo("Trigger", &rule.triggerIndex, kTriggerNames, kTriggerCount))
        {
            m_dirty = true;
        }

        // Trigger parameter (context-dependent)
        bool needsParam = (rule.triggerIndex == 5 || rule.triggerIndex == 6 || // KeyPress/Release: action name
                           rule.triggerIndex == 9 ||                           // Timer: interval
                           rule.triggerIndex == 15);                           // Custom: event name
        if (needsParam)
        {
            const char* hint = "Parameter";
            if (rule.triggerIndex == 5 || rule.triggerIndex == 6)
                hint = "Action name (e.g., Jump, Fire)";
            else if (rule.triggerIndex == 9)
                hint = "Interval in seconds (e.g., 2.0)";
            else if (rule.triggerIndex == 15)
                hint = "Custom event name";

            if (ImGui::InputTextWithHint("Trigger Param", hint, rule.triggerParam, sizeof(rule.triggerParam)))
            {
                m_dirty = true;
            }
        }

        ImGui::Unindent();
    }

    void EventResponsePanel::RenderConditionSection()
    {
        auto& rule = m_rules[m_selectedRule];

        ImGui::Text("IF: (conditions, leave empty to always fire)");
        ImGui::Indent();

        for (int g = 0; g < static_cast<int>(rule.conditionGroups.size()); ++g)
        {
            auto& group = rule.conditionGroups[g];
            ImGui::PushID(g);

            // Group header
            const char* logicItems[] = {"AND", "OR"};
            ImGui::SetNextItemWidth(60.0f);
            if (ImGui::Combo("##logic", &group.logic, logicItems, 2))
            {
                m_dirty = true;
            }
            ImGui::SameLine();
            ImGui::Text("Group %d", g + 1);
            ImGui::SameLine();
            if (ImGui::SmallButton("X##grp"))
            {
                rule.conditionGroups.erase(rule.conditionGroups.begin() + g);
                m_dirty = true;
                ImGui::PopID();
                break;
            }

            ImGui::Indent();
            for (int c = 0; c < static_cast<int>(group.conditions.size()); ++c)
            {
                auto& cond = group.conditions[c];
                ImGui::PushID(c);

                if (ImGui::Checkbox("NOT##cond", &cond.negated))
                {
                    m_dirty = true;
                }
                ImGui::SameLine();

                ImGui::SetNextItemWidth(160.0f);
                if (ImGui::Combo("##condType", &cond.typeIndex, kConditionNames, kConditionCount))
                {
                    m_dirty = true;
                }

                ImGui::SameLine();
                ImGui::SetNextItemWidth(80.0f);
                if (ImGui::InputText("##p1", cond.param1, sizeof(cond.param1)))
                {
                    m_dirty = true;
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80.0f);
                if (ImGui::InputText("##p2", cond.param2, sizeof(cond.param2)))
                {
                    m_dirty = true;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("X##cond"))
                {
                    group.conditions.erase(group.conditions.begin() + c);
                    m_dirty = true;
                    ImGui::PopID();
                    break;
                }

                ImGui::PopID();
            }

            if (ImGui::SmallButton("+ Condition"))
            {
                group.conditions.push_back({});
                m_dirty = true;
            }

            ImGui::Unindent();
            ImGui::PopID();
        }

        if (ImGui::SmallButton("+ Condition Group"))
        {
            rule.conditionGroups.push_back({});
            m_dirty = true;
        }

        ImGui::Unindent();
    }

    void EventResponsePanel::RenderActionList()
    {
        auto& rule = m_rules[m_selectedRule];

        ImGui::Text("THEN: (actions, executed in order)");
        ImGui::Indent();

        for (int a = 0; a < static_cast<int>(rule.actions.size()); ++a)
        {
            ImGui::PushID(a);
            RenderActionEditor(a);

            // Move up/down and delete
            ImGui::SameLine();
            if (a > 0 && ImGui::SmallButton("^"))
            {
                std::swap(rule.actions[a], rule.actions[a - 1]);
                m_dirty = true;
            }
            ImGui::SameLine();
            if (a + 1 < static_cast<int>(rule.actions.size()) && ImGui::SmallButton("v"))
            {
                std::swap(rule.actions[a], rule.actions[a + 1]);
                m_dirty = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("X##act"))
            {
                rule.actions.erase(rule.actions.begin() + a);
                m_dirty = true;
                ImGui::PopID();
                break;
            }

            ImGui::PopID();
        }

        if (ImGui::SmallButton("+ Add Action"))
        {
            rule.actions.push_back({});
            m_dirty = true;
        }

        ImGui::Unindent();
    }

    void EventResponsePanel::RenderActionEditor(int actionIndex)
    {
        auto& rule = m_rules[m_selectedRule];
        auto& action = rule.actions[actionIndex];

        // Action type selector
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo("##actType", &action.typeIndex, kActionNames, kActionCount))
        {
            action.paramCount = GetActionParamCount(action.typeIndex);
            m_dirty = true;
        }

        // Parameter fields
        int pCount = GetActionParamCount(action.typeIndex);
        for (int p = 0; p < pCount && p < 4; ++p)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f);
            const char* label = GetActionParamLabel(action.typeIndex, p);
            char fieldId[32];
            std::snprintf(fieldId, sizeof(fieldId), "##p%d", p);
            if (ImGui::InputTextWithHint(fieldId, label, action.params[p], sizeof(action.params[p])))
            {
                m_dirty = true;
            }
        }
    }

    // ========================================================================
    // Sync with Engine
    // ========================================================================

    void EventResponsePanel::SyncFromEngine()
    {
        auto& ers = Spark::Gameplay::EventResponseSystem::GetInstance();
        const auto& engineRules = ers.GetRules();

        m_rules.clear();
        m_rules.reserve(engineRules.size());

        for (const auto& er : engineRules)
        {
            RuleEntry re{};
            std::strncpy(re.name, er.name.c_str(), sizeof(re.name) - 1);
            re.sourceEntityId = er.sourceEntityId;
            re.triggerIndex = static_cast<int>(er.trigger);
            std::strncpy(re.triggerParam, er.triggerParam.c_str(), sizeof(re.triggerParam) - 1);
            re.enabled = er.enabled;
            re.oneShot = er.oneShot;

            for (const auto& act : er.actions)
            {
                ActionEntry ae{};
                ae.typeIndex = static_cast<int>(act.type);
                ae.paramCount = static_cast<int>(act.params.size());
                for (int p = 0; p < ae.paramCount && p < 4; ++p)
                {
                    std::visit(
                        [&ae, p](auto&& val)
                        {
                            using T = std::decay_t<decltype(val)>;
                            if constexpr (std::is_same_v<T, std::string>)
                                std::strncpy(ae.params[p], val.c_str(), sizeof(ae.params[p]) - 1);
                            else if constexpr (std::is_same_v<T, int64_t>)
                                std::snprintf(ae.params[p], sizeof(ae.params[p]), "%lld", static_cast<long long>(val));
                            else if constexpr (std::is_same_v<T, double>)
                                std::snprintf(ae.params[p], sizeof(ae.params[p]), "%.3f", val);
                            else if constexpr (std::is_same_v<T, uint32_t>)
                                std::snprintf(ae.params[p], sizeof(ae.params[p]), "%u", val);
                        },
                        act.params[p]);
                }
                re.actions.push_back(std::move(ae));
            }

            m_rules.push_back(std::move(re));
        }

        m_dirty = false;
    }

    void EventResponsePanel::SyncToEngine()
    {
        auto& ers = Spark::Gameplay::EventResponseSystem::GetInstance();
        ers.ClearRules();

        for (const auto& re : m_rules)
        {
            Spark::Gameplay::EventResponseRule rule;
            rule.name = re.name;
            rule.sourceEntityId = re.sourceEntityId;
            rule.trigger = static_cast<Spark::Gameplay::EventTriggerType>(re.triggerIndex);
            rule.triggerParam = re.triggerParam;
            rule.enabled = re.enabled;
            rule.oneShot = re.oneShot;

            for (const auto& ae : re.actions)
            {
                Spark::Gameplay::GameplayAction action;
                action.type = static_cast<Spark::Gameplay::ActionType>(ae.typeIndex);
                int pCount = GetActionParamCount(ae.typeIndex);
                for (int p = 0; p < pCount && p < 4; ++p)
                {
                    if (ae.params[p][0] != '\0')
                    {
                        // Try to parse as number, fall back to string
                        char* end = nullptr;
                        double dval = std::strtod(ae.params[p], &end);
                        if (end != ae.params[p] && *end == '\0')
                        {
                            action.params.push_back(dval);
                        }
                        else
                        {
                            action.params.push_back(std::string(ae.params[p]));
                        }
                    }
                    else
                    {
                        action.params.push_back(std::monostate{});
                    }
                }
                rule.actions.push_back(std::move(action));
            }

            ers.AddRule(std::move(rule));
        }

        m_dirty = false;
    }

    void EventResponsePanel::AddNewRule()
    {
        RuleEntry re{};
        std::snprintf(re.name, sizeof(re.name), "New Rule %zu", m_rules.size() + 1);
        re.triggerIndex = static_cast<int>(Spark::Gameplay::EventTriggerType::OnStart);
        m_rules.push_back(std::move(re));
        m_selectedRule = static_cast<int>(m_rules.size()) - 1;
        m_dirty = true;
    }

} // namespace SparkEditor
