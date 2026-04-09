/**
 * @file GameplayTags.h
 * @brief Hierarchical gameplay tag system for data-driven categorization and queries
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a lightweight tag system inspired by UE's FGameplayTag. Tags are
 * dot-separated hierarchical identifiers (e.g. "Damage.Fire.DoT") that enable
 * data-driven filtering, querying, and matching without code changes.
 *
 * ## Usage
 * @code
 *   auto& tags = Spark::Gameplay::GameplayTagRegistry::GetInstance();
 *   tags.Initialize();
 *
 *   // Register tags
 *   tags.RegisterTag("Damage.Fire");
 *   tags.RegisterTag("Damage.Fire.DoT");
 *   tags.RegisterTag("Status.Immune.Fire");
 *
 *   // Create a container on an entity
 *   GameplayTagContainer container;
 *   container.AddTag(tags.GetTagId("Damage.Fire.DoT"));
 *
 *   // Query
 *   container.HasTag(tags.GetTagId("Damage.Fire.DoT")); // true
 *   container.HasTagMatching(tags, "Damage.Fire");       // true (parent match)
 * @endcode
 */

#pragma once

#include "Spark/ServiceInterfaces.h"
#include "Utils/Assert.h"

#include <algorithm>
#include <cstdint>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Spark::Gameplay
{

    /** @brief Unique identifier for a registered gameplay tag */
    using GameplayTagId = uint32_t;

    /** @brief Invalid/unregistered tag ID sentinel */
    constexpr GameplayTagId INVALID_TAG_ID = 0;

    /**
     * @brief A single registered gameplay tag with hierarchy info
     */
    struct GameplayTagInfo
    {
        GameplayTagId id = INVALID_TAG_ID;       ///< Unique numeric ID
        std::string fullName;                    ///< Full dot-separated name (e.g. "Damage.Fire.DoT")
        GameplayTagId parentId = INVALID_TAG_ID; ///< Parent tag ID (INVALID_TAG_ID for root tags)
        std::vector<GameplayTagId> children;     ///< Direct child tag IDs
    };

    /**
     * @brief Container holding a set of gameplay tags for an entity or ability
     *
     * Supports O(1) exact tag checks and hierarchical parent matching via
     * the tag registry.
     */
    class GameplayTagContainer
    {
      public:
        /**
         * @brief Add a tag to this container
         * @param tagId Tag ID to add
         */
        void AddTag(GameplayTagId tagId)
        {
            if (tagId != INVALID_TAG_ID)
                m_tags.insert(tagId);
        }

        /**
         * @brief Remove a tag from this container
         * @param tagId Tag ID to remove
         */
        void RemoveTag(GameplayTagId tagId) { m_tags.erase(tagId); }

        /**
         * @brief Check if this container has an exact tag
         * @param tagId Tag ID to check
         * @return true if the exact tag is present
         */
        bool HasTag(GameplayTagId tagId) const { return m_tags.contains(tagId); }

        /**
         * @brief Check if this container has ANY of the given tags
         * @param other Container of tags to check against
         * @return true if at least one tag matches
         */
        bool HasAny(const GameplayTagContainer& other) const
        {
            for (auto id : other.m_tags)
            {
                if (m_tags.contains(id))
                    return true;
            }
            return false;
        }

        /**
         * @brief Check if this container has ALL of the given tags
         * @param other Container of tags to check against
         * @return true if every tag in other is present
         */
        bool HasAll(const GameplayTagContainer& other) const
        {
            for (auto id : other.m_tags)
            {
                if (!m_tags.contains(id))
                    return false;
            }
            return true;
        }

        /** @brief Remove all tags */
        void Clear() { m_tags.clear(); }

        /** @brief Get the number of tags in this container */
        size_t Count() const { return m_tags.size(); }

        /** @brief Check if container is empty */
        bool IsEmpty() const { return m_tags.empty(); }

        /** @brief Get read-only access to the underlying tag set */
        const std::unordered_set<GameplayTagId>& GetTags() const { return m_tags; }

      private:
        std::unordered_set<GameplayTagId> m_tags;
    };

    /**
     * @brief Central registry for all gameplay tags in the engine
     *
     * Singleton that manages tag registration, hierarchy resolution, and ID lookup.
     * Tags are registered once at startup and referenced by ID at runtime for O(1) lookups.
     */
    class GameplayTagRegistry : public Spark::IGameplayTagService
    {
      public:
        /**
         * @brief Get the singleton instance
         * @return Reference to the GameplayTagRegistry
         */
        static GameplayTagRegistry& GetInstance()
        {
            static GameplayTagRegistry instance;
            return instance;
        }

        /**
         * @brief Reset and initialize the registry state.
         * @note Threading: game-thread-only.
         */
        void Initialize() override
        {
            m_gameThreadId = std::this_thread::get_id();
            std::unique_lock lock(m_registryMutex);
            m_nextId = 1;
            m_tags.clear();
            m_nameToId.clear();
            m_initialized = true;
        }

        /**
         * @brief Clear all registered tags and mark the service uninitialized.
         * @note Threading: game-thread-only.
         */
        void Shutdown() override
        {
            AssertGameThread("Shutdown");
            std::unique_lock lock(m_registryMutex);
            m_tags.clear();
            m_nameToId.clear();
            m_initialized = false;
        }

        /**
         * @brief Register a tag by string view.
         * @note Threading: game-thread-only.
         */
        uint32_t RegisterTag(std::string_view fullName) override { return RegisterTag(std::string(fullName)); }
        uint32_t RegisterTag(const char* fullName)
        {
            if (fullName == nullptr)
                return INVALID_TAG_ID;
            return RegisterTag(std::string_view(fullName));
        }

        /**
         * @brief Register a tag (and all parent tags implicitly).
         * @param fullName Dot-separated tag name (e.g. "Damage.Fire.DoT").
         * @return The tag ID for the registered tag.
         * @note Threading: game-thread-only.
         */
        GameplayTagId RegisterTag(const std::string& fullName)
        {
            AssertGameThread("RegisterTag");
            std::unique_lock lock(m_registryMutex);
            return RegisterTagUnlocked(fullName);
        }

        /**
         * @brief Get the ID for a tag name
         * @param fullName Dot-separated tag name
         * @return Tag ID, or INVALID_TAG_ID if not registered
         */
        uint32_t GetTagId(std::string_view fullName) const override { return GetTagId(std::string(fullName)); }
        uint32_t GetTagId(const char* fullName) const
        {
            if (fullName == nullptr)
                return INVALID_TAG_ID;
            return GetTagId(std::string_view(fullName));
        }

        /**
         * @brief Get the ID for a tag name.
         * @param fullName Dot-separated tag name.
         * @return Tag ID, or INVALID_TAG_ID if not registered.
         * @note Threading: thread-safe.
         */
        GameplayTagId GetTagId(const std::string& fullName) const
        {
            std::shared_lock lock(m_registryMutex);
            return GetTagIdUnlocked(fullName);
        }

        /**
         * @brief Get tag info by ID.
         * @param id Tag ID.
         * @return Pointer to immutable tag info, or nullptr if not found.
         * @note Threading: thread-safe.
         */
        const GameplayTagInfo* GetTagInfo(GameplayTagId id) const
        {
            std::shared_lock lock(m_registryMutex);
            auto it = m_tags.find(id);
            return it != m_tags.end() ? &it->second : nullptr;
        }

        /**
         * @brief Check if @p tagId equals @p parentTagId or is its descendant.
         * @note Threading: thread-safe.
         */
        bool IsTagChildOf(GameplayTagId tagId, GameplayTagId parentTagId) const
        {
            std::shared_lock lock(m_registryMutex);
            return IsTagChildOfUnlocked(tagId, parentTagId);
        }

        /**
         * @brief Check if any tag in @p container matches @p parentTagName or its descendants.
         * @note Threading: thread-safe.
         */
        bool ContainerMatchesTag(const GameplayTagContainer& container, const std::string& parentTagName) const
        {
            std::shared_lock lock(m_registryMutex);
            GameplayTagId parentId = GetTagIdUnlocked(parentTagName);
            if (parentId == INVALID_TAG_ID)
                return false;

            for (auto tagId : container.GetTags())
            {
                if (IsTagChildOfUnlocked(tagId, parentId))
                    return true;
            }
            return false;
        }

        /**
         * @brief Get the total number of registered tags.
         * @note Threading: thread-safe.
         */
        size_t GetTagCount() const
        {
            std::shared_lock lock(m_registryMutex);
            return m_tags.size();
        }

        /**
         * @brief Get all registered tag names, sorted alphabetically.
         * @note Threading: thread-safe.
         */
        std::vector<std::string> GetAllTagNames() const
        {
            std::shared_lock lock(m_registryMutex);
            std::vector<std::string> names;
            names.reserve(m_nameToId.size());
            for (const auto& [name, id] : m_nameToId)
                names.push_back(name);
            std::sort(names.begin(), names.end());
            return names;
        }

        /**
         * @brief Get debug/console status text.
         * @note Threading: thread-safe.
         */
        std::string Console_GetStatus() const
        {
            std::shared_lock lock(m_registryMutex);
            return "GameplayTagRegistry: " + std::to_string(m_tags.size()) + " tags registered";
        }

      private:
        GameplayTagRegistry() = default;

        void AssertGameThread(const char* methodName) const
        {
            ASSERT_MSG(std::this_thread::get_id() == m_gameThreadId, "GameplayTagRegistry::%s must run on game thread",
                       methodName);
        }

        GameplayTagId RegisterTagUnlocked(const std::string& fullName)
        {
            if (fullName.empty())
                return INVALID_TAG_ID;

            auto existing = m_nameToId.find(fullName);
            if (existing != m_nameToId.end())
                return existing->second;

            GameplayTagId parentId = INVALID_TAG_ID;
            auto lastDot = fullName.rfind('.');
            if (lastDot != std::string::npos)
            {
                std::string parentName = fullName.substr(0, lastDot);
                parentId = RegisterTagUnlocked(parentName);
            }

            GameplayTagId id = m_nextId++;
            GameplayTagInfo info;
            info.id = id;
            info.fullName = fullName;
            info.parentId = parentId;
            m_tags[id] = info;
            m_nameToId[fullName] = id;

            if (parentId != INVALID_TAG_ID)
                m_tags[parentId].children.push_back(id);

            return id;
        }

        GameplayTagId GetTagIdUnlocked(const std::string& fullName) const
        {
            auto it = m_nameToId.find(fullName);
            return it != m_nameToId.end() ? it->second : INVALID_TAG_ID;
        }

        bool IsTagChildOfUnlocked(GameplayTagId tagId, GameplayTagId parentTagId) const
        {
            GameplayTagId current = tagId;
            while (current != INVALID_TAG_ID)
            {
                if (current == parentTagId)
                    return true;
                auto it = m_tags.find(current);
                if (it == m_tags.end())
                    break;
                current = it->second.parentId;
            }
            return false;
        }

        bool m_initialized = false;
        std::thread::id m_gameThreadId = std::this_thread::get_id();
        mutable std::shared_mutex m_registryMutex;
        GameplayTagId m_nextId = 1;
        std::unordered_map<GameplayTagId, GameplayTagInfo> m_tags;
        std::unordered_map<std::string, GameplayTagId> m_nameToId;
    };

} // namespace Spark::Gameplay
