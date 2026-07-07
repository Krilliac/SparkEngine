// Test_gameplay_instance.cpp — regression tests for InstanceManager completion logic.
//
// AllEncountersDone previously returned false for ANY encounter not in the Done state,
// including optional encounters players legitimately skip — so an instance containing an
// optional encounter could never set completed=true. The fix cross-references the
// template's optional flags in CompleteEncounter.

#include "../TestFramework.h"
#include "Engine/Gameplay/InstanceManager.h"

using namespace Spark::Gameplay;

TEST(Instance_CompletesWhenOptionalEncounterSkipped)
{
    auto& mgr = InstanceManager::GetInstance();
    mgr.Initialize();

    InstanceTemplate tmpl;
    tmpl.templateId = 500;
    tmpl.name = "TestDungeon";
    EncounterDefinition required;
    required.id = 1;
    required.optional = false;
    EncounterDefinition optional;
    optional.id = 2;
    optional.optional = true;
    tmpl.encounters.push_back(required);
    tmpl.encounters.push_back(optional);
    mgr.RegisterTemplate(tmpl);

    InstanceID id = mgr.CreateInstance(500);
    ASSERT_TRUE(id != 0);

    // Complete only the required encounter; leave the optional one NotStarted.
    ASSERT_TRUE(mgr.StartEncounter(id, 1));
    mgr.CompleteEncounter(id, 1);

    const InstanceData* data = mgr.GetInstanceData(id);
    ASSERT_TRUE(data != nullptr);
    EXPECT_TRUE(data->completed); // false before the fix
}

TEST(Instance_NotCompleteWhileRequiredEncounterPending)
{
    auto& mgr = InstanceManager::GetInstance();
    mgr.Initialize();

    InstanceTemplate tmpl;
    tmpl.templateId = 501;
    EncounterDefinition r1;
    r1.id = 1;
    r1.optional = false;
    EncounterDefinition r2;
    r2.id = 2;
    r2.optional = false;
    tmpl.encounters.push_back(r1);
    tmpl.encounters.push_back(r2);
    mgr.RegisterTemplate(tmpl);

    InstanceID id = mgr.CreateInstance(501);
    ASSERT_TRUE(id != 0);

    ASSERT_TRUE(mgr.StartEncounter(id, 1));
    mgr.CompleteEncounter(id, 1);

    const InstanceData* data = mgr.GetInstanceData(id);
    ASSERT_TRUE(data != nullptr);
    EXPECT_FALSE(data->completed); // second required encounter still pending
}
