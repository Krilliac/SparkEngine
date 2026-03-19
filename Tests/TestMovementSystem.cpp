// TestMovementSystem.cpp - Tests for AI movement generator priority stack
// Standalone implementations for CI testing (no DirectXMath dependency)

#include "TestFramework.h"
#include <array>
#include <memory>
#include <vector>

namespace TestMovement
{

    enum class MovementType
    {
        Idle,
        RandomWander,
        Waypoint,
        Chase,
        Follow,
        Flee,
        Point,
        Home,
        Patrol
    };
    enum class MovementSlot : int
    {
        Default = 0,
        Motion = 1,
        Active = 2,
        Controlled = 3
    };
    static constexpr int SlotCount = 4;

    struct IMovementGenerator
    {
        virtual ~IMovementGenerator() = default;
        virtual MovementType GetType() const = 0;
        virtual MovementSlot GetSlot() const = 0;
        virtual void Initialize() {}
        virtual void Finalize() {}
    };

    // Concrete generators -- slot assignments match production MovementSystem.h

#define DEFINE_GENERATOR(Name, Type, Slot)                                                                             \
    struct Name : IMovementGenerator                                                                                   \
    {                                                                                                                  \
        MovementType GetType() const override                                                                          \
        {                                                                                                              \
            return MovementType::Type;                                                                                 \
        }                                                                                                              \
        MovementSlot GetSlot() const override                                                                          \
        {                                                                                                              \
            return MovementSlot::Slot;                                                                                 \
        }

    DEFINE_GENERATOR(IdleGenerator, Idle, Default)
};

DEFINE_GENERATOR(RandomWanderGenerator, RandomWander, Default)
float radius;
explicit RandomWanderGenerator(float r = 10.0f) : radius(r) {}
}
;

DEFINE_GENERATOR(ChaseGenerator, Chase, Motion)
uint32_t targetId;
explicit ChaseGenerator(uint32_t id) : targetId(id) {}
}
;

DEFINE_GENERATOR(FollowGenerator, Follow, Motion)
uint32_t targetId;
float distance;
FollowGenerator(uint32_t id, float d) : targetId(id), distance(d) {}
}
;

DEFINE_GENERATOR(FleeGenerator, Flee, Active)
uint32_t threatId;
float fleeDistance;
float duration;
FleeGenerator(uint32_t id, float d, float t) : threatId(id), fleeDistance(d), duration(t) {}
}
;

DEFINE_GENERATOR(PointGenerator, Point, Motion)
float x, y, z;
PointGenerator(float px, float py, float pz) : x(px), y(py), z(pz) {}
}
;

DEFINE_GENERATOR(HomeGenerator, Home, Motion)
float x, y, z;
HomeGenerator(float hx, float hy, float hz) : x(hx), y(hy), z(hz) {}
}
;

DEFINE_GENERATOR(PatrolGenerator, Patrol, Default)
std::vector<std::array<float, 3>> waypoints;
bool loop;
PatrolGenerator(std::vector<std::array<float, 3>> pts, bool l) : waypoints(std::move(pts)), loop(l) {}
}
;

#undef DEFINE_GENERATOR

class MotionController
{
  public:
    void SetGenerator(std::unique_ptr<IMovementGenerator> gen)
    {
        int slot = static_cast<int>(gen->GetSlot());
        if (m_slots[slot])
            m_slots[slot]->Finalize();
        m_slots[slot] = std::move(gen);
        m_slots[slot]->Initialize();
    }
    void ClearSlot(MovementSlot slot)
    {
        int i = static_cast<int>(slot);
        if (m_slots[i])
        {
            m_slots[i]->Finalize();
            m_slots[i].reset();
        }
    }
    void ClearAll()
    {
        for (auto& s : m_slots)
            if (s)
            {
                s->Finalize();
                s.reset();
            }
    }
    IMovementGenerator* GetActiveGenerator() const
    {
        for (int i = SlotCount - 1; i >= 0; --i)
            if (m_slots[i])
                return m_slots[i].get();
        return nullptr;
    }
    MovementType GetActiveMovementType() const
    {
        auto* g = GetActiveGenerator();
        return g ? g->GetType() : MovementType::Idle;
    }
    bool HasMovement() const { return GetActiveGenerator() != nullptr; }
    bool HasMovementInSlot(MovementSlot s) const { return m_slots[static_cast<int>(s)] != nullptr; }

