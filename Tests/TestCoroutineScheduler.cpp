/**
 * @file TestCoroutineScheduler.cpp
 * @brief Tests for the gameplay coroutine/task scheduler
 */

#include "TestFramework.h"
#include <functional>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>

// ============================================================================
// Standalone reimplementation for cross-platform testing
// ============================================================================

namespace
{

    class YieldInstruction
    {
      public:
        virtual ~YieldInstruction() = default;
        virtual bool IsReady(float dt) = 0;
        virtual void Reset() {}
    };

    class WaitForSeconds : public YieldInstruction
    {
      public:
        explicit WaitForSeconds(float s) : target(s) {}
        bool IsReady(float dt) override
        {
            elapsed += dt;
            return elapsed >= target;
        }
        void Reset() override { elapsed = 0; }

      private:
        float target, elapsed = 0;
    };

    class WaitForFrames : public YieldInstruction
    {
      public:
        explicit WaitForFrames(int n) : target(n) {}
        bool IsReady(float) override { return ++count >= target; }
        void Reset() override { count = 0; }

      private:
        int target, count = 0;
    };

    class WaitUntil : public YieldInstruction
    {
      public:
        explicit WaitUntil(std::function<bool()> p) : pred(std::move(p)) {}
        bool IsReady(float) override { return pred && pred(); }

      private:
        std::function<bool()> pred;
    };

    class Coroutine
    {
      public:
        explicit Coroutine(const std::string& name) : m_name(name) {}
        const std::string& GetName() const { return m_name; }
        bool IsFinished() const { return m_step >= (int)m_steps.size(); }
        void Cancel() { m_cancelled = true; }
        bool IsCancelled() const { return m_cancelled; }

        Coroutine& Do(std::function<void()> action)
        {
            m_steps.push_back({Step::Action, std::move(action), nullptr});
            return *this;
        }
        Coroutine& Wait(float seconds)
        {
            m_steps.push_back({Step::Yield, nullptr, std::make_unique<WaitForSeconds>(seconds)});
            return *this;
        }
        Coroutine& WaitFrames(int n)
        {
            m_steps.push_back({Step::Yield, nullptr, std::make_unique<WaitForFrames>(n)});
            return *this;
        }
        Coroutine& WaitFor(std::function<bool()> pred)
        {
            m_steps.push_back({Step::Yield, nullptr, std::make_unique<WaitUntil>(std::move(pred))});
            return *this;
        }

        void Update(float dt)
        {
            if (m_cancelled || IsFinished())
                return;
            while (m_step < (int)m_steps.size())
            {
                auto& s = m_steps[m_step];
                if (s.type == Step::Action)
                {
                    if (s.action)
                        s.action();
                    m_step++;
                    continue;
                }
                if (s.yield && s.yield->IsReady(dt))
                {
                    m_step++;
                    continue;
                }
                break;
            }
        }

      private:
        struct Step
        {
            enum Type
            {
                Action,
                Yield
            } type;
            std::function<void()> action;
            std::unique_ptr<YieldInstruction> yield;
        };
        std::string m_name;
        std::vector<Step> m_steps;
        int m_step = 0;
        bool m_cancelled = false;
    };

    class Scheduler
    {
      public:
        Coroutine& Start(const std::string& name)
        {
            m_cos.push_back(std::make_unique<Coroutine>(name));
            return *m_cos.back();
        }
        void Stop(const std::string& name)
        {
            for (auto& c : m_cos)
                if (c && c->GetName() == name)
                    c->Cancel();
        }
        void StopAll()
        {
            for (auto& c : m_cos)
                if (c)
                    c->Cancel();
        }
        bool IsRunning(const std::string& name) const
        {
            for (auto& c : m_cos)
                if (c && c->GetName() == name && !c->IsFinished() && !c->IsCancelled())
                    return true;
            return false;
        }
        void Update(float dt)
        {
            for (auto& c : m_cos)
                if (c && !c->IsCancelled() && !c->IsFinished())
                    c->Update(dt);
            m_cos.erase(std::remove_if(m_cos.begin(), m_cos.end(), [](const std::unique_ptr<Coroutine>& c)
                                       { return !c || c->IsCancelled() || c->IsFinished(); }),
                        m_cos.end());
        }
        size_t ActiveCount() const { return m_cos.size(); }

      private:
        std::vector<std::unique_ptr<Coroutine>> m_cos;
    };

} // anonymous namespace

// ============================================================================
// Tests
// ============================================================================

TEST(Coroutine_DoExecutesImmediately)
{
    int counter = 0;
    Coroutine co("test");
    co.Do([&]() { counter++; }).Do([&]() { counter += 10; });
    co.Update(0.016f);
    EXPECT_EQ(counter, 11);
    EXPECT_TRUE(co.IsFinished());
}

