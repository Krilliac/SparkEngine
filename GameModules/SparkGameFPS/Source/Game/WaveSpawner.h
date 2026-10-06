/**
 * @file WaveSpawner.h
 * @brief Wave-based enemy spawning system with escalating difficulty
 *
 * Manages enemy waves with configurable composition, difficulty scaling,
 * rest periods between waves, and boss wave support.
 */

#pragma once
#include "Core/Platform.h"
#include "Enemy.h"
#include "WaveComposition.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif

#include <optional>
#include <vector>
#include <string>
#include <functional>

class Game;

namespace Spark
{

    /**
     * @brief Current state of the wave spawner
     */
    enum class WaveState
    {
        Idle,       ///< Not started
        Countdown,  ///< Rest period before next wave
        Spawning,   ///< Actively spawning enemies
        InProgress, ///< All enemies spawned, waiting for kills
        Completed,  ///< All waves cleared
        Failed      ///< Player died
    };

    /**
     * @brief Callbacks for wave lifecycle events
     */
    struct WaveCallbacks
    {
        std::function<void(int waveNum, const std::string& announcement)> onWaveStart;
        std::function<void(int waveNum, int enemiesKilled)> onWaveComplete;
        std::function<void(int totalWaves)> onAllWavesComplete;
        std::function<void(int waveNum)> onWaveFailed;
        std::function<void(float timeRemaining)> onCountdownTick;
    };

    /**
     * @brief Wave-based enemy spawning system
     *
     * Generates escalating waves of enemies with difficulty scaling.
     * Integrates with the Game's SpawnEnemy() for actual enemy creation.
     */
    class WaveSpawner
    {
      public:
        WaveSpawner();
        ~WaveSpawner() = default;

        /**
         * @brief Initialize with spawn point locations
         * @param spawnPoints Positions where enemies can appear
         */
        void Initialize(const std::vector<DirectX::XMFLOAT3>& spawnPoints);

        /**
         * @brief Update the wave spawner each frame
         * @param dt Delta time
         * @param aliveEnemies Current number of alive enemies
         * @param game Game instance for spawning enemies
         */
        void Update(float dt, size_t aliveEnemies, Game* game);

        /**
         * @brief Start wave progression from wave 1
         */
        void Start();

        /**
         * @brief Pause wave progression
         */
        void Pause() { m_paused = true; }

        /**
         * @brief Resume wave progression
         */
        void Resume() { m_paused = false; }

        /**
         * @brief Skip to a specific wave number
         * @param waveNum Requested wave; clamped to [1, GetTotalWaves()] before use.
         * @return The wave that will start when the countdown expires.
         */
        int SkipToWave(int waveNum);

        /**
         * @brief Reset all state back to idle
         */
        void Reset();

        // === Accessors ===

        WaveState GetState() const { return m_state; }
        int GetCurrentWave() const { return m_currentWave; }
        int GetTotalWaves() const { return m_totalWaves; }
        int GetEnemiesKilledThisWave() const { return m_enemiesKilledThisWave; }
        int GetTotalEnemiesKilled() const { return m_totalEnemiesKilled; }
        float GetCountdownRemaining() const { return m_countdownTimer; }
        bool IsBossWave() const;

        // === Configuration ===

        /// Clamped to [1, WaveComposition::MAX_WAVE_NUMBER].
        void SetTotalWaves(int waves) { m_totalWaves = WaveComposition::ClampWaveNumber(waves); }
        void SetRestDuration(float seconds) { m_restDuration = seconds; }
        /// Non-finite values fall back to 1.0; finite ones are clamped to the documented 1.0-3.0 range.
        void SetDifficultyScale(float scale) { m_difficultyScale = WaveComposition::SanitizeDifficultyScale(scale); }
        WaveCallbacks& GetCallbacks() { return m_callbacks; }

        // Console integration
        std::string Console_GetStatus() const;

      private:
        WaveDefinition GenerateWave(int waveNumber) const;
        void SpawnWave(const WaveDefinition& wave, Game* game);
        DirectX::XMFLOAT3 GetRandomSpawnPoint() const;

        WaveState m_state{WaveState::Idle};
        int m_currentWave{0};
        int m_totalWaves{20};
        float m_restDuration{8.0f};
        float m_countdownTimer{0.0f};
        float m_difficultyScale{1.0f};
        bool m_paused{false};
        bool m_waveTransitionReady{false}; ///< Set when the rest countdown expires

        int m_enemiesSpawnedThisWave{0};
        int m_enemiesKilledThisWave{0};
        int m_totalEnemiesKilled{0};
        int m_enemiesAliveAtWaveStart{0};

        std::vector<DirectX::XMFLOAT3> m_spawnPoints;
        WaveCallbacks m_callbacks;
        std::optional<float> m_waveRestRemaining; ///< Seconds until the next wave; empty when not armed
    };

} // namespace Spark
