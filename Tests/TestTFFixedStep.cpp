/**
 * @file TestTFFixedStep.cpp
 * @brief Production-linked tests for the TERRAFRONT fixed-step schedule.
 */

#include "TestFramework.h"
#include "Core/TFFixedStep.h"

#include <string>
#include <vector>

namespace
{
    struct FixedStepProbe
    {
        const char* name;
        std::vector<std::string>* order;
        float observedDelta = 0.0f;
        int calls = 0;

        void FixedUpdate(float deltaTime)
        {
            observedDelta = deltaTime;
            ++calls;
            order->emplace_back(name);
        }
    };
} // namespace

TEST(TerrafrontFixedStep_IncludesDamageInAuthoritativeOrder)
{
    std::vector<std::string> order;
    FixedStepProbe server{"server", &order};
    FixedStepProbe players{"players", &order};
    FixedStepProbe abilities{"abilities", &order};
    FixedStepProbe grenades{"grenades", &order};
    FixedStepProbe weapons{"weapons", &order};
    FixedStepProbe damage{"damage", &order};
    FixedStepProbe vehicles{"vehicles", &order};
    FixedStepProbe regions{"regions", &order};
    FixedStepProbe travel{"travel", &order};
    FixedStepProbe bots{"bots", &order};
    FixedStepProbe replication{"replication", &order};

    constexpr float kStep = 1.0f / 60.0f;
    Terrafront::Detail::RunFixedStep(kStep, server, players, abilities, grenades, weapons, damage, vehicles, regions,
                                     travel, bots, replication);

    EXPECT_EQ(order.size(), 11u);
    EXPECT_EQ(order[4], std::string("weapons"));
    EXPECT_EQ(order[5], std::string("damage"));
    EXPECT_EQ(order[6], std::string("vehicles"));
    EXPECT_EQ(damage.calls, 1);
    EXPECT_NEAR(damage.observedDelta, kStep, 0.000001f);
}
