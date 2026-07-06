/**
 * @file TFCharacterSystem.h
 * @brief TERRAFRONT character CRUD + enter-world core logic (W5 onboarding, Task 3).
 *
 * Core logic only: operates directly on a `TFDatabase*` (no TFGameContext),
 * so it is unit-testable standalone, mirroring TFAccountSystem (Task 2).
 * Character model is faction + name + persistent progression; class stays
 * per-spawn (not stored here).
 */
#pragma once

#include "Persistence/TFDatabase.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Terrafront {

constexpr int kTFMaxCharSlots = 5;

enum class TFCharErr : uint8_t { Ok = 0, SlotsFull, NameTaken, NameInvalid, NoSuchCharacter, NotYourCharacter, ServerError };

struct TFCharCreateResult
{
    bool        ok = false;
    TFCharErr   err = TFCharErr::ServerError;
    uint64_t    charId = 0;
};

class TFCharacterSystem
{
  public:
    void SetDatabase(TFDatabase* db) { m_db = db; }

    std::vector<TFCharacterRecord> List(uint64_t accountId);

    // name 3..23, alnum+single-space (no lead/trail/double space), unique, valid faction, slot cap
    TFCharCreateResult Create(uint64_t accountId, const std::string& name, FactionId faction);

    TFCharErr Delete(uint64_t accountId, uint64_t charId);   // ownership-checked

    // enter-world binding (Task 4): returns the character (faction becomes authoritative)
    bool EnterWorld(uint64_t accountId, uint64_t charId, TFCharacterRecord& out);

    void PersistProgress(uint64_t charId, uint32_t xp, uint16_t rank, uint32_t flux);   // routes to db

    // Exposed for reuse (net-layer validation in Task 4/5) and unit testing.
    static bool ValidateCharacterName(const std::string& name, std::string& err);

  private:
    TFDatabase* m_db = nullptr;
};

} // namespace Terrafront
