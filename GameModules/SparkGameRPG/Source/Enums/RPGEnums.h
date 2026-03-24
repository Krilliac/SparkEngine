/**
 * @file RPGEnums.h
 * @brief Enumerations for RPG gameplay systems
 * @author Spark Engine Team
 * @date 2026
 *
 * Contains all enum types used by the RPG game module: character classes,
 * damage types, equipment slots, quest states, dialogue nodes, NPC behavior,
 * and combat actions.
 */

#pragma once

#include <cstdint>

namespace RPG
{

    /**
     * @brief Playable character classes with distinct stat growth and abilities
     */
    enum class CharacterClass : uint8_t
    {
        Warrior = 0,
        Mage = 1,
        Ranger = 2,
        Cleric = 3,
        Rogue = 4,
        Paladin = 5,
        Count = 6
    };

    /**
     * @brief Damage types for combat calculations and resistances
     */
    enum class DamageType : uint8_t
    {
        Physical = 0,
        Fire = 1,
        Ice = 2,
        Lightning = 3,
        Holy = 4,
        Shadow = 5,
        Count = 6
    };

    /**
     * @brief Equipment slot positions on a character
     */
    enum class ItemSlot : uint8_t
    {
        Head = 0,
        Chest = 1,
        Legs = 2,
        Feet = 3,
        Hands = 4,
        MainHand = 5,
        OffHand = 6,
        Ring = 7,
        Amulet = 8,
        Count = 9
    };

    /**
     * @brief Quest progression states
     */
    enum class QuestState : uint8_t
    {
        NotStarted = 0,
        InProgress = 1,
        Completed = 2,
        Failed = 3,
        Count = 4
    };

    /**
     * @brief Node types in a branching dialogue tree
     */
    enum class DialogueNodeType : uint8_t
    {
        Text = 0,
        Choice = 1,
        Check = 2,
        Reward = 3,
        End = 4,
        Count = 5
    };

    /**
     * @brief NPC behavioral states
     */
    enum class NPCBehavior : uint8_t
    {
        Idle = 0,
        Patrol = 1,
        Guard = 2,
        Merchant = 3,
        QuestGiver = 4,
        Count = 5
    };

    /**
     * @brief Actions available during combat
     */
    enum class CombatAction : uint8_t
    {
        Attack = 0,
        Defend = 1,
        Cast = 2,
        UseItem = 3,
        Flee = 4,
        Count = 5
    };

    /**
     * @brief Item rarity tiers affecting stats and value
     */
    enum class ItemRarity : uint8_t
    {
        Common = 0,
        Uncommon = 1,
        Rare = 2,
        Epic = 3,
        Legendary = 4,
        Count = 5
    };

    /**
     * @brief NPC disposition toward the player
     */
    enum class NPCDisposition : uint8_t
    {
        Hostile = 0,
        Neutral = 1,
        Friendly = 2,
        Count = 3
    };

    /**
     * @brief Quest objective types
     */
    enum class ObjectiveType : uint8_t
    {
        Kill = 0,
        Collect = 1,
        Talk = 2,
        Explore = 3,
        Escort = 4,
        Count = 5
    };

} // namespace RPG