  private:
    std::array<std::unique_ptr<IMovementGenerator>, SlotCount> m_slots{};
};

class MovementSystem
{
  public:
    static MovementSystem& GetInstance()
    {
        static MovementSystem s;
        return s;
    }

    void MoveIdle(MotionController& c) { c.SetGenerator(std::make_unique<IdleGenerator>()); }
    void MoveChase(MotionController& c, uint32_t id) { c.SetGenerator(std::make_unique<ChaseGenerator>(id)); }
    void MoveFollow(MotionController& c, uint32_t id, float d)
    {
        c.SetGenerator(std::make_unique<FollowGenerator>(id, d));
    }
    void MoveFlee(MotionController& c, uint32_t id, float d, float t)
    {
        c.SetGenerator(std::make_unique<FleeGenerator>(id, d, t));
    }
    void MovePoint(MotionController& c, float x, float y, float z)
    {
        c.SetGenerator(std::make_unique<PointGenerator>(x, y, z));
    }
    void MoveHome(MotionController& c, float x, float y, float z)
    {
        c.SetGenerator(std::make_unique<HomeGenerator>(x, y, z));
    }
    void MoveRandomWander(MotionController& c, float r) { c.SetGenerator(std::make_unique<RandomWanderGenerator>(r)); }
    void MovePatrol(MotionController& c, std::vector<std::array<float, 3>> wp, bool loop)
    {
        c.SetGenerator(std::make_unique<PatrolGenerator>(std::move(wp), loop));
    }
    void StopMovement(MotionController& c, MovementSlot s) { c.ClearSlot(s); }
    void StopAllMovement(MotionController& c) { c.ClearAll(); }
    MovementType GetMovementType(const MotionController& c) const { return c.GetActiveMovementType(); }

  private:
    MovementSystem() = default;
};

} // namespace TestMovement

// ============================================================================
// Tests
// ============================================================================

TEST(Movement_SlotPriorityHigherSlotWins)
{
    using namespace TestMovement;
    MotionController ctrl;
    ctrl.SetGenerator(std::make_unique<IdleGenerator>());          // Default(0)
    ctrl.SetGenerator(std::make_unique<FleeGenerator>(42, 20, 5)); // Active(2)

    auto* active = ctrl.GetActiveGenerator();
    EXPECT_TRUE(active != nullptr);
    EXPECT_EQ(static_cast<int>(active->GetType()), static_cast<int>(MovementType::Flee));
    EXPECT_EQ(static_cast<int>(active->GetSlot()), static_cast<int>(MovementSlot::Active));
}

TEST(Movement_SetIdleGeneratorOnDefaultSlot)
{
    using namespace TestMovement;
    MotionController ctrl;
    ctrl.SetGenerator(std::make_unique<IdleGenerator>());

    EXPECT_TRUE(ctrl.HasMovement());
    EXPECT_TRUE(ctrl.HasMovementInSlot(MovementSlot::Default));
    EXPECT_EQ(static_cast<int>(ctrl.GetActiveMovementType()), static_cast<int>(MovementType::Idle));
}

TEST(Movement_ChaseOverridesIdle)
{
    using namespace TestMovement;
    auto& sys = MovementSystem::GetInstance();
    MotionController ctrl;

    sys.MoveIdle(ctrl);
    EXPECT_EQ(static_cast<int>(sys.GetMovementType(ctrl)), static_cast<int>(MovementType::Idle));

    sys.MoveChase(ctrl, 99); // Motion(1) > Default(0)
    EXPECT_EQ(static_cast<int>(sys.GetMovementType(ctrl)), static_cast<int>(MovementType::Chase));
    EXPECT_TRUE(ctrl.HasMovementInSlot(MovementSlot::Default)); // Idle still present
    EXPECT_TRUE(ctrl.HasMovementInSlot(MovementSlot::Motion));
}

TEST(Movement_ClearSlotRestoresLowerPriority)
{
    using namespace TestMovement;
    MotionController ctrl;
    ctrl.SetGenerator(std::make_unique<IdleGenerator>());   // Default(0)
    ctrl.SetGenerator(std::make_unique<ChaseGenerator>(1)); // Motion(1)
    EXPECT_EQ(static_cast<int>(ctrl.GetActiveMovementType()), static_cast<int>(MovementType::Chase));

    ctrl.ClearSlot(MovementSlot::Motion);
    EXPECT_EQ(static_cast<int>(ctrl.GetActiveMovementType()), static_cast<int>(MovementType::Idle));
    EXPECT_FALSE(ctrl.HasMovementInSlot(MovementSlot::Motion));
}

