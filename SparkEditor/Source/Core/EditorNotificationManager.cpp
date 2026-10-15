/**
 * @file EditorNotificationManager.cpp
 * @brief Implementation of the SparkEditor toast stack and bell history.
 */

#include "EditorNotificationManager.h"

#include <SparkUI/Fonts.h>
#include <SparkUI/Glyphs.h>
#include <SparkUI/Runtime.h>
#include <SparkUI/Widgets.h>

#include <imgui.h>

#include <algorithm>
#include <array>
#include <ctime>
#include <iterator>
#include <string>
#include <utility>

namespace SparkEditor
{

    namespace
    {
        namespace Ui = Spark::Ui;

        /// Seconds over which an expiring toast fades out when motion is allowed.
        constexpr float kFadeSeconds = 0.5f;

        Ui::Status ToStatus(NotificationSeverity severity) noexcept
        {
            switch (severity)
            {
            case NotificationSeverity::Success:
                return Ui::Status::Success;
            case NotificationSeverity::Warning:
                return Ui::Status::Warning;
            case NotificationSeverity::Error:
                return Ui::Status::Error;
            case NotificationSeverity::Info:
                break;
            }
            return Ui::Status::Info;
        }

        std::string LocalClockNow()
        {
            const std::time_t now = std::time(nullptr);
            std::tm local{};
#if defined(_WIN32)
            localtime_s(&local, &now);
#else
            localtime_r(&now, &local);
#endif
            char text[16] = {};
            std::strftime(text, sizeof(text), "%H:%M:%S", &local);
            return text;
        }

        /// Measured layout of one toast, shared by the measuring and drawing passes.
        struct ToastLayout
        {
            float height = 0.0f;
            float titleHeight = 0.0f;
        };
    } // namespace

    float EditorNotificationManager::ToastWidth(float workWidth)
    {
        // Preferred width is Em(26) (at least 280 px), capped at a share of the work area so a large UI scale
        // wraps the text instead of covering the panels; a narrow window still gets a readable Em(16) toast.
        const float preferred = std::max(Ui::Em(26.0f), Ui::Px(280.0f));
        const float viewportCap = std::max(0.0f, workWidth) * kMaxToastViewportFraction;
        const float readable = std::min(Ui::Em(16.0f), workWidth - Ui::Px(32.0f));
        return std::max(1.0f, std::max(std::min(preferred, viewportCap), readable));
    }

    float EditorNotificationManager::DefaultDuration(NotificationSeverity severity) noexcept
    {
        switch (severity)
        {
        case NotificationSeverity::Warning:
            return 8.0f;
        case NotificationSeverity::Error:
            return 0.0f;
        case NotificationSeverity::Info:
        case NotificationSeverity::Success:
            break;
        }
        return 4.0f;
    }

    NotificationSeverity EditorNotificationManager::SeverityFromType(std::string_view type) noexcept
    {
        if (type == "error")
            return NotificationSeverity::Error;
        if (type == "warning")
            return NotificationSeverity::Warning;
        if (type == "success")
            return NotificationSeverity::Success;
        return NotificationSeverity::Info;
    }

    void EditorNotificationManager::Notify(NotificationSeverity severity, std::string title, std::string body,
                                           float seconds)
    {
        Entry entry;
        entry.severity = severity;
        entry.title = std::move(title);
        entry.body = std::move(body);
        entry.clock = LocalClockNow();

        m_history.push_back(entry);
        if (m_history.size() > kMaxHistory)
            m_history.erase(m_history.begin(), m_history.begin() + (m_history.size() - kMaxHistory));
        ++m_unread;

        Toast toast;
        toast.entry = std::move(entry);
        toast.id = m_nextId++;
        // Errors wait for the user: a failure that vanishes on its own is easy to miss.
        toast.duration =
            severity == NotificationSeverity::Error ? 0.0f : (seconds > 0.0f ? seconds : DefaultDuration(severity));
        toast.timeLeft = toast.duration;
        m_toasts.push_back(std::move(toast));

        // Beyond the visible limit the oldest toast leaves the screen; it is still in the history.
        if (m_toasts.size() > kMaxVisibleToasts)
            m_toasts.erase(m_toasts.begin(), m_toasts.begin() + (m_toasts.size() - kMaxVisibleToasts));
    }

