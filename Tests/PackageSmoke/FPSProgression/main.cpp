#include "Game/FPSLocalProfile.h"
#include "Game/ProgressionSystem.h"

#include <iostream>
#include <string>
#include <unordered_map>

int main()
{
    int failures = 0;
    auto check = [&failures](bool condition, const char* message)
    {
        if (!condition)
        {
            std::cerr << message << '\n';
            ++failures;
        }
    };

    Spark::ProgressionSystem earned;
    earned.Initialize();
    check(earned.IsWeaponUnlocked(SparkEditor::WeaponType::PISTOL), "Starting pistol is unavailable");
    check(!earned.IsWeaponUnlocked(SparkEditor::WeaponType::SHOTGUN), "Shotgun unlocked before earning XP");
    earned.AwardXP(600, "consumer");
    check(earned.GetLevel() == 3 && earned.GetCurrentXP() == 600, "XP did not advance to level 3");
    check(earned.IsWeaponUnlocked(SparkEditor::WeaponType::SHOTGUN), "Level 3 did not unlock shotgun");
    check(earned.IsClassUnlocked(SparkEditor::PlayerClass::MEDIC), "Level 2 did not unlock medic");

    Spark::FPSLocalProfile saved;
    saved.progressionXP = earned.GetCurrentXP();
    saved.progressionLevel = earned.GetLevel();
    saved.kills = 7;
    saved.health = 42.5f;
    std::unordered_map<std::string, std::string> state{{"other.module", "keep"}};
    saved.WriteTo(state);
    Spark::FPSLocalProfile loaded;
    std::string error;
    check(loaded.ReadFrom(state, error), "Profile round trip failed");
    check(loaded.progressionXP == 600 && loaded.progressionLevel == 3 && loaded.kills == 7 && loaded.health == 42.5f &&
              state.at("other.module") == "keep",
          "Profile fields or unrelated module state changed");

    Spark::ProgressionSystem restored;
    restored.Initialize();
    int levelEvents = 0;
    int unlockEvents = 0;
    int xpEvents = 0;
    int awardEvents = 0;
    restored.GetCallbacks().onLevelUp = [&](int, const Spark::LevelBonuses&) { ++levelEvents; };
    restored.GetCallbacks().onUnlock = [&](const Spark::LevelUnlock&) { ++unlockEvents; };
    restored.GetCallbacks().onXPGained = [&](int, int) { ++xpEvents; };
    restored.GetCallbacks().onXPAwarded = [&](int base, const std::string& source, int modified)
    {
        ++awardEvents;
        check(base == 500 && source == "after-load" && modified == 510, "XP diagnostic metadata changed");
        check(restored.GetCurrentXP() == 600 && xpEvents == 0, "XP diagnostic callback ran after XP mutation");
    };
    restored.RestoreProgress(loaded.progressionXP);
    check(restored.GetLevel() == 3 && restored.IsWeaponUnlocked(SparkEditor::WeaponType::SHOTGUN),
          "Restore did not reconstruct progression and unlocks");
    check(levelEvents == 0 && unlockEvents == 0 && xpEvents == 0 && awardEvents == 0,
          "Restore emitted live gameplay callbacks");
    restored.AwardXP(500, "after-load");
    check(restored.GetLevel() == 4 && restored.GetCurrentXP() == 1110,
          "Post-load XP bonus or level progression is incorrect");
    check(levelEvents == 1 && unlockEvents == 1 && xpEvents == 1 && awardEvents == 1,
          "Restore did not preserve callbacks for subsequent gameplay");
    check(restored.IsClassUnlocked(SparkEditor::PlayerClass::ENGINEER), "Post-load engineer unlock is missing");
    return failures == 0 ? 0 : 1;
}
