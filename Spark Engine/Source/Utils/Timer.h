/**
 * @file Timer.h
 * @brief High-precision timing system for frame rate and delta time calculation
 * @author Spark Engine Team
 * @date 2025
 * 
 * This class provides a high-precision timing system using the system's high-resolution
 * clock for accurate frame timing, delta time calculation, and total elapsed time tracking.
 * Essential for frame-rate independent game logic and performance measurement.
 */

#pragma once

#include "../Core/framework.h"
#include "Utils/Assert.h"
#include <chrono>

/**
 * @brief High-precision timer for game engine timing
 * 
 * The Timer class provides accurate timing functionality for game engines using
 * the system's high-resolution clock. It tracks delta time between frames and
 * total elapsed time, supporting pause/resume functionality for game state management.
 * 
 * Features include:
 * - High-precision timing using std::chrono::high_resolution_clock
 * - Delta time calculation for frame-rate independent updates
 * - Total elapsed time tracking
 * - Pause/resume functionality
 * - Automatic time tracking with simple interface
 * 
 * @note Delta time is calculated between consecutive calls to GetDeltaTime()
 * @warning Ensure Start() is called before using timing functions
 */
class Timer
{
private:
    std::chrono::high_resolution_clock::time_point m_lastTime;  ///< Time point of last measurement
    float                                          m_deltaTime; ///< Time elapsed since last frame in seconds
    float                                          m_totalTime; ///< Total elapsed time since start in seconds
    bool                                           m_paused;    ///< Whether the timer is currently paused

public:
    /**
     * @brief Default constructor
     * 
     * Initializes timer with zero values and stopped state.
     * Call Start() to begin timing operations.
     */
    Timer();

    /**
     * @brief Destructor
     * 
     * Automatically stops the timer if it was running.
     */
    ~Timer();

    /**
     * @brief Start or resume the timer
     * 
     * Begins timing operations or resumes from a paused state.
     * Records the current time as the reference point for delta calculations.
     */
    void  Start();

    /**
     * @brief Pause the timer
     * 
     * Stops time accumulation while preserving current state.
     * Can be resumed with Start().
     */
    void  Stop();

    /**
     * @brief Reset timer to initial state
     * 
     * Resets total time to zero and stops the timer.
     * Must call Start() again to resume timing.
     */
    void  Reset();

    /**
     * @brief Get delta time since last call and update internal time
     * 
     * Returns the time elapsed since the last call to this method and
     * updates the internal timing state for the next call.
     * 
     * @return Delta time in seconds since last call
     * @note Returns 0.0 if timer is paused
     */
    float GetDeltaTime();

    /**
     * @brief Get total elapsed time since timer start
     * 
     * Returns the cumulative time that has elapsed since the timer
     * was started, excluding any time spent in paused state.
     * 
     * @return Total elapsed time in seconds
     */
    float GetTotalTime() const { return m_totalTime; }

    /**
     * @brief Check if the timer is currently paused
     * @return true if timer is paused, false if running
     */
    bool IsPaused() const { return m_paused; }

private:
    /**
     * @brief Update internal time tracking
     * 
     * Internal method that calculates delta time and updates total time.
     * Called automatically by GetDeltaTime().
     */
    void UpdateTime();
};