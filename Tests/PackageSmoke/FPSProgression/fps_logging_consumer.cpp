#include "Core/FPSLog.h"

#include <chrono>

int main()
{
    using Clock = Spark::FPSLog::RateLimiter::Clock;
    using Seconds = std::chrono::seconds;
    const auto start = Clock::time_point{};
    Spark::FPSLog::RateLimiter limiter;
    if (!limiter.Allow(2, Seconds(5), start) || !limiter.Allow(2, Seconds(5), start + Seconds(1)) ||
        limiter.Allow(2, Seconds(5), start + Seconds(2)) || !limiter.Allow(2, Seconds(5), start + Seconds(5)) ||
        !limiter.Allow(2, Seconds(5), start + Seconds(6)) || limiter.Allow(2, Seconds(5), start + Seconds(7)) ||
        limiter.Allow(0, Seconds(5), start + Seconds(8)))
    {
        return 1;
    }

    int evaluated = 0;
    Spark::ModuleLog::Bind(nullptr);
    const auto emit = [&]() { FPS_CONSOLE_RATE_LIMITED(1, 60, (++evaluated, "deferred"), "INFO"); };
    emit();
    emit();
    return evaluated == 1 ? 0 : 2;
}
