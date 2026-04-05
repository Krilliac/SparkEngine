// TestSparkGameRacing.cpp - Tests for Racing game module mechanics
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <limits>

namespace TestRacing
{

    struct LapRecord
    {
        int lapNumber = 0;
        float lapTime = 0.0f;
    };

    struct Checkpoint
    {
        int id = 0;
        bool passed = false;
    };

    struct Racer
    {
        std::string name;
        float distanceTraveled = 0.0f;
        int currentLap = 0;
        float currentLapTime = 0.0f;
        float totalRaceTime = 0.0f;
        float bestLapTime = std::numeric_limits<float>::max();
        bool dnf = false;
        int position = 0;
        std::vector<LapRecord> completedLaps;
        std::vector<Checkpoint> checkpoints;

        void InitCheckpoints(int count)
        {
            checkpoints.clear();
            for (int i = 0; i < count; i++)
            {
                checkpoints.push_back({i, false});
            }
        }

        void PassCheckpoint(int id)
        {
            for (auto& cp : checkpoints)
            {
                if (cp.id == id)
                {
                    cp.passed = true;
                    return;
                }
            }
        }

        bool AllCheckpointsPassed() const
        {
            for (const auto& cp : checkpoints)
            {
                if (!cp.passed)
                {
                    return false;
                }
            }
            return true;
        }

        bool CompleteLap()
        {
            if (!AllCheckpointsPassed())
            {
                return false;
            }

            currentLap++;
            LapRecord record;
            record.lapNumber = currentLap;
            record.lapTime = currentLapTime;
            completedLaps.push_back(record);

            if (currentLapTime < bestLapTime)
            {
                bestLapTime = currentLapTime;
            }

            currentLapTime = 0.0f;

            // Reset checkpoints for next lap
            for (auto& cp : checkpoints)
            {
                cp.passed = false;
            }

            return true;
        }

        void Update(float dt)
        {
            if (dnf)
            {
                return;
            }
            currentLapTime += dt;
            totalRaceTime += dt;
        }
    };

    static void RankRacers(std::vector<Racer*>& racers)
    {
        std::sort(racers.begin(), racers.end(),
                  [](const Racer* a, const Racer* b)
                  {
                      if (a->currentLap != b->currentLap)
                      {
                          return a->currentLap > b->currentLap;
                      }
                      return a->distanceTraveled > b->distanceTraveled;
                  });

        for (int i = 0; i < static_cast<int>(racers.size()); i++)
        {
            racers[static_cast<size_t>(i)]->position = i + 1;
        }
    }

} // namespace TestRacing

TEST(Racing_LapTiming)
{
    TestRacing::Racer racer;
    racer.name = "Player1";
    racer.InitCheckpoints(3);

    // Simulate first lap: 30 seconds
    racer.Update(30.0f);
    racer.PassCheckpoint(0);
    racer.PassCheckpoint(1);
    racer.PassCheckpoint(2);
    EXPECT_TRUE(racer.CompleteLap());

    EXPECT_EQ(racer.currentLap, 1);
    EXPECT_EQ(static_cast<int>(racer.completedLaps.size()), 1);
    EXPECT_NEAR(racer.completedLaps[0].lapTime, 30.0f, 0.001f);

    // Simulate second lap: 28 seconds
    racer.Update(28.0f);
    racer.PassCheckpoint(0);
    racer.PassCheckpoint(1);
    racer.PassCheckpoint(2);
    EXPECT_TRUE(racer.CompleteLap());

    EXPECT_EQ(racer.currentLap, 2);
    EXPECT_EQ(static_cast<int>(racer.completedLaps.size()), 2);
    EXPECT_NEAR(racer.completedLaps[1].lapTime, 28.0f, 0.001f);
}

TEST(Racing_CheckpointValidation)
{
    TestRacing::Racer racer;
    racer.name = "Player1";
    racer.InitCheckpoints(3);

    racer.Update(25.0f);

    // Only pass 2 of 3 checkpoints
    racer.PassCheckpoint(0);
    racer.PassCheckpoint(2);

    // Lap should not complete — missed checkpoint 1
    EXPECT_FALSE(racer.CompleteLap());
    EXPECT_EQ(racer.currentLap, 0);
    EXPECT_EQ(static_cast<int>(racer.completedLaps.size()), 0);

    // Now pass the missing checkpoint
    racer.PassCheckpoint(1);
    EXPECT_TRUE(racer.CompleteLap());
    EXPECT_EQ(racer.currentLap, 1);
}

TEST(Racing_PositionTracking)
{
    TestRacing::Racer racer1;
    racer1.name = "Alice";
    racer1.currentLap = 2;
    racer1.distanceTraveled = 500.0f;

    TestRacing::Racer racer2;
    racer2.name = "Bob";
    racer2.currentLap = 2;
    racer2.distanceTraveled = 800.0f;

    TestRacing::Racer racer3;
    racer3.name = "Charlie";
    racer3.currentLap = 3;
    racer3.distanceTraveled = 200.0f;

    std::vector<TestRacing::Racer*> racers = {&racer1, &racer2, &racer3};
    TestRacing::RankRacers(racers);

    // Charlie is on lap 3 so 1st; Bob has more distance on lap 2 so 2nd; Alice 3rd
    EXPECT_EQ(racer3.position, 1);
    EXPECT_EQ(racer2.position, 2);
    EXPECT_EQ(racer1.position, 3);
}

TEST(Racing_DNFTimeout)
{
    TestRacing::Racer racer;
    racer.name = "SlowDriver";
    racer.InitCheckpoints(2);

    float timeoutLimit = 120.0f;

    // Simulate time passing
    racer.Update(130.0f);

    // Check timeout
    if (racer.totalRaceTime > timeoutLimit && racer.currentLap == 0)
    {
        racer.dnf = true;
    }

    EXPECT_TRUE(racer.dnf);

    // DNF racer should not accumulate more time
    float timeBeforeUpdate = racer.totalRaceTime;
    racer.Update(10.0f);
    EXPECT_NEAR(racer.totalRaceTime, timeBeforeUpdate, 0.001f);
}

TEST(Racing_BestLapTracking)
{
    TestRacing::Racer racer;
    racer.name = "Speedster";
    racer.InitCheckpoints(2);

    // Lap 1: 35 seconds
    racer.Update(35.0f);
    racer.PassCheckpoint(0);
    racer.PassCheckpoint(1);
    racer.CompleteLap();
    EXPECT_NEAR(racer.bestLapTime, 35.0f, 0.001f);

    // Lap 2: 29 seconds (new best)
    racer.Update(29.0f);
    racer.PassCheckpoint(0);
    racer.PassCheckpoint(1);
    racer.CompleteLap();
    EXPECT_NEAR(racer.bestLapTime, 29.0f, 0.001f);

    // Lap 3: 32 seconds (not best)
    racer.Update(32.0f);
    racer.PassCheckpoint(0);
    racer.PassCheckpoint(1);
    racer.CompleteLap();
    EXPECT_NEAR(racer.bestLapTime, 29.0f, 0.001f);

    EXPECT_EQ(racer.currentLap, 3);
    EXPECT_EQ(static_cast<int>(racer.completedLaps.size()), 3);
}
