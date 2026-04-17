/**
 * @file EditorNotificationManager.h
 * @brief Toast-style notification queue for the SparkEditor UI.
 *
 * Owns a small queue of transient notifications that fade out after a
 * configurable duration. Rendered in the top-right corner of the main
 * viewport. Split from EditorUI so the rendering and state are
 * self-contained and independently testable.
 */

#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace SparkEditor
{

    /**
     * @brief Toast-notification queue owned by EditorUI.
     */
    class EditorNotificationManager
    {
      public:
        /**
         * @brief Push a new notification onto the queue.
         * @param message   Text body displayed to the user.
         * @param type      One of "info", "success", "warning", "error".
         * @param duration  Seconds before auto-dismiss. <= 0 pins the notification.
         */
        void Show(const std::string& message, const std::string& type = "info", float duration = 3.0f);

        /**
         * @brief Advance fade timers and drop expired notifications.
         */
        void Update(float deltaTime);

        /**
         * @brief Render all live notifications as stacked toasts.
         */
        void Render();

        /**
         * @brief Number of notifications currently queued (for diagnostics).
         */
        size_t GetCount() const { return m_notifications.size(); }

      private:
        struct Notification
        {
            std::string message;
            std::string type;
            float duration;
            float timeLeft;
            std::chrono::steady_clock::time_point timestamp;
        };

        std::vector<Notification> m_notifications;
    };

} // namespace SparkEditor
