/**
 * @file EditorNotificationManager.h
 * @brief Toast notifications and notification history for the SparkEditor UI.
 *
 * Toasts stack upward from the bottom-right corner of the main viewport's work area, which sits above the status
 * bar. At most kMaxVisibleToasts are on screen; older ones leave the stack early but stay in the bell history, which
 * keeps the last kMaxHistory notifications with an unread count for the status-bar bell.
 *
 * Lifetime by severity: Info and Success last 4 s, Warning 8 s, Error stays until dismissed. Hovering or focusing a
 * toast pauses its timer. Toasts fade out only when Spark::Ui::ReduceMotion() is off.
 *
 * Contract: game thread only (Show/Notify/Update/Render all touch ImGui-adjacent state). EditorUI owns the single
 * instance through a unique_ptr. Storage is two small vectors bounded by kMaxVisibleToasts and kMaxHistory; Render
 * allocates only the per-toast window-name strings.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace SparkEditor
{

    /**
     * @brief How serious a notification is; drives its glyph, colour and lifetime.
     */
    enum class NotificationSeverity
    {
        Info,
        Success,
        Warning,
        Error
    };

    /**
     * @brief Toast queue plus bell history, owned by EditorUI.
     */
    class EditorNotificationManager
    {
      public:
        /// Toasts shown at once; a newer toast pushes the oldest one into history only.
        static constexpr std::size_t kMaxVisibleToasts = 4;
        /// Notifications the bell history keeps (oldest dropped first).
        static constexpr std::size_t kMaxHistory = 50;

        /**
         * @brief Default on-screen seconds for a severity; 0 means "until dismissed".
         */
        static float DefaultDuration(NotificationSeverity severity) noexcept;

        /**
         * @brief Map the legacy type string: "error", "warning", "success", anything else is Info.
         */
        static NotificationSeverity SeverityFromType(std::string_view type) noexcept;

        /**
         * @brief Post a notification.
         * @param severity  Drives glyph, status word, colour and default lifetime.
         * @param title     Main line, wrapped.
         * @param body      Optional secondary text under the title.
         * @param seconds   On-screen seconds; <= 0 uses DefaultDuration(). Errors always persist.
         */
        void Notify(NotificationSeverity severity, std::string title, std::string body = {}, float seconds = 0.0f);

        /**
         * @brief String API kept for existing callers; maps @p type through SeverityFromType().
         * @param message   Text shown as the toast title.
         * @param type      "info", "success", "warning" or "error".
         * @param duration  Seconds on screen; <= 0 uses the severity default. Errors always persist.
         */
        void Show(const std::string& message, const std::string& type = "info", float duration = 0.0f);

        /**
         * @brief Advance timers of toasts that are not hovered or focused and drop expired ones.
         */
        void Update(float deltaTime);

        /**
         * @brief Render the visible toasts in the bottom-right of the main viewport's work area.
         */
        void Render();

        /**
         * @brief Render the history list; call inside the status-bar bell popup. Marks everything read.
         */
        void RenderHistory();

        /**
         * @brief Toasts currently on screen.
         */
        std::size_t GetCount() const { return m_toasts.size(); }

        /**
         * @brief Notifications posted since the history was last opened.
         */
        std::size_t GetUnreadCount() const { return m_unread; }

        /**
         * @brief Notifications kept in the bell history.
         */
        std::size_t GetHistoryCount() const { return m_history.size(); }

        /**
         * @brief Reset the unread count without opening the history.
         */
        void MarkAllRead() { m_unread = 0; }

        /**
         * @brief Remove every visible toast; history is kept.
         */
        void DismissAll() { m_toasts.clear(); }

      private:
        struct Entry
        {
            NotificationSeverity severity = NotificationSeverity::Info;
            std::string title;
            std::string body;
            std::string clock; ///< Local wall-clock time it was posted, "HH:MM:SS"
        };

        struct Toast
        {
            Entry entry;
            std::uint64_t id = 0;
            float duration = 0.0f; ///< 0 = until dismissed
            float timeLeft = 0.0f;
            bool paused = false; ///< Hovered or focused during the last Render()
        };

        std::vector<Toast> m_toasts;  ///< Oldest first
        std::vector<Entry> m_history; ///< Oldest first, bounded by kMaxHistory
        std::size_t m_unread = 0;
        std::uint64_t m_nextId = 1;
    };

} // namespace SparkEditor
