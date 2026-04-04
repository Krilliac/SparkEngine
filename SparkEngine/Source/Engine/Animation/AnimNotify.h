/**
 * @file AnimNotify.h
 * @brief Animation notify system for firing gameplay events from animation keyframes
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a mechanism to trigger gameplay events at specific times during animation
 * playback. Supports built-in notify types (Sound, Particle, TrailStart/Stop, HitWindow)
 * and custom user-defined notifies.
 *
 * ## Usage
 * @code
 *   using namespace Spark::Animation;
 *
 *   auto& notifyMgr = AnimNotifyManager::GetInstance();
 *   notifyMgr.Initialize();
 *
 *   // Add a footstep sound at 0.3s into the walk animation
 *   AnimNotifyEvent footstep;
 *   footstep.triggerTime = 0.3f;
 *   footstep.type = AnimNotifyType::Sound;
 *   footstep.name = "Footstep_Left";
 *   footstep.payload = "footstep_concrete";
 *   notifyMgr.AddNotify("Walk", footstep);
 *
 *   // Register a handler
 *   notifyMgr.OnNotifyTriggered([](const AnimNotifyTrigger& trigger) {
 *       if (trigger.type == AnimNotifyType::Sound)
 *           PlaySound(trigger.payload, trigger.entityId);
 *   });
 *
 *   // Evaluate during animation playback
 *   notifyMgr.Evaluate("Walk", entityId, previousTime, currentTime);
 * @endcode
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Animation
{

    /**
     * @brief Built-in animation notify types
     */
    enum class AnimNotifyType : uint8_t
    {
        Custom,         ///< User-defined notify with arbitrary payload
        Sound,          ///< Trigger a sound effect
        Particle,       ///< Spawn a particle effect
        TrailStart,     ///< Begin rendering a weapon/motion trail
        TrailStop,      ///< Stop rendering a trail
        HitWindowOpen,  ///< Open a melee hit detection window
        HitWindowClose, ///< Close a melee hit detection window
        CameraShake,    ///< Trigger a camera shake effect
        FootPlant,      ///< Foot contacts ground (for IK/dust)
    };

    /**
     * @brief An event placed on an animation clip's timeline
     *
     * Notifies are authored per-clip and evaluated during playback. When the
     * animation's local time crosses a notify's triggerTime, the notify fires.
     */
    struct AnimNotifyEvent
    {
        /** @brief Time in seconds within the clip when this notify fires */
        float triggerTime = 0.0f;

        /** @brief Type of notify (determines handler routing) */
        AnimNotifyType type = AnimNotifyType::Custom;

        /** @brief Human-readable name for editor display and debugging */
        std::string name;

        /** @brief Type-specific payload (sound name, particle effect name, etc.) */
        std::string payload;

        /** @brief Optional bone name for position-relative effects */
        std::string boneName;

        /** @brief Duration for window-type notifies (HitWindow, Trail). 0 = instant. */
        float duration = 0.0f;

        /** @brief Intensity/magnitude for parameterized notifies (camera shake) */
        float magnitude = 1.0f;
    };

    /**
     * @brief Data passed to notify handlers when a notify fires
     */
    struct AnimNotifyTrigger
    {
        /** @brief Which entity triggered this notify */
        uint32_t entityId = 0;

        /** @brief The notify event data */
        AnimNotifyType type = AnimNotifyType::Custom;
        std::string name;
        std::string payload;
        std::string boneName;
        float magnitude = 1.0f;
        float duration = 0.0f;

        /** @brief Name of the animation clip that triggered this */
        std::string clipName;
    };

    /**
     * @brief Track of notifies associated with an animation clip
     */
    struct AnimNotifyTrack
    {
        /** @brief Sorted list of notify events on this track */
        std::vector<AnimNotifyEvent> events;

        /**
         * @brief Sort events by trigger time (call after adding events)
         */
        void SortByTime()
        {
            std::sort(events.begin(), events.end(),
                      [](const AnimNotifyEvent& a, const AnimNotifyEvent& b) { return a.triggerTime < b.triggerTime; });
        }
    };

    /**
     * @brief Manages animation notifies across all clips
     *
     * Singleton that stores notify tracks per animation clip and evaluates them
     * during playback. Handlers are registered via callbacks.
     */
    class AnimNotifyManager
    {
      public:
        using NotifyCallback = std::function<void(const AnimNotifyTrigger&)>;

        /**
         * @brief Get the singleton instance
         * @return Reference to the AnimNotifyManager
         */
        static AnimNotifyManager& GetInstance()
        {
            static AnimNotifyManager instance;
            return instance;
        }

        /**
         * @brief Initialize the notify manager
         */
        void Initialize() { m_initialized = true; }

        /**
         * @brief Shut down and clear all notify tracks
         */
        void Shutdown()
        {
            m_tracks.clear();
            m_handlers.clear();
            m_initialized = false;
        }

        /**
         * @brief Add a notify event to a clip's track
         * @param clipName Name of the animation clip
         * @param event The notify event to add
         */
        void AddNotify(const std::string& clipName, const AnimNotifyEvent& event)
        {
            m_tracks[clipName].events.push_back(event);
            m_tracks[clipName].SortByTime();
        }

        /**
         * @brief Remove all notifies from a clip
         * @param clipName Name of the animation clip
         */
        void ClearNotifies(const std::string& clipName) { m_tracks.erase(clipName); }

        /**
         * @brief Get the notify track for a clip
         * @param clipName Name of the animation clip
         * @return Pointer to the track, or nullptr if none exists
         */
        const AnimNotifyTrack* GetTrack(const std::string& clipName) const
        {
            auto it = m_tracks.find(clipName);
            return it != m_tracks.end() ? &it->second : nullptr;
        }

        /**
         * @brief Get the number of notify tracks
         * @return Number of clips with notify tracks
         */
        size_t GetTrackCount() const { return m_tracks.size(); }

        /**
         * @brief Register a handler for notify events
         * @param handler Callback invoked when any notify fires
         */
        void OnNotifyTriggered(NotifyCallback handler) { m_handlers.push_back(std::move(handler)); }

        /**
         * @brief Evaluate notifies for a clip between two time points
         *
         * Finds all notifies with triggerTime in (previousTime, currentTime] and
         * fires them. Handles looping clips where currentTime < previousTime.
         *
         * @param clipName Name of the animation clip
         * @param entityId Entity that owns the animation
         * @param previousTime Previous playback time in seconds
         * @param currentTime Current playback time in seconds
         * @param clipDuration Total clip duration (needed for loop detection)
         * @return Number of notifies fired
         */
        int Evaluate(const std::string& clipName, uint32_t entityId, float previousTime, float currentTime,
                     float clipDuration = 0.0f)
        {
            auto it = m_tracks.find(clipName);
            if (it == m_tracks.end())
                return 0;

            const auto& track = it->second;
            int fired = 0;

            bool looped = (currentTime < previousTime) && (clipDuration > 0.0f);

            for (const auto& event : track.events)
            {
                bool shouldFire = false;

                if (looped)
                {
                    // Wrapped around: fire if in (previousTime, clipDuration] or [0, currentTime]
                    shouldFire = (event.triggerTime > previousTime && event.triggerTime <= clipDuration) ||
                                 (event.triggerTime >= 0.0f && event.triggerTime <= currentTime);
                }
                else
                {
                    shouldFire = (event.triggerTime > previousTime && event.triggerTime <= currentTime);
                }

                if (shouldFire)
                {
                    AnimNotifyTrigger trigger;
                    trigger.entityId = entityId;
                    trigger.type = event.type;
                    trigger.name = event.name;
                    trigger.payload = event.payload;
                    trigger.boneName = event.boneName;
                    trigger.magnitude = event.magnitude;
                    trigger.duration = event.duration;
                    trigger.clipName = clipName;

                    for (const auto& handler : m_handlers)
                    {
                        handler(trigger);
                    }
                    ++fired;
                }
            }

            return fired;
        }

        /**
         * @brief Get status string for console/debug display
         * @return Formatted status string
         */
        std::string Console_GetStatus() const
        {
            std::string status = "AnimNotifyManager: ";
            size_t totalNotifies = 0;
            for (const auto& [name, track] : m_tracks)
                totalNotifies += track.events.size();
            status += std::to_string(m_tracks.size()) + " tracks, ";
            status += std::to_string(totalNotifies) + " notifies, ";
            status += std::to_string(m_handlers.size()) + " handlers";
            return status;
        }

      private:
        AnimNotifyManager() = default;

        bool m_initialized = false;
        std::unordered_map<std::string, AnimNotifyTrack> m_tracks;
        std::vector<NotifyCallback> m_handlers;
    };

} // namespace Spark::Animation
