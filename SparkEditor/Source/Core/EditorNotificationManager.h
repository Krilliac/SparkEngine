/**
 * @file EditorNotificationManager.h
 * @brief Toast notifications and notification history for the SparkEditor UI.
 *
 * Toasts stack upward from the bottom-right corner of the main viewport's work area, which sits above the status
 * bar. A toast is Em(26) wide (at least 280 px) but never wider than kMaxToastViewportFraction of the work area, so
 * at a large UI scale its text wraps instead of covering the panels. At most kMaxVisibleToasts are on screen; older
 * ones leave the stack early but stay in the bell history, which keeps the last kMaxHistory notifications with an
 * unread count for the status-bar bell.
 *
 * Lifetime by severity: Info and Success last 4 s, Warning 8 s, Error stays until dismissed. Hovering or focusing a
 * toast pauses its timer. Toasts fade out only when Spark::Ui::ReduceMotion() is off. Each toast is drawn in a
 * window named after its id ("##Notification<id>"), so its hover and focus state stays with it when an older toast
 * leaves the stack.
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
    enum class NotificationSeverity : std::uint8_t
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

        /// Largest share of the work-area width one toast may take.
        static constexpr float kMaxToastViewportFraction = 0.28f;

        /**
         * @brief Width of a toast for a work area @p workWidth px wide at the current UI scale.
         * @details min(max(Em(26), 280 px), kMaxToastViewportFraction x @p workWidth), but never narrower than
         *          min(Em(16), @p workWidth - 32 px) so a small window still gets a readable toast. Needs a current
         *          ImGui context (Em uses its font size).
         */
        static float ToastWidth(float workWidth);

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
         * @brief Render the visible toasts in the bottom-right of a screen-space area.
         *
         * The editor passes the central dock node, so toasts cover the scene or Start page instead of editable
         * Inspector or Hierarchy fields. Width still follows ToastWidth() of the viewport, capped to the area.
         * @param areaMinX Left edge of the area in screen coordinates.
         * @param areaMinY Top edge of the area in screen coordinates.
         * @param areaWidth Width of the area; a non-positive width or height falls back to the viewport work area.
         * @param areaHeight Height of the area.
         */
        void Render(float areaMinX, float areaMinY, float areaWidth, float areaHeight);

        /**
         * @brief Render the history list; call inside a popup (the status-bar bell, or View > Notifications and
         *        the command palette when the status bar is hidden). Marks everything read.
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