TEST(Coroutine_WaitForSecondsDelays)
{
    int step = 0;
    Coroutine co("test");
    co.Do([&]() { step = 1; }).Wait(1.0f).Do([&]() { step = 2; });

    co.Update(0.016f); // executes Do(step=1), starts waiting
    EXPECT_EQ(step, 1);
    EXPECT_FALSE(co.IsFinished());

    co.Update(0.5f); // 0.5s passed — still waiting
    EXPECT_EQ(step, 1);

    co.Update(0.6f); // 1.1s total — wait complete, executes Do(step=2)
    EXPECT_EQ(step, 2);
    EXPECT_TRUE(co.IsFinished());
}

TEST(Coroutine_WaitForFramesDelays)
{
    int step = 0;
    Coroutine co("test");
    co.Do([&]() { step = 1; }).WaitFrames(3).Do([&]() { step = 2; });

    co.Update(0.016f); // executes Do(step=1), enters yield (frame count: 1)
    EXPECT_EQ(step, 1);

    co.Update(0.016f); // frame count: 2
    EXPECT_EQ(step, 1);

    co.Update(0.016f); // frame count: 3 — yield satisfied, executes Do(step=2)
    EXPECT_EQ(step, 2);
    EXPECT_TRUE(co.IsFinished());
}

TEST(Coroutine_WaitUntilPredicate)
{
    bool ready = false;
    int step = 0;
    Coroutine co("test");
    co.Do([&]() { step = 1; }).WaitFor([&]() { return ready; }).Do([&]() { step = 2; });

    co.Update(0.016f);
    EXPECT_EQ(step, 1);

    co.Update(0.016f);
    EXPECT_EQ(step, 1); // still waiting

    ready = true;
    co.Update(0.016f);
    EXPECT_EQ(step, 2);
}

TEST(Coroutine_CancelStopsExecution)
{
    int step = 0;
    Coroutine co("test");
    co.Do([&]() { step = 1; }).Wait(1.0f).Do([&]() { step = 2; });

    co.Update(0.016f);
    EXPECT_EQ(step, 1);

    co.Cancel();
    co.Update(2.0f); // enough time, but cancelled
    EXPECT_EQ(step, 1);
    EXPECT_TRUE(co.IsCancelled());
}

TEST(Scheduler_ManagesMultipleCoroutines)
{
    Scheduler sched;
    int a = 0, b = 0;

    sched.Start("a").Do([&]() { a = 1; }).Wait(1.0f).Do([&]() { a = 2; });
    sched.Start("b").Do([&]() { b = 1; }).Wait(0.5f).Do([&]() { b = 2; });

    EXPECT_EQ(sched.ActiveCount(), (size_t)2);

    sched.Update(0.016f);
    EXPECT_EQ(a, 1);
    EXPECT_EQ(b, 1);

    sched.Update(0.5f);
    EXPECT_EQ(a, 1);                           // still waiting
    EXPECT_EQ(b, 2);                           // done
    EXPECT_EQ(sched.ActiveCount(), (size_t)1); // b cleaned up

    sched.Update(0.6f);
    EXPECT_EQ(a, 2);
    EXPECT_EQ(sched.ActiveCount(), (size_t)0);
}

TEST(Scheduler_StopByName)
{
    Scheduler sched;
    int x = 0;
    sched.Start("task").Do([&]() { x = 1; }).Wait(1.0f).Do([&]() { x = 2; });
    sched.Update(0.016f);
    EXPECT_EQ(x, 1);

    sched.Stop("task");
    sched.Update(2.0f);
    EXPECT_EQ(x, 1); // never reached step 2
}

TEST(Scheduler_StopAll)
{
    Scheduler sched;
    sched.Start("a").Wait(1.0f);
    sched.Start("b").Wait(1.0f);
    EXPECT_EQ(sched.ActiveCount(), (size_t)2);

    sched.StopAll();
    sched.Update(0.016f);
    EXPECT_EQ(sched.ActiveCount(), (size_t)0);
}

TEST(Scheduler_IsRunning)
{
    Scheduler sched;
    sched.Start("task").Wait(1.0f);
    sched.Update(0.016f);
    EXPECT_TRUE(sched.IsRunning("task"));
    EXPECT_FALSE(sched.IsRunning("nonexistent"));
}

TEST(Coroutine_ChainedWaits)
{
    int step = 0;
    Coroutine co("test");
    co.Do([&]() { step = 1; }).Wait(1.0f).Do([&]() { step = 2; }).Wait(1.0f).Do([&]() { step = 3; });

    co.Update(0.016f); // Do(step=1), wait timer starts
    EXPECT_EQ(step, 1);

    co.Update(0.5f); // wait: 0.516s < 1.0s
    EXPECT_EQ(step, 1);

    co.Update(0.5f); // wait: 1.016s >= 1.0s — Do(step=2), second wait starts
    EXPECT_EQ(step, 2);

    co.Update(1.0f); // second wait: 1.0s >= 1.0s — Do(step=3)
    EXPECT_EQ(step, 3);
    EXPECT_TRUE(co.IsFinished());
}