TEST(Movement_ClearAllRemovesEverything)
{
    using namespace TestMovement;
    MotionController ctrl;
    ctrl.SetGenerator(std::make_unique<IdleGenerator>());
    ctrl.SetGenerator(std::make_unique<ChaseGenerator>(5));
    ctrl.SetGenerator(std::make_unique<FleeGenerator>(3, 15, 4));
    EXPECT_TRUE(ctrl.HasMovement());

    ctrl.ClearAll();
    EXPECT_FALSE(ctrl.HasMovement());
    EXPECT_FALSE(ctrl.HasMovementInSlot(MovementSlot::Default));
    EXPECT_FALSE(ctrl.HasMovementInSlot(MovementSlot::Motion));
    EXPECT_FALSE(ctrl.HasMovementInSlot(MovementSlot::Active));
    EXPECT_FALSE(ctrl.HasMovementInSlot(MovementSlot::Controlled));
}

TEST(Movement_PointMovementWithTargetCoordinates)
{
    using namespace TestMovement;
    auto& sys = MovementSystem::GetInstance();
    MotionController ctrl;

    sys.MovePoint(ctrl, 100.0f, 25.0f, -50.0f);
    EXPECT_EQ(static_cast<int>(sys.GetMovementType(ctrl)), static_cast<int>(MovementType::Point));
    EXPECT_TRUE(ctrl.HasMovementInSlot(MovementSlot::Motion));

    auto* gen = dynamic_cast<PointGenerator*>(ctrl.GetActiveGenerator());
    EXPECT_TRUE(gen != nullptr);
    EXPECT_NEAR(gen->x, 100.0f, 0.001f);
    EXPECT_NEAR(gen->y, 25.0f, 0.001f);
    EXPECT_NEAR(gen->z, -50.0f, 0.001f);
}

TEST(Movement_PatrolWaypointsWithLoopFlag)
{
    using namespace TestMovement;
    auto& sys = MovementSystem::GetInstance();
    MotionController ctrl;

    std::vector<std::array<float, 3>> wp = {{{0, 0, 0}}, {{10, 0, 0}}, {{10, 0, 10}}, {{0, 0, 10}}};
    sys.MovePatrol(ctrl, wp, true);
    EXPECT_EQ(static_cast<int>(sys.GetMovementType(ctrl)), static_cast<int>(MovementType::Patrol));
    EXPECT_TRUE(ctrl.HasMovementInSlot(MovementSlot::Default));

    auto* gen = dynamic_cast<PatrolGenerator*>(ctrl.GetActiveGenerator());
    EXPECT_TRUE(gen != nullptr);
    EXPECT_EQ(static_cast<int>(gen->waypoints.size()), 4);
    EXPECT_TRUE(gen->loop);
    EXPECT_NEAR(gen->waypoints[2][2], 10.0f, 0.001f);

    // Non-looping patrol
    MotionController ctrl2;
    sys.MovePatrol(ctrl2, {{{0, 0, 0}}, {{5, 0, 0}}}, false);
    auto* gen2 = dynamic_cast<PatrolGenerator*>(ctrl2.GetActiveGenerator());
    EXPECT_TRUE(gen2 != nullptr);
    EXPECT_FALSE(gen2->loop);
}

TEST(Movement_FleeGeneratorWithDistanceAndDuration)
{
    using namespace TestMovement;
    auto& sys = MovementSystem::GetInstance();
    MotionController ctrl;

    sys.MoveFlee(ctrl, 77, 30.0f, 8.0f);
    EXPECT_EQ(static_cast<int>(sys.GetMovementType(ctrl)), static_cast<int>(MovementType::Flee));
    EXPECT_TRUE(ctrl.HasMovementInSlot(MovementSlot::Active));

    auto* gen = dynamic_cast<FleeGenerator*>(ctrl.GetActiveGenerator());
    EXPECT_TRUE(gen != nullptr);
    EXPECT_EQ(gen->threatId, static_cast<uint32_t>(77));
    EXPECT_NEAR(gen->fleeDistance, 30.0f, 0.001f);
    EXPECT_NEAR(gen->duration, 8.0f, 0.001f);
}

