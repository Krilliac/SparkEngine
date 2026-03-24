/**
 * @file WaveSpawner.cpp
 * @brief Wave-based enemy spawning with escalating difficulty
 */

#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include "WaveSpawner.h"
#include "Game.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <sstream>

using namespace DirectX;

namespace Spark
{

    WaveSpawner::WaveSpawner() = default;

    void WaveSpawner::Initialize(const std::vector<XMFLOAT3>& spawnPoints)
    {
        m_spawnPoints = spawnPoints;
        if (m_spawnPoints.empty())
        {
            // Fallback spawn points around the arena perimeter
            m_spawnPoints = {
                {20.0f, 1.0f, 20.0f}, {-20.0f, 1.0f, 20.0f}, {20.0f, 1.0f, -20.0f}, {-20.0f, 1.0f, -20.0f},
                {25.0f, 1.0f, 0.0f},  {-25.0f, 1.0f, 0.0f},  {0.0f, 1.0f, 25.0f},   {0.0f, 1.0f, -25.0f},
            };
        }
        m_state = WaveState::Idle;
    }

    void WaveSpawner::Start()
    {
        m_currentWave = 0;
        m_totalEnemiesKilled = 0;
        m_state = WaveState::Countdown;
        m_countdownTimer = 3.0f; // Short initial countdown
        m_waveTransitionReady = false;
        m_waveScheduler.ClearAll();
        m_waveScheduler.Schedule([this] { m_waveTransitionReady = true; }, 3.0f);
    }

    void WaveSpawner::Update(float dt, size_t aliveEnemies, Game* game)
    {
        if (m_paused || m_state == WaveState::Idle || m_state == WaveState::Completed || m_state == WaveState::Failed)
            return;

        m_waveScheduler.Update(dt);

        switch (m_state)
        {
        case WaveState::Countdown:
        {
            // Update display timer for countdown tick callbacks
            m_countdownTimer = std::max(m_countdownTimer - dt, 0.0f);
            if (m_callbacks.onCountdownTick)
                m_callbacks.onCountdownTick(m_countdownTimer);

            // Scheduler fires m_waveTransitionReady when countdown expires
            if (m_waveTransitionReady)
            {
                m_waveTransitionReady = false;
                m_currentWave++;
                WaveDefinition wave = GenerateWave(m_currentWave);

                if (m_callbacks.onWaveStart)
                    m_callbacks.onWaveStart(m_currentWave, wave.announcement);

                m_enemiesAliveAtWaveStart = static_cast<int>(aliveEnemies);
                m_enemiesSpawnedThisWave = 0;
                m_enemiesKilledThisWave = 0;

                SpawnWave(wave, game);
                m_state = WaveState::InProgress;
            }
            break;
        }

        case WaveState::InProgress:
        {
            // Track kills: enemies that died since wave started
            int currentAlive = static_cast<int>(aliveEnemies);
            int totalSpawnedAndExisting = m_enemiesSpawnedThisWave + m_enemiesAliveAtWaveStart;
            m_enemiesKilledThisWave = totalSpawnedAndExisting - currentAlive;

            // Wave complete when all spawned enemies are dead
            if (aliveEnemies == 0)
            {
                m_totalEnemiesKilled += m_enemiesKilledThisWave;

                if (m_callbacks.onWaveComplete)
                    m_callbacks.onWaveComplete(m_currentWave, m_enemiesKilledThisWave);

                if (m_currentWave >= m_totalWaves)
                {
                    m_state = WaveState::Completed;
                    if (m_callbacks.onAllWavesComplete)
                        m_callbacks.onAllWavesComplete(m_totalWaves);
                }
                else
                {
                    m_state = WaveState::Countdown;
                    m_countdownTimer = m_restDuration;
                    m_waveTransitionReady = false;
                    m_waveScheduler.ClearAll();
                    m_waveScheduler.Schedule([this] { m_waveTransitionReady = true; }, m_restDuration);
                }
            }
            break;
        }

        default:
            break;
        }
    }

    void WaveSpawner::SkipToWave(int waveNum)
    {
        m_currentWave = std::max(0, waveNum - 1);
        m_state = WaveState::Countdown;
        m_countdownTimer = 3.0f;
        m_waveTransitionReady = false;
        m_waveScheduler.ClearAll();
        m_waveScheduler.Schedule([this] { m_waveTransitionReady = true; }, 3.0f);
    }

    void WaveSpawner::Reset()
    {
        m_state = WaveState::Idle;
        m_currentWave = 0;
        m_totalEnemiesKilled = 0;
        m_enemiesKilledThisWave = 0;
        m_enemiesSpawnedThisWave = 0;
        m_countdownTimer = 0.0f;
        m_paused = false;
    }

    bool WaveSpawner::IsBossWave() const
    {
        return (m_currentWave % 5 == 0) && m_currentWave > 0;
    }

