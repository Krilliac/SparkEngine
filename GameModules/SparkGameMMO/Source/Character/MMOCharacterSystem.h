/**
 * @file MMOCharacterSystem.h
 * @brief Character creation, selection, race/class definitions, and appearance
 * @author Spark Engine Team
 * @date 2026
 *
 * Defines playable races and classes with stat modifiers, starting equipment,
 * spawn points, and appearance options. Manages the character creation flow
 * and character selection for an account.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/MMOEnums.h"
#include "Persistence/MMOPersistenceSystem.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace MMO
{

    /// @brief Playable race ID
    enum class RaceId : uint8_t
    {
        Human = 0,
        Elf = 1,
        Dwarf = 2,
        Orc = 3,
        Count = 4
    };

    /// @brief Playable class ID
    enum class ClassId : uint8_t
    {
        Warrior = 0,
        Ranger = 1,
        Mage = 2,
        Healer = 3,
        Engineer = 4,
        Count = 5
    };

    /// @brief Stat modifiers from race/class
    struct StatBlock
    {
        float health = 100.0f;
        float mana = 50.0f;
        float stamina = 100.0f;
        float strength = 10.0f;
        float agility = 10.0f;
        float intellect = 10.0f;
        float armor = 0.0f;
        float moveSpeed = 5.0f;
    };

    /// @brief Race definition
    struct RaceDef
    {
        RaceId id = RaceId::Human;
        std::string name;
        std::string description;
        std::string lore;
        StatBlock baseStats;
        uint32_t startingAreaId = 1;
        float spawnX = 0, spawnY = 1, spawnZ = 0;
        std::vector<uint32_t> startingItems; ///< ItemDef IDs
        std::vector<ClassId> allowedClasses;
    };

    /// @brief Class definition
    struct ClassDef
    {
        ClassId id = ClassId::Warrior;
        std::string name;
        std::string description;
        std::string role; ///< "Tank", "DPS", "Healer", "Support"
        StatBlock statModifiers;
        std::vector<uint32_t> startingItems;
        std::vector<uint32_t> startingRecipes;
        PartyRole defaultPartyRole = PartyRole::DPS;
        std::string iconName;
    };

    /// @brief Appearance customization options
    struct AppearanceOptions
    {
        int faceIndex = 0;
        int hairIndex = 0;
        int hairColorIndex = 0;
        int skinColorIndex = 0;
        int bodyTypeIndex = 0;
        int markingIndex = 0;

        static constexpr int MAX_FACES = 8;
        static constexpr int MAX_HAIRS = 12;
        static constexpr int MAX_HAIR_COLORS = 10;
        static constexpr int MAX_SKIN_COLORS = 8;
        static constexpr int MAX_BODY_TYPES = 3;
        static constexpr int MAX_MARKINGS = 6;
    };

    /// @brief Character creation request
    struct CharacterCreateRequest
    {
        std::string name;
        RaceId race = RaceId::Human;
        ClassId classId = ClassId::Warrior;
        AppearanceOptions appearance;
    };

    /// @brief Character summary for the selection screen
    struct CharacterSummary
    {
        uint32_t characterId = 0;
        std::string name;
        RaceId race = RaceId::Human;
        ClassId classId = ClassId::Warrior;
        int level = 1;
        uint32_t areaId = 1;
        std::string areaName;
        float playTimeHours = 0.0f;
        AppearanceOptions appearance;
        uint64_t lastPlayed = 0;
    };

    /// @brief Result of character creation
    struct CharacterCreateResult
    {
        bool success = false;
        std::string errorMessage;
        uint32_t characterId = 0;
    };

    /**
     * @brief Race/class registry and character CRUD
     */
    class MMOCharacterSystem
    {
      public:
        MMOCharacterSystem() = default;
        ~MMOCharacterSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Shutdown();
        void RenderDebugUI();

        // === Race/Class Registry ===
        const RaceDef* GetRace(RaceId id) const;
        const ClassDef* GetClass(ClassId id) const;
        const std::vector<RaceDef>& GetAllRaces() const { return m_races; }
        const std::vector<ClassDef>& GetAllClasses() const { return m_classes; }
        bool IsValidCombination(RaceId race, ClassId classId) const;

        // === Character CRUD ===
        CharacterCreateResult CreateCharacter(uint32_t accountId, const CharacterCreateRequest& request);
        bool DeleteCharacter(uint32_t accountId, uint32_t characterId);
        std::vector<CharacterSummary> GetCharacters(uint32_t accountId) const;
        const CharacterSummary* GetCharacter(uint32_t characterId) const;
        bool IsNameAvailable(const std::string& name) const;

        // === Character Data ===
        StatBlock ComputeStats(RaceId race, ClassId classId, int level) const;
        CharacterSaveData BuildNewCharacterData(uint32_t accountId, const CharacterCreateRequest& req) const;

        size_t GetTotalCharacters() const { return m_characters.size(); }
        std::string GetCharacterListString(uint32_t accountId) const;

      private:
        void RegisterDefaultRaces();
        void RegisterDefaultClasses();
        bool ValidateName(const std::string& name, std::string& errorOut) const;

        Spark::IEngineContext* m_context{nullptr};
        std::vector<RaceDef> m_races;
        std::vector<ClassDef> m_classes;
        std::unordered_map<uint32_t, CharacterSummary> m_characters;
        std::unordered_map<uint32_t, std::vector<uint32_t>> m_accountCharacters; ///< accountId → charIds
        uint32_t m_nextCharId = 1;

        static constexpr int MIN_NAME_LENGTH = 2;
        static constexpr int MAX_NAME_LENGTH = 16;
    };

} // namespace MMO
