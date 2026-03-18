/**
 * @file CoroutineTypes.h
 * @brief Type definitions for the coroutine system -- yield instructions, promise types, and builder coroutine
 * @author Spark Engine Team
 * @date 2025
 *
 * Contains all coroutine-related types used by CoroutineScheduler:
 * - YieldInstruction hierarchy (WaitForSeconds, WaitForFrames, etc.)
 * - C++20 coroutine types (GameCoroutine, YieldAwaiter, co_await operators)
 * - Builder-pattern Coroutine class
 * - NativeCoroutineWrapper adapter
 *
 * @see CoroutineScheduler.h
 */

#pragma once
#include <functional>
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <coroutine>

namespace Spark
{

    // Forward declaration for WaitForEvent
    class EventBus;

    // ============================================================================
    // Yield Instructions -- tell the scheduler when to resume a coroutine step
    // ============================================================================

    /**
     * @brief Base class for yield instructions that pause coroutine execution.
     */
    class YieldInstruction
    {
      public:
        virtual ~YieldInstruction() = default;

        /**
         * @brief Called each frame to check if the yield condition is satisfied.
         * @param deltaTime  Frame delta time in seconds.
         * @return True when the coroutine should resume.
         */
        virtual bool IsReady(float deltaTime) = 0;

        /** @brief Reset the instruction for reuse (e.g., in repeating coroutines). */
        virtual void Reset() {}
    };

    /**
     * @brief Yield for a specified duration in seconds.
     */
    class WaitForSeconds : public YieldInstruction
    {
      public:
        explicit WaitForSeconds(float seconds) : m_target(seconds) {}

        bool IsReady(float deltaTime) override
        {
            m_elapsed += deltaTime;
            return m_elapsed >= m_target;
        }

        void Reset() override { m_elapsed = 0.0f; }

      private:
        float m_target;
        float m_elapsed = 0.0f;
    };

    /**
     * @brief Yield for a specified number of frames.
     */
    class WaitForFrames : public YieldInstruction
    {
      public:
        explicit WaitForFrames(int frames) : m_target(frames) {}

        bool IsReady(float /*deltaTime*/) override { return ++m_count >= m_target; }

        void Reset() override { m_count = 0; }

      private:
        int m_target;
        int m_count = 0;
    };

    /**
     * @brief Yield until a predicate returns true.
     */
    class WaitUntil : public YieldInstruction
    {
      public:
        explicit WaitUntil(std::function<bool()> predicate) : m_predicate(std::move(predicate)) {}

        bool IsReady(float /*deltaTime*/) override { return m_predicate && m_predicate(); }

      private:
        std::function<bool()> m_predicate;
    };

    /**
     * @brief Yield for one frame (resume next frame).
     */
    class WaitForEndOfFrame : public YieldInstruction
    {
      public:
        bool IsReady(float /*deltaTime*/) override
        {
            if (m_waited)
                return true;
            m_waited = true;
            return false;
        }

        void Reset() override { m_waited = false; }

      private:
        bool m_waited = false;
    };

    /**
     * @brief Yield until a named event is fired on a shared flag.
     *
     * The caller sets a shared atomic bool to true when the event fires.
     * This decouples the coroutine from EventBus include dependencies.
     *
     * Usage with EventBus:
     * @code
     *   auto flag = std::make_shared<std::atomic<bool>>(false);
     *   auto subID = eventBus.Subscribe<MyEvent>([flag](const MyEvent&) {
     *       flag->store(true);
     *   });
     *   // In builder coroutine:
     *   scheduler.StartCoroutine("wait_event")
     *       .WaitForEvent(flag)
     *       .Do([&]() { OnEventReceived(); });
     * @endcode
     */
    class WaitForEvent : public YieldInstruction
    {
      public:
        explicit WaitForEvent(std::shared_ptr<std::atomic<bool>> eventFlag) : m_eventFlag(std::move(eventFlag)) {}

        bool IsReady(float /*deltaTime*/) override
        {
            return m_eventFlag && m_eventFlag->load(std::memory_order_acquire);
        }

        void Reset() override
        {
            if (m_eventFlag)
                m_eventFlag->store(false, std::memory_order_release);
        }

      private:
        std::shared_ptr<std::atomic<bool>> m_eventFlag;
    };

    // ============================================================================
    // C++20 Coroutine Types
    // ============================================================================