    WaveDefinition WaveSpawner::GenerateWave(int waveNumber) const
    {
        WaveDefinition wave;
        wave.waveNumber = waveNumber;

        float difficulty = 1.0f + (waveNumber - 1) * 0.15f * m_difficultyScale;
        wave.healthMultiplier = 1.0f + (waveNumber - 1) * 0.1f * m_difficultyScale;
        wave.damageMultiplier = 1.0f + (waveNumber - 1) * 0.08f * m_difficultyScale;
        wave.speedMultiplier = 1.0f + (waveNumber - 1) * 0.03f * m_difficultyScale;

        bool isBoss = (waveNumber % 5 == 0);
        wave.isBossWave = isBoss;

        if (isBoss)
        {
            // Boss waves: fewer but tougher enemies + a heavy
            wave.gruntCount = waveNumber / 2;
            wave.guardCount = waveNumber / 3;
            wave.heavyCount = 1 + waveNumber / 5;
            wave.healthMultiplier *= 1.5f;
            wave.announcement = "BOSS WAVE " + std::to_string(waveNumber) + "!";
        }
        else
        {
            // Normal waves: scaling composition
            wave.gruntCount = 2 + waveNumber;
            wave.scoutCount = (waveNumber >= 3) ? (waveNumber / 2) : 0;
            wave.guardCount = (waveNumber >= 5) ? (waveNumber / 3) : 0;
            wave.sniperCount = (waveNumber >= 7) ? (waveNumber / 4) : 0;
            wave.medicCount = (waveNumber >= 8) ? (waveNumber / 5) : 0;
            wave.heavyCount = (waveNumber >= 10) ? (waveNumber / 6) : 0;
            wave.announcement = "Wave " + std::to_string(waveNumber);
        }

        // Cap total enemies per wave to prevent overwhelming spawns
        int total =
            wave.gruntCount + wave.scoutCount + wave.guardCount + wave.heavyCount + wave.sniperCount + wave.medicCount;
        constexpr int MAX_PER_WAVE = 30;
        if (total > MAX_PER_WAVE)
        {
            float scale = static_cast<float>(MAX_PER_WAVE) / total;
            wave.gruntCount = static_cast<int>(wave.gruntCount * scale);
            wave.scoutCount = static_cast<int>(wave.scoutCount * scale);
            wave.guardCount = static_cast<int>(wave.guardCount * scale);
            wave.heavyCount = std::max(wave.heavyCount, 1); // Always keep at least 1 heavy on boss
            wave.sniperCount = static_cast<int>(wave.sniperCount * scale);
            wave.medicCount = static_cast<int>(wave.medicCount * scale);
        }

        return wave;
    }

    void WaveSpawner::SpawnWave(const WaveDefinition& wave, Game* game)
    {
        if (!game)
            return;

        auto spawnGroup = [&](EnemyType type, int count)
        {
            for (int i = 0; i < count; ++i)
            {
                XMFLOAT3 pos = GetRandomSpawnPoint();
                auto* enemy = game->SpawnEnemy(type, pos.x, pos.y, pos.z);
                if (enemy)
                    m_enemiesSpawnedThisWave++;
            }
        };

        spawnGroup(EnemyType::Grunt, wave.gruntCount);
        spawnGroup(EnemyType::Scout, wave.scoutCount);
        spawnGroup(EnemyType::Guard, wave.guardCount);
        spawnGroup(EnemyType::Heavy, wave.heavyCount);
        spawnGroup(EnemyType::Sniper, wave.sniperCount);
        spawnGroup(EnemyType::Medic, wave.medicCount);

        std::wstring msg = L"Wave " + std::to_wstring(wave.waveNumber) + L": spawned " +
                           std::to_wstring(m_enemiesSpawnedThisWave) + L" enemies";
        LOG_TO_CONSOLE_IMMEDIATE(msg, L"INFO");
    }

    XMFLOAT3 WaveSpawner::GetRandomSpawnPoint() const
    {
        if (m_spawnPoints.empty())
            return {0.0f, 1.0f, 0.0f};

        static std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<size_t> dist(0, m_spawnPoints.size() - 1);

        XMFLOAT3 base = m_spawnPoints[dist(rng)];

        // Add slight random offset to avoid stacking
        std::uniform_real_distribution<float> offset(-3.0f, 3.0f);
        base.x += offset(rng);
        base.z += offset(rng);

        return base;
    }

    std::string WaveSpawner::Console_GetStatus() const
    {
        std::stringstream ss;
        ss << "=== Wave Spawner ===\n";

        const char* stateNames[] = {"Idle", "Countdown", "Spawning", "InProgress", "Completed", "Failed"};
        ss << "State: " << stateNames[static_cast<int>(m_state)] << "\n";
        ss << "Wave: " << m_currentWave << "/" << m_totalWaves << "\n";
        ss << "Enemies This Wave: spawned=" << m_enemiesSpawnedThisWave << " killed=" << m_enemiesKilledThisWave
           << "\n";
        ss << "Total Kills: " << m_totalEnemiesKilled << "\n";
        ss << "Difficulty Scale: " << m_difficultyScale << "\n";

        if (m_state == WaveState::Countdown)
            ss << "Next wave in: " << m_countdownTimer << "s\n";
        if (IsBossWave())
            ss << "*** BOSS WAVE ***\n";

        return ss.str();
    }

} // namespace Spark
