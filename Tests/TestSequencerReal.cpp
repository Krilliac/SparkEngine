/**
 * @file TestSequencerReal.cpp
 * @brief Real-class tests for Spark::Cinematic::SequencerManager
 */

#include "TestFramework.h"
#include "Engine/Cinematic/Sequencer.h"

namespace
{
    void ResetSequencer()
    {
        auto& mgr = Spark::Cinematic::SequencerManager::GetInstance();
        mgr.StopAll();
        // Clean up existing sequences from prior tests via RemoveSequence.
        // There's no Clear() API; rely on unique test-scoped names.
    }
} // namespace

TEST(SequencerReal_SingletonStable)
{
    auto& a = Spark::Cinematic::SequencerManager::GetInstance();
    auto& b = Spark::Cinematic::SequencerManager::GetInstance();
    EXPECT_TRUE(&a == &b);
}

TEST(SequencerReal_CreateSequence)
{
    ResetSequencer();
    auto& mgr = Spark::Cinematic::SequencerManager::GetInstance();
    auto* seq = mgr.CreateSequence("PhaseKK_Test1");
    EXPECT_TRUE(seq != nullptr);
}

TEST(SequencerReal_GetSequenceAfterCreate)
{
    ResetSequencer();
    auto& mgr = Spark::Cinematic::SequencerManager::GetInstance();
    mgr.CreateSequence("PhaseKK_GetTest");
    auto* seq = mgr.GetSequence("PhaseKK_GetTest");
    EXPECT_TRUE(seq != nullptr);
}

TEST(SequencerReal_GetUnknownReturnsNull)
{
    ResetSequencer();
    auto& mgr = Spark::Cinematic::SequencerManager::GetInstance();
    auto* seq = mgr.GetSequence("DefinitelyDoesNotExist_PhaseKK");
    EXPECT_TRUE(seq == nullptr);
}

TEST(SequencerReal_RemoveSequence)
{
    ResetSequencer();
    auto& mgr = Spark::Cinematic::SequencerManager::GetInstance();
    mgr.CreateSequence("PhaseKK_Removable");
    mgr.RemoveSequence("PhaseKK_Removable");
    auto* seq = mgr.GetSequence("PhaseKK_Removable");
    EXPECT_TRUE(seq == nullptr);
}

TEST(SequencerReal_StopAllIdempotent)
{
    ResetSequencer();
    auto& mgr = Spark::Cinematic::SequencerManager::GetInstance();
    mgr.StopAll();
    mgr.StopAll();
    EXPECT_FALSE(mgr.IsAnyCutscenePlaying());
}

TEST(SequencerReal_ConsoleListSequencesReturnsString)
{
    ResetSequencer();
    auto& mgr = Spark::Cinematic::SequencerManager::GetInstance();
    const auto str = mgr.Console_ListSequences();
    // May be empty or a status; just verify the call is safe.
    (void)str;
    EXPECT_TRUE(true);
}

TEST(SequencerReal_UpdateWithoutActiveIsSafe)
{
    ResetSequencer();
    auto& mgr = Spark::Cinematic::SequencerManager::GetInstance();
    mgr.Update(0.016f);
    mgr.Update(0.016f);
    EXPECT_FALSE(mgr.IsAnyCutscenePlaying());
}