    /**
     * @brief Return type for C++20 coroutine functions that yield to the scheduler.
     *
     * A GameCoroutine function can use co_await with any YieldInstruction,
     * co_yield to pause for one frame, and co_return to complete.
     */
    class GameCoroutine
    {
      public:
        struct promise_type;
        using handle_type = std::coroutine_handle<promise_type>;

        /**
         * @brief Promise type that drives the C++20 coroutine machinery.
         */
        struct promise_type
        {
            /// The currently active yield instruction (set by co_await)
            std::unique_ptr<YieldInstruction> currentYield;

            /// Whether the coroutine has finished
            bool finished = false;

            GameCoroutine get_return_object() { return GameCoroutine{handle_type::from_promise(*this)}; }

            std::suspend_always initial_suspend() noexcept { return {}; }
            std::suspend_always final_suspend() noexcept
            {
                finished = true;
                return {};
            }

            void return_void() { finished = true; }
            void unhandled_exception() { finished = true; }

            /// Supports co_yield for yielding one frame
            std::suspend_always yield_value([[maybe_unused]] std::nullptr_t)
            {
                currentYield = std::make_unique<WaitForEndOfFrame>();
                return {};
            }
        };

        GameCoroutine() = default;
        explicit GameCoroutine(handle_type h) : m_handle(h) {}

        // Move-only
        GameCoroutine(const GameCoroutine&) = delete;
        GameCoroutine& operator=(const GameCoroutine&) = delete;
        GameCoroutine(GameCoroutine&& other) noexcept : m_handle(other.m_handle) { other.m_handle = nullptr; }
        GameCoroutine& operator=(GameCoroutine&& other) noexcept
        {
            if (this != &other)
            {
                if (m_handle)
                    m_handle.destroy();
                m_handle = other.m_handle;
                other.m_handle = nullptr;
            }
            return *this;
        }

        ~GameCoroutine()
        {
            if (m_handle)
                m_handle.destroy();
        }

        /**
         * @brief Check if the coroutine has completed execution.
         */
        bool IsFinished() const { return !m_handle || m_handle.done() || m_handle.promise().finished; }

        /**
         * @brief Tick the coroutine forward. Returns true if still running.
         * @param deltaTime  Frame delta time in seconds.
         */
        bool Update(float deltaTime)
        {
            if (IsFinished())
                return false;

            auto& promise = m_handle.promise();

            // If we have an active yield, check if it is ready
            if (promise.currentYield)
            {
                if (!promise.currentYield->IsReady(deltaTime))
                    return true; // Still waiting

                // Yield is satisfied, clear it and resume
                promise.currentYield.reset();
            }

            if (!m_handle.done())
            {
                m_handle.resume();
            }

            return !IsFinished();
        }

        handle_type GetHandle() const { return m_handle; }

      private:
        handle_type m_handle = nullptr;
    };

    /**
     * @brief Awaiter that bridges YieldInstruction to C++20 co_await.
     *
     * Allows: co_await WaitForSeconds(2.0f);
     */
    template <typename T>
        requires std::derived_from<T, YieldInstruction>
    struct YieldAwaiter
    {
        T instruction;

        explicit YieldAwaiter(T inst) : instruction(std::move(inst)) {}

        bool await_ready() const noexcept { return false; }

        void await_suspend(std::coroutine_handle<GameCoroutine::promise_type> handle) const
        {
            handle.promise().currentYield = std::make_unique<T>(std::move(instruction));
        }

        void await_resume() const noexcept {}
    };

    // Enable co_await for all YieldInstruction-derived types
    inline YieldAwaiter<WaitForSeconds> operator co_await(WaitForSeconds w)
    {
        return YieldAwaiter<WaitForSeconds>{std::move(w)};
    }

    inline YieldAwaiter<WaitForFrames> operator co_await(WaitForFrames w)
    {
        return YieldAwaiter<WaitForFrames>{std::move(w)};
    }

    inline YieldAwaiter<WaitUntil> operator co_await(WaitUntil w)
    {
        return YieldAwaiter<WaitUntil>{std::move(w)};
    }

    inline YieldAwaiter<WaitForEndOfFrame> operator co_await(WaitForEndOfFrame w)
    {
        return YieldAwaiter<WaitForEndOfFrame>{std::move(w)};
    }

    inline YieldAwaiter<WaitForEvent> operator co_await(WaitForEvent w)
    {
        return YieldAwaiter<WaitForEvent>{std::move(w)};
    }

    // ============================================================================
    // Coroutine -- a sequence of action/yield steps (builder pattern)
    // ============================================================================

