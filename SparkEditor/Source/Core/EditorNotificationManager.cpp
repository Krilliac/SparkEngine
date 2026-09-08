/**
 * @file EditorNotificationManager.cpp
 * @brief Implementation of the SparkEditor toast notification queue.
 */

#include "EditorNotificationManager.h"

#include "EditorIcons.h"
#include "EditorTheme.h"

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
        const auto& theme = EditorTheme::GetCurrentThemeData();
        const float NOTIFICATION_MAX_WIDTH = 360.0f;
        const float NOTIFICATION_MIN_HEIGHT = 56.0f;
        const float NOTIFICATION_SPACING = 8.0f;
        const float NOTIFICATION_INSET = 16.0f;
        const float NOTIFICATION_PADDING_Y = 12.0f;
        const float NOTIFICATION_TEXT_LEFT = 40.0f;
        const float NOTIFICATION_TEXT_RIGHT = 14.0f;

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        float yOffset = viewport->WorkPos.y + 12.0f;
        const float notificationWidth =
            std::min(NOTIFICATION_MAX_WIDTH, std::max(1.0f, viewport->WorkSize.x - 2.0f * NOTIFICATION_INSET));
        const float textWrapWidth = std::max(1.0f, notificationWidth - NOTIFICATION_TEXT_LEFT - NOTIFICATION_TEXT_RIGHT);

        for (size_t i = 0; i < m_notifications.size(); ++i)
        {
            const auto& notification = m_notifications[i];

            // Fade out in last 0.5 seconds
            float alpha = 1.0f;
            if (notification.duration > 0.0f && notification.timeLeft < 0.5f)
            {
                alpha = std::max(0.0f, notification.timeLeft / 0.5f);
            }

            const ImVec2 textSize = ImGui::CalcTextSize(notification.message.c_str(), nullptr, false, textWrapWidth);
            const float notificationHeight =
                std::max(NOTIFICATION_MIN_HEIGHT, textSize.y + 2.0f * NOTIFICATION_PADDING_Y);
            ImVec2 notificationPos(viewport->WorkPos.x + viewport->WorkSize.x - notificationWidth - NOTIFICATION_INSET,
                                   yOffset);

            ImGui::SetNextWindowPos(notificationPos);
            ImGui::SetNextWindowSize(ImVec2(notificationWidth, notificationHeight));
            ImGui::SetNextWindowBgAlpha(0.95f * alpha);

            std::string windowName = "##Notification" + std::to_string(i);
            ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoDocking |
                                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                     ImGuiWindowFlags_NoSavedSettings;

            // Theme-matched accent colors
            ImVec4 accentColor = theme.accentSecondary.WithAlpha(alpha).ToImVec4();
            const char* icon = ICON_FA_INFO_CIRCLE;
            if (notification.type == "error")
            {
                accentColor = theme.textError.WithAlpha(alpha).ToImVec4();
                icon = ICON_FA_TIMES;
            }
            else if (notification.type == "warning")
            {
                accentColor = theme.textWarning.WithAlpha(alpha).ToImVec4();
                icon = ICON_FA_EXCLAMATION;
            }
            else if (notification.type == "success")
            {
                accentColor = theme.textSuccess.WithAlpha(alpha).ToImVec4();
                icon = ICON_FA_CHECK;
            }

            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, theme.popupRounding);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, theme.backgroundLight.WithAlpha(0.95f * alpha).ToImVec4());
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(accentColor.x, accentColor.y, accentColor.z, 0.25f * alpha));
            if (ImGui::Begin(windowName.c_str(), nullptr, flags))
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 wp = ImGui::GetWindowPos();
                ImVec2 ws = ImGui::GetWindowSize();

                // Left accent stripe (3px, rounded left corners)
                dl->AddRectFilled(wp, ImVec2(wp.x + 3, wp.y + ws.y), ImGui::ColorConvertFloat4ToU32(accentColor),
                                  theme.popupRounding, ImDrawFlags_RoundCornersLeft);

                // Subtle background gradient overlay (darker at bottom)
                dl->AddRectFilledMultiColor(wp, ImVec2(wp.x + ws.x, wp.y + ws.y),
                                            ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0)),
                                            ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0)),
                                            ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0.1f * alpha)),
                                            ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0.1f * alpha)));

                // Keep the icon vertically centered while the text starts at a
                // stable top padding and can grow to multiple lines.
                const ImVec2 iconPos(wp.x + 14.0f, wp.y + (notificationHeight - ImGui::GetTextLineHeight()) * 0.5f);
                dl->AddText(iconPos, ImGui::ColorConvertFloat4ToU32(accentColor), icon);
                ImGui::SetCursorPos(ImVec2(NOTIFICATION_TEXT_LEFT, NOTIFICATION_PADDING_Y));
                ImGui::PushTextWrapPos(notificationWidth - NOTIFICATION_TEXT_RIGHT);
                ImGui::PushStyleColor(ImGuiCol_Text, theme.text.WithAlpha(alpha).ToImVec4());
                ImGui::TextUnformatted(notification.message.c_str());
                ImGui::PopStyleColor();
                ImGui::PopTextWrapPos();
            }
            ImGui::End();
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(2);
            yOffset += notificationHeight + NOTIFICATION_SPACING;
        }
    }

} // namespace SparkEditor
