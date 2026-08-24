/**
 * @file TFCharacterSystem.cpp
 * @brief TERRAFRONT character CRUD + enter-world core logic (W5 onboarding, Task 3).
 *
 * Kept minimal-dependency (TFDatabase.h + stdlib) so it links standalone
 * into SparkTests without pulling in TFGameContext or the engine module
 * scaffolding (mirrors TFAccountSystem.cpp, Task 2).
 */
#include "Account/TFCharacterSystem.h"

#include <cctype>
#include <chrono>

namespace Terrafront
{

    namespace
    {

        int64_t NowMs()
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
        }

    } // namespace

    // Reused verbatim (with the (unsigned char) isalnum fix) from the
    // compile+test-verified helper stashed at
    // .superpowers/sdd/stash/trilobite-verified-helpers.md: 3..23 chars,
    // alnum+single-space, no leading/trailing/consecutive spaces.
    bool TFCharacterSystem::ValidateCharacterName(const std::string& name, std::string& err)
    {
        if (name.empty())
        {
            err = "Empty name";
            return false;
        }
        if (name.length() < 3)
        {
            err = "Name too short";
            return false;
        }
        if (name.length() > 23)
        {
            err = "Name too long";
            return false;
        }
        bool spaceFound = false;
        for (size_t i = 0; i < name.length(); ++i)
        {
            char c = name[i];
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != ' ')
            {
                err = "Invalid character in name";
                return false;
            }
            if (c == ' ')
            {
                if (spaceFound || i == 0 || i == name.length() - 1)
                {
                    err = "Leading, trailing, or consecutive spaces";
                    return false;
                }
                spaceFound = true;
            }
            else
            {
                spaceFound = false;
            }
        }
        return true;
    }

    std::vector<TFCharacterRecord> TFCharacterSystem::List(uint64_t accountId)
    {
        if (!m_db)
            return {};
        return m_db->ListCharacters(accountId);
    }

    TFCharCreateResult TFCharacterSystem::Create(uint64_t accountId, const std::string& name, FactionId faction)
    {
        TFCharCreateResult result;
        if (!m_db)
        {
            result.err = TFCharErr::ServerError;
            return result;
        }

        std::string nameErr;
        if (!ValidateCharacterName(name, nameErr) || faction == FactionId::None || faction >= FactionId::COUNT)
        {
            result.err = TFCharErr::NameInvalid;
            return result;
        }

        TFCharacterRecord existing;
        if (m_db->FindCharacterByName(name, existing))
        {
            result.err = TFCharErr::NameTaken;
            return result;
        }

        if (m_db->ListCharacters(accountId).size() >= static_cast<size_t>(kTFMaxCharSlots))
        {
            result.err = TFCharErr::SlotsFull;
            return result;
        }

        TFCharacterRecord rec;
        if (!m_db->CreateCharacter(accountId, name, faction, rec))
        {
            result.err = TFCharErr::ServerError;
            return result;
        }

        result.ok = true;
        result.err = TFCharErr::Ok;
        result.charId = rec.id;
        return result;
    }

    TFCharErr TFCharacterSystem::Delete(uint64_t accountId, uint64_t charId)
    {
        if (!m_db)
            return TFCharErr::ServerError;

        TFCharacterRecord rec;
        if (!m_db->FindCharacter(charId, rec))
            return TFCharErr::NoSuchCharacter;

        if (rec.accountId != accountId)
            return TFCharErr::NotYourCharacter;

        if (!m_db->DeleteCharacter(charId))
            return TFCharErr::ServerError;

        return TFCharErr::Ok;
    }

    bool TFCharacterSystem::EnterWorld(uint64_t accountId, uint64_t charId, TFCharacterRecord& out)
    {
        if (!m_db)
            return false;

        TFCharacterRecord rec;
        if (!m_db->FindCharacter(charId, rec))
            return false;

        if (rec.accountId != accountId)
            return false;

        out = rec;
        return true;
    }

    bool TFCharacterSystem::PersistProgress(uint64_t charId, uint32_t xp, uint16_t rank, uint32_t flux)
    {
        if (!m_db)
            return false;
        return m_db->SaveCharacterProgress(charId, xp, rank, flux, NowMs());
    }

} // namespace Terrafront