    /**
     * @brief A single coroutine: a named sequence of action steps separated by yields.
     */
    class Coroutine
    {
      public:
        explicit Coroutine(const std::string& name) : m_name(name) {}

        /** @brief Get the coroutine's name (for cancellation and debugging). */
        const std::string& GetName() const { return m_name; }

        /** @brief Check if all steps have been executed. */
        bool IsFinished() const { return m_currentStep >= static_cast<int>(m_steps.size()); }

        /** @brief Mark this coroutine for removal. */
        void Cancel() { m_cancelled = true; }

        /** @brief Check if the coroutine has been cancelled. */
        bool IsCancelled() const { return m_cancelled; }

        // ---- Builder API -------------------------------------------------------

        /**
         * @brief Add an action step (runs immediately when reached).
         */
        Coroutine& Do(std::function<void()> action)
        {
            m_steps.push_back({Step::Type::Action, std::move(action), nullptr});
            return *this;
        }

        /**
         * @brief Add a wait-for-seconds yield step.
         */
        Coroutine& WaitForSeconds(float seconds)
        {
            m_steps.push_back({Step::Type::Yield, nullptr, std::make_unique<Spark::WaitForSeconds>(seconds)});
            return *this;
        }

        /**
         * @brief Add a wait-for-frames yield step.
         */
        Coroutine& WaitForFrames(int frames)
        {
            m_steps.push_back({Step::Type::Yield, nullptr, std::make_unique<Spark::WaitForFrames>(frames)});
            return *this;
        }

        /**
         * @brief Add a wait-until-predicate yield step.
         */
        Coroutine& WaitUntil(std::function<bool()> predicate)
        {
            m_steps.push_back({Step::Type::Yield, nullptr, std::make_unique<Spark::WaitUntil>(std::move(predicate))});
            return *this;
        }

        /**
         * @brief Add a wait-for-event yield step using a shared flag.
         */
        Coroutine& WaitForEvent(std::shared_ptr<std::atomic<bool>> eventFlag)
        {
            m_steps.push_back(
                {Step::Type::Yield, nullptr, std::make_unique<Spark::WaitForEvent>(std::move(eventFlag))});
            return *this;
        }

        /**
         * @brief Add a yield-one-frame step.
         */
        Coroutine& YieldFrame()
        {
            m_steps.push_back({Step::Type::Yield, nullptr, std::make_unique<WaitForEndOfFrame>()});
            return *this;
        }

        /**
         * @brief Tick the coroutine forward by one frame.
         *
         * Executes action steps immediately and checks yield conditions.
         * @param deltaTime  Frame delta time in seconds.
         */
        void Update(float deltaTime)
        {
            if (m_cancelled || IsFinished())
                return;

            while (m_currentStep < static_cast<int>(m_steps.size()))
            {
                auto& step = m_steps[m_currentStep];

                if (step.type == Step::Type::Action)
                {
                    if (step.action)
                        step.action();
                    m_currentStep++;
                    continue;
                }

                // Yield step -- check if ready
                if (step.yield && step.yield->IsReady(deltaTime))
                {
                    m_currentStep++;
                    continue;
                }

                // Not ready -- stop processing until next frame
                break;
            }
        }

      private:
        struct Step
        {
            enum class Type
            {
                Action,
                Yield
            };
            Type type;
            std::function<void()> action;
            std::unique_ptr<YieldInstruction> yield;
        };

        std::string m_name;
        std::vector<Step> m_steps;
        int m_currentStep = 0;
        bool m_cancelled = false;
    };

    // ============================================================================
    // NativeCoroutineWrapper -- adapts a GameCoroutine for the scheduler
    // ============================================================================

    /**
     * @brief Wraps a C++20 GameCoroutine so the scheduler can manage it
     *        alongside builder-pattern coroutines.
     */
    class NativeCoroutineWrapper
    {
      public:
        NativeCoroutineWrapper(const std::string& name, GameCoroutine coroutine)
            : m_name(name), m_coroutine(std::move(coroutine))
        {
        }

        const std::string& GetName() const { return m_name; }
        bool IsFinished() const { return m_coroutine.IsFinished(); }
        bool IsCancelled() const { return m_cancelled; }

        void Cancel() { m_cancelled = true; }

        void Update(float deltaTime)
        {
            if (m_cancelled || m_coroutine.IsFinished())
                return;
            m_coroutine.Update(deltaTime);
        }

      private:
        std::string m_name;
        GameCoroutine m_coroutine;
        bool m_cancelled = false;
    };

} // namespace Spark