    void EditorNotificationManager::Show(const std::string& message, const std::string& type, float duration)
    {
        Notify(SeverityFromType(type), message, {}, duration);
    }

    void EditorNotificationManager::Update(float deltaTime)
    {
        for (Toast& toast : m_toasts)
        {
            if (toast.duration > 0.0f && !toast.paused)
                toast.timeLeft -= deltaTime;
        }
        std::erase_if(m_toasts, [](const Toast& toast) { return toast.duration > 0.0f && toast.timeLeft <= 0.0f; });
    }

    // NOTE: Intentionally exceeds the 60-line guideline -- one linear measure-then-draw pass over the stack.
    void EditorNotificationManager::Render()
    {
        if (m_toasts.empty())
            return;

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const float inset = Ui::Px(16.0f);
        const float spacing = Ui::Px(8.0f);
        const float padX = Ui::Px(12.0f);
        const float padY = Ui::Px(10.0f);
        const float gap = Ui::Px(4.0f);
        const float stripe = Ui::Px(3.0f);
        const float rounding = Ui::Px(6.0f);
        const float closeSide = std::max(Ui::Px(24.0f), ImGui::GetFrameHeight());
        const float width = ToastWidth(viewport->WorkSize.x);
        const float textLeft = stripe + padX;
        const float wrapWidth = std::max(1.0f, width - textLeft - padX);

        // Measure every toast first so the stack can grow upward from the bottom edge.
        std::array<ToastLayout, kMaxVisibleToasts> layouts{};
        for (std::size_t i = 0; i < m_toasts.size(); ++i)
        {
            const Entry& entry = m_toasts[i].entry;
            ToastLayout& layout = layouts[i];
            layout.titleHeight = ImGui::CalcTextSize(entry.title.c_str(), nullptr, false, wrapWidth).y;
            layout.height = padY + closeSide + gap + layout.titleHeight + padY;
            if (!entry.body.empty())
                layout.height += gap + ImGui::CalcTextSize(entry.body.c_str(), nullptr, false, wrapWidth).y;
        }

        // Newest toast sits lowest; older ones stack above it, so index 0 (oldest) is on top.
        std::array<float, kMaxVisibleToasts> tops{};
        float bottom = viewport->WorkPos.y + viewport->WorkSize.y - inset;
        for (std::size_t i = m_toasts.size(); i-- > 0;)
        {
            tops[i] = bottom - layouts[i].height;
            bottom = tops[i] - spacing;
        }
        const float left = viewport->WorkPos.x + viewport->WorkSize.x - width - inset;

        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings;

        std::uint64_t dismissedId = 0;
        for (std::size_t i = 0; i < m_toasts.size(); ++i)
        {
            Toast& toast = m_toasts[i];
            const ToastLayout& layout = layouts[i];
            const Ui::Status status = ToStatus(toast.entry.severity);

            float alpha = 1.0f;
            if (!Ui::ReduceMotion() && toast.duration > 0.0f && toast.timeLeft < kFadeSeconds)
                alpha = std::max(0.0f, toast.timeLeft / kFadeSeconds);

            ImGui::SetNextWindowPos(ImVec2(left, tops[i]));
            ImGui::SetNextWindowSize(ImVec2(width, layout.height));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, rounding);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, Ui::Px(1.0f));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, Ui::ColV(Ui::Role::SurfaceOverlay, alpha));
            ImGui::PushStyleColor(ImGuiCol_Border, Ui::ColV(Ui::Role::BorderSubtle, alpha));

            // Named by id, not stack position: focus and hover (which pause the timer) belong to this toast.
            const std::string windowName = "##Notification" + std::to_string(toast.id);
            if (ImGui::Begin(windowName.c_str(), nullptr, flags))
            {
                ImGui::PushID(static_cast<int>(toast.id));
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                const ImVec2 windowPos = ImGui::GetWindowPos();
                drawList->AddRectFilled(windowPos, ImVec2(windowPos.x + stripe, windowPos.y + layout.height),
                                        Ui::Col(Ui::StatusTextRole(status), alpha), rounding,
                                        ImDrawFlags_RoundCornersLeft);

                // Header: glyph + status word, then the posting time, then the dismiss button.
                ImGui::SetCursorPos(ImVec2(textLeft, padY + (closeSide - ImGui::GetTextLineHeight()) * 0.5f));
                {
                    Ui::ScopedRoleColor statusColor(ImGuiCol_Text, Ui::StatusTextRole(status), alpha);
                    ImGui::TextUnformatted(Ui::StatusGlyph(status));
                    ImGui::SameLine();
                    Ui::ScopedFont strong(Ui::FontRole::Strong);
                    ImGui::TextUnformatted(Ui::StatusWord(status));
                }
                ImGui::SameLine();
                {
                    Ui::ScopedRoleColor clockColor(ImGuiCol_Text, Ui::Role::TextSecondary, alpha);
                    Ui::ScopedFont mono(Ui::FontRole::MonoSmall);
                    ImGui::TextUnformatted(toast.entry.clock.c_str());
                }

                ImGui::SetCursorPos(ImVec2(width - padX * 0.5f - closeSide, padY));
                if (Ui::IconButton("##dismiss", Ui::Glyph::kXmark, "Dismiss"))
                    dismissedId = toast.id;

                ImGui::PushTextWrapPos(width - padX);
                ImGui::SetCursorPos(ImVec2(textLeft, padY + closeSide + gap));
                {
                    Ui::ScopedRoleColor titleColor(ImGuiCol_Text, Ui::Role::TextPrimary, alpha);
                    ImGui::TextUnformatted(toast.entry.title.c_str());
                }
                if (!toast.entry.body.empty())
                {
                    ImGui::SetCursorPos(ImVec2(textLeft, padY + closeSide + gap + layout.titleHeight + gap));
                    Ui::ScopedRoleColor bodyColor(ImGuiCol_Text, Ui::Role::TextSecondary, alpha);
                    ImGui::TextUnformatted(toast.entry.body.c_str());
                }
                ImGui::PopTextWrapPos();

                // Reading or reaching for a toast keeps it on screen.
                toast.paused = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) ||
                               ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
                ImGui::PopID();
            }
            ImGui::End();
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(3);
        }

        if (dismissedId != 0)
            std::erase_if(m_toasts, [dismissedId](const Toast& toast) { return toast.id == dismissedId; });
    }

    void EditorNotificationManager::RenderHistory()
    {
        m_unread = 0;
        if (m_history.empty())
        {
            Ui::SecondaryText("No notifications yet.");
            return;
        }

        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + Ui::Em(28.0f));
        for (auto it = m_history.rbegin(); it != m_history.rend(); ++it)
        {
            const Ui::Status status = ToStatus(it->severity);
            ImGui::PushID(static_cast<int>(std::distance(m_history.rbegin(), it)));
            {
                Ui::ScopedRoleColor statusColor(ImGuiCol_Text, Ui::StatusTextRole(status));
                ImGui::TextUnformatted(Ui::StatusGlyph(status));
            }
            ImGui::SameLine();
            {
                Ui::ScopedFont mono(Ui::FontRole::MonoSmall);
                Ui::ScopedRoleColor clockColor(ImGuiCol_Text, Ui::Role::TextSecondary);
                ImGui::TextUnformatted(it->clock.c_str());
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(it->title.c_str());
            if (!it->body.empty())
                Ui::SecondaryText("%s", it->body.c_str());
            ImGui::PopID();
        }
        ImGui::PopTextWrapPos();

        ImGui::Separator();
        if (Ui::SecondaryButton("Dismiss all toasts"))
            DismissAll();
    }

} // namespace SparkEditor
