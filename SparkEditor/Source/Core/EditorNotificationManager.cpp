/**
 * @file EditorNotificationManager.cpp
 * @brief Implementation of the SparkEditor toast notification queue.
 */

#include "EditorNotificationManager.h"

#include "EditorIcons.h"

#include <imgui.h>

#include <algorithm>

namespace SparkEditor
{

    void EditorNotificationManager::Show(const std::string& message, const std::string& type, float duration)
    {
        Notification notification;
        notification.message = message;
        notification.type = type;
        notification.duration = duration;
        notification.timeLeft = duration;
        notification.timestamp = std::chrono::steady_clock::now();

        m_notifications.push_back(notification);
    }

    void EditorNotificationManager::Update(float deltaTime)
    {
        auto it = m_notifications.begin();
        while (it != m_notifications.end())
        {
            it->timeLeft -= deltaTime;
            if (it->timeLeft <= 0.0f && it->duration > 0.0f)
            {
                it = m_notifications.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    // NOTE: Intentionally exceeds 50-line guideline — linear UI layout code
    void EditorNotificationManager::Render()
    {
        const float NOTIFICATION_WIDTH = 340.0f;
        const float NOTIFICATION_HEIGHT = 56.0f;
        const float NOTIFICATION_SPACING = 8.0f;

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        float yOffset = viewport->WorkPos.y + 12.0f;

        for (size_t i = 0; i < m_notifications.size(); ++i)
        {
            const auto& notification = m_notifications[i];

            // Fade out in last 0.5 seconds
            float alpha = 1.0f;
            if (notification.duration > 0.0f && notification.timeLeft < 0.5f)
            {
                alpha = std::max(0.0f, notification.timeLeft / 0.5f);
            }

            ImVec2 notificationPos(viewport->WorkPos.x + viewport->WorkSize.x - NOTIFICATION_WIDTH - 16.0f,
                                   yOffset + i * (NOTIFICATION_HEIGHT + NOTIFICATION_SPACING));

            ImGui::SetNextWindowPos(notificationPos);
            ImGui::SetNextWindowSize(ImVec2(NOTIFICATION_WIDTH, NOTIFICATION_HEIGHT));
            ImGui::SetNextWindowBgAlpha(0.95f * alpha);

            std::string windowName = "##Notification" + std::to_string(i);
            ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoDocking |
                                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                     ImGuiWindowFlags_NoSavedSettings;

            // Theme-matched accent colors
            ImVec4 accentColor(0.102f, 0.686f, 0.737f, alpha); // teal (info)
            const char* icon = ICON_FA_INFO_CIRCLE;
            if (notification.type == "error")
            {
                accentColor = ImVec4(0.910f, 0.251f, 0.251f, alpha);
                icon = ICON_FA_TIMES;
            }
            else if (notification.type == "warning")
            {
                accentColor = ImVec4(0.941f, 0.659f, 0.188f, alpha);
                icon = ICON_FA_EXCLAMATION;
            }
            else if (notification.type == "success")
            {
                accentColor = ImVec4(0.239f, 0.839f, 0.549f, alpha);
                icon = ICON_FA_CHECK;
            }

            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.118f, 0.129f, 0.161f, 0.95f * alpha));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(accentColor.x, accentColor.y, accentColor.z, 0.25f * alpha));
            if (ImGui::Begin(windowName.c_str(), nullptr, flags))
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 wp = ImGui::GetWindowPos();
                ImVec2 ws = ImGui::GetWindowSize();

                // Left accent stripe (3px, rounded left corners)
                dl->AddRectFilled(wp, ImVec2(wp.x + 3, wp.y + ws.y), ImGui::ColorConvertFloat4ToU32(accentColor), 8.0f,
                                  ImDrawFlags_RoundCornersLeft);

                // Subtle background gradient overlay (darker at bottom)
                dl->AddRectFilledMultiColor(wp, ImVec2(wp.x + ws.x, wp.y + ws.y),
                                            ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0)),
                                            ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0)),
                                            ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0.1f * alpha)),
                                            ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0.1f * alpha)));

                // Content with padding past the stripe
                ImGui::SetCursorPos(ImVec2(14, (NOTIFICATION_HEIGHT - ImGui::GetTextLineHeight()) * 0.5f));
                ImGui::TextColored(accentColor, "%s", icon);
                ImGui::SameLine(0, 8);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.847f, 0.863f, 0.902f, alpha));
                ImGui::TextWrapped("%s", notification.message.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::End();
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(2);
        }
    }

} // namespace SparkEditor