TEST(Movement_StopMovementForSpecificSlot)
{
    using namespace TestMovement;
    auto& sys = MovementSystem::GetInstance();
    MotionController ctrl;

    sys.MoveIdle(ctrl);                  // Default(0)
    sys.MoveChase(ctrl, 10);             // Motion(1)
    sys.MoveFlee(ctrl, 20, 15.0f, 3.0f); // Active(2)

    sys.StopMovement(ctrl, MovementSlot::Motion);
    EXPECT_FALSE(ctrl.HasMovementInSlot(MovementSlot::Motion));
    EXPECT_TRUE(ctrl.HasMovementInSlot(MovementSlot::Default));
    EXPECT_TRUE(ctrl.HasMovementInSlot(MovementSlot::Active));
    EXPECT_EQ(static_cast<int>(sys.GetMovementType(ctrl)), static_cast<int>(MovementType::Flee));
}

TEST(Movement_GetMovementTypeReflectsActiveGenerator)
{
    using namespace TestMovement;
    auto& sys = MovementSystem::GetInstance();
    MotionController ctrl;

    EXPECT_EQ(static_cast<int>(sys.GetMovementType(ctrl)), static_cast<int>(MovementType::Idle));

    sys.MoveRandomWander(ctrl, 15.0f);
    EXPECT_EQ(static_cast<int>(sys.GetMovementType(ctrl)), static_cast<int>(MovementType::RandomWander));

    sys.MoveHome(ctrl, 0, 0, 0); // Motion(1) overrides Default(0)
    EXPECT_EQ(static_cast<int>(sys.GetMovementType(ctrl)), static_cast<int>(MovementType::Home));

    sys.MoveFlee(ctrl, 3, 20, 5); // Active(2) overrides Motion(1)
    EXPECT_EQ(static_cast<int>(sys.GetMovementType(ctrl)), static_cast<int>(MovementType::Flee));
}

TEST(Movement_MultipleGeneratorsInDifferentSlots)
{
    using namespace TestMovement;
    MotionController ctrl;

    ctrl.SetGenerator(std::make_unique<IdleGenerator>());         // Default(0)
    ctrl.SetGenerator(std::make_unique<HomeGenerator>(0, 0, 0));  // Motion(1)
    ctrl.SetGenerator(std::make_unique<FleeGenerator>(1, 10, 5)); // Active(2)

    EXPECT_TRUE(ctrl.HasMovementInSlot(MovementSlot::Default));
    EXPECT_TRUE(ctrl.HasMovementInSlot(MovementSlot::Motion));
    EXPECT_TRUE(ctrl.HasMovementInSlot(MovementSlot::Active));
    EXPECT_EQ(static_cast<int>(ctrl.GetActiveMovementType()), static_cast<int>(MovementType::Flee));

    ctrl.ClearSlot(MovementSlot::Active);
    EXPECT_EQ(static_cast<int>(ctrl.GetActiveMovementType()), static_cast<int>(MovementType::Home));

    ctrl.ClearSlot(MovementSlot::Motion);
    EXPECT_EQ(static_cast<int>(ctrl.GetActiveMovementType()), static_cast<int>(MovementType::Idle));
}

TEST(Movement_HasMovementChecks)
{
    using namespace TestMovement;
    MotionController ctrl;

    EXPECT_FALSE(ctrl.HasMovement());
    EXPECT_FALSE(ctrl.HasMovementInSlot(MovementSlot::Default));

    ctrl.SetGenerator(std::make_unique<FleeGenerator>(1, 10, 3));
    EXPECT_TRUE(ctrl.HasMovement());
    EXPECT_TRUE(ctrl.HasMovementInSlot(MovementSlot::Active));
    EXPECT_FALSE(ctrl.HasMovementInSlot(MovementSlot::Default));

    ctrl.ClearSlot(MovementSlot::Active);
    EXPECT_FALSE(ctrl.HasMovement());

    // Replace in same slot: Chase then Follow both go to Motion(1)
    ctrl.SetGenerator(std::make_unique<ChaseGenerator>(10));
    ctrl.SetGenerator(std::make_unique<FollowGenerator>(20, 5.0f));
    EXPECT_TRUE(ctrl.HasMovementInSlot(MovementSlot::Motion));
    EXPECT_EQ(static_cast<int>(ctrl.GetActiveMovementType()), static_cast<int>(MovementType::Follow));
}
