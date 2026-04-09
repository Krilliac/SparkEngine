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

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
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
         * @brief Initialize the registry
         */
        void Initialize() override
        {
            m_nextId = 1;
            m_tags.clear();
            m_nameToId.clear();
            m_initialized = true;
        }

        /**
         * @brief Shut down and clear all registered tags
         */
        void Shutdown() override
        {
            m_tags.clear();
            m_nameToId.clear();
            m_initialized = false;
        }

        uint32_t RegisterTag(std::string_view fullName) override { return RegisterTag(std::string(fullName)); }

        /**
         * @brief Register a tag (and all parent tags implicitly)
         * @param fullName Dot-separated tag name (e.g. "Damage.Fire.DoT")
         * @return The tag ID for the registered tag
         *
         * If the tag already exists, returns its existing ID.
         * Parent tags (e.g. "Damage", "Damage.Fire") are created automatically.
         */
        GameplayTagId RegisterTag(const std::string& fullName)
        {
            if (fullName.empty())
                return INVALID_TAG_ID;

            auto existing = m_nameToId.find(fullName);
            if (existing != m_nameToId.end())
                return existing->second;

            // Register parent tags first
            GameplayTagId parentId = INVALID_TAG_ID;
            auto lastDot = fullName.rfind('.');
            if (lastDot != std::string::npos)
            {
                std::string parentName = fullName.substr(0, lastDot);
                parentId = RegisterTag(parentName);
            }

            GameplayTagId id = m_nextId++;
            GameplayTagInfo info;
            info.id = id;
            info.fullName = fullName;
            info.parentId = parentId;
            m_tags[id] = info;
            m_nameToId[fullName] = id;

            // Add as child of parent
            if (parentId != INVALID_TAG_ID)
                m_tags[parentId].children.push_back(id);

            return id;
        }

        /**
         * @brief Get the ID for a tag name
         * @param fullName Dot-separated tag name
         * @return Tag ID, or INVALID_TAG_ID if not registered
         */
        uint32_t GetTagId(std::string_view fullName) const override { return GetTagId(std::string(fullName)); }

        GameplayTagId GetTagId(const std::string& fullName) const
        {
            auto it = m_nameToId.find(fullName);
            return it != m_nameToId.end() ? it->second : INVALID_TAG_ID;
        }

        /**
         * @brief Get tag info by ID
         * @param id Tag ID
         * @return Pointer to tag info, or nullptr if not found
         */
        const GameplayTagInfo* GetTagInfo(GameplayTagId id) const
        {
            auto it = m_tags.find(id);
            return it != m_tags.end() ? &it->second : nullptr;
        }

        /**
         * @brief Check if a tag matches another tag or is a child of it
         * @param tagId The tag to check
         * @param parentTagId The potential parent tag
         * @return true if tagId equals parentTagId or is a descendant
         */
        bool IsTagChildOf(GameplayTagId tagId, GameplayTagId parentTagId) const
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

        /**
         * @brief Check if a container has a tag matching the given tag or any child of it
         * @param container The tag container to search
         * @param parentTagName Tag name to match (also matches children)
         * @return true if any tag in the container matches
         */
        bool ContainerMatchesTag(const GameplayTagContainer& container, const std::string& parentTagName) const
        {
            GameplayTagId parentId = GetTagId(parentTagName);
            if (parentId == INVALID_TAG_ID)
                return false;

            for (auto tagId : container.GetTags())
            {
                if (IsTagChildOf(tagId, parentId))
                    return true;
            }
            return false;
        }

        /**
         * @brief Get the total number of registered tags
         * @return Number of tags
         */
        size_t GetTagCount() const { return m_tags.size(); }

        /**
         * @brief Get all registered tag names
         * @return Vector of tag names sorted alphabetically
         */
        std::vector<std::string> GetAllTagNames() const
        {
            std::vector<std::string> names;
            names.reserve(m_nameToId.size());
            for (const auto& [name, id] : m_nameToId)
                names.push_back(name);
            std::sort(names.begin(), names.end());
            return names;
        }

        /**
         * @brief Get status string for console/debug display
         * @return Formatted status string
         */
        std::string Console_GetStatus() const
        {
            return "GameplayTagRegistry: " + std::to_string(m_tags.size()) + " tags registered";
        }

      private:
        GameplayTagRegistry() = default;

        bool m_initialized = false;
        GameplayTagId m_nextId = 1;
        std::unordered_map<GameplayTagId, GameplayTagInfo> m_tags;
        std::unordered_map<std::string, GameplayTagId> m_nameToId;
    };

} // namespace Spark::Gameplay
