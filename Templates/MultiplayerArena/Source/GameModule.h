/**
 * @file GameModule.h
 * @brief MultiplayerArena -- bounded local arena game module
 *
 * The deterministic lobby and match rules are transport-neutral. When loaded
 * by SparkEngine, the module supplies a two-player keyboard match around them.
 */

#pragma once

#include <Spark/SparkSDK.h>

#include "Game/TemplateRuntime.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

enum class ArenaTeam : uint8_t
{
    Unassigned = 0,
    Cyan = 1,
    Magenta = 2
};

[[nodiscard]] constexpr bool IsPlayableArenaTeam(uint8_t team)
{
    return team == static_cast<uint8_t>(ArenaTeam::Cyan) || team == static_cast<uint8_t>(ArenaTeam::Magenta);
}

struct NetPlayer
{
    uint32_t playerId = 0;
    std::string name;
    uint8_t team = static_cast<uint8_t>(ArenaTeam::Unassigned); ///< Stable wire IDs: 1 = cyan, 2 = magenta
    float health = 100.0f;
    uint32_t kills = 0;
    uint32_t deaths = 0;
    uint32_t assists = 0;
    uint32_t score = 0;
    bool isReady = false;
    bool isAlive = true;
    float respawnTimer = 0.0f;
};

enum class MatchPhase : uint8_t
{
    Lobby = 0,
    Countdown = 1,
    InProgress = 2,
    Overtime = 3,
    PostMatch = 4
};

struct MatchState
{
    MatchPhase phase = MatchPhase::Lobby;
    float matchTimer = 0.0f;
    float matchDuration = 300.0f;
    float countdownTimer = 5.0f;
    uint32_t teamCyanScore = 0;
    uint32_t teamMagentaScore = 0;
    uint32_t scoreLimit = 50;
    uint32_t minPlayers = 2;
};

class MultiplayerArenaModule final : public Spark::IModule
{
  public:
    Spark::ModuleInfo GetModuleInfo() const override
    {
        Spark::ModuleInfo info{};
        info.name = "MultiplayerArena";
        info.version = "0.2.0";
        info.sdkVersion = SPARK_SDK_VERSION;
        info.loadOrder = 1000;
        return info;
    }

    bool OnLoad(Spark::IEngineContext* context) override
    {
        m_context = context;
        m_match = MatchState{};
        m_players.clear();
        ResetRuntimeHandles();
        ResetInputEdges();

        if (!m_runtime.Load(context, "MultiplayerArena", {"Startup.sparkscene", "Scenes/Arena.sparkscene"},
                            [this](const Spark::Templates::TemplateRuntimeScene& scene)
                            {
                                m_cameraEntity = scene.Find("Main Camera");
                                m_cyanEntity = scene.Find("Cyan Player");
                                m_magentaEntity = scene.Find("Magenta Player");
                                m_shieldEntity = scene.Find("Shield Pickup");
                                return scene.Get<Transform>(m_cameraEntity) && scene.Get<Camera>(m_cameraEntity) &&
                                       scene.Get<Transform>(m_cyanEntity) && scene.Get<MeshRenderer>(m_cyanEntity) &&
                                       scene.Get<Transform>(m_magentaEntity) &&
                                       scene.Get<MeshRenderer>(m_magentaEntity) &&
                                       scene.Get<Transform>(m_shieldEntity) && scene.Get<MeshRenderer>(m_shieldEntity);
                            }))
            return false;

        if (m_runtime.IsActive())
        {
            RememberSpawnPoints();
            ConfigurePlayableMatch();
            if (m_runtime.GetGraphics())
            {
                m_cyanHudEntity = m_runtime.CreateSprite("Arena Cyan HUD", "Assets/multiplayer_arena_runtime_sheet.png",
                                                         Spark::Templates::TemplateRuntimeScene::SheetCell(0, 0));
                m_magentaHudEntity =
                    m_runtime.CreateSprite("Arena Magenta HUD", "Assets/multiplayer_arena_runtime_sheet.png",
                                           Spark::Templates::TemplateRuntimeScene::SheetCell(1, 0));
                m_phaseHudEntity =
                    m_runtime.CreateSprite("Arena Phase HUD", "Assets/multiplayer_arena_runtime_sheet.png",
                                           Spark::Templates::TemplateRuntimeScene::SheetCell(2, 1));
            }
        }

        SyncRuntimeState();
        return true;
    }

    void OnUnload() override
    {
        m_runtime.Unload();
        ResetRuntimeHandles();
        m_context = nullptr;
    }

    bool SupportsHotReload() const override { return false; }

    void OnUpdate(float deltaTime) override
    {
        if (!std::isfinite(deltaTime) || deltaTime <= 0.0f)
            return;

        switch (m_match.phase)
        {
        case MatchPhase::Lobby:
            UpdateLobby();
            break;
        case MatchPhase::Countdown:
            m_match.countdownTimer = std::max(0.0f, m_match.countdownTimer - deltaTime);
            if (m_match.countdownTimer == 0.0f)
                m_match.phase = MatchPhase::InProgress;
            break;
        case MatchPhase::InProgress:
        case MatchPhase::Overtime:
            UpdateMatch(deltaTime);
            break;
        case MatchPhase::PostMatch:
            break;
        }

        UpdateRuntimeInput(deltaTime);
        SyncRuntimeState();
    }

    void OnRender() override { m_runtime.Render(m_cameraEntity); }
    void OnResize(int width, int height) override { m_runtime.Resize(width, height); }

    bool AddPlayer(uint32_t playerId, const std::string& name, uint8_t team)
    {
        if (m_match.phase != MatchPhase::Lobby || name.empty() || !IsPlayableArenaTeam(team) ||
            std::ranges::any_of(m_players, [playerId](const NetPlayer& player) { return player.playerId == playerId; }))
            return false;
        NetPlayer player;
        player.playerId = playerId;
        player.name = name;
        player.team = team;
        m_players.push_back(std::move(player));
        return true;
    }

    bool SetReady(uint32_t playerId, bool ready)
    {
        if (m_match.phase != MatchPhase::Lobby)
            return false;
        const auto player = FindPlayer(playerId);
        if (player == m_players.end())
            return false;
        player->isReady = ready;
        return true;
    }

    bool RecordElimination(uint32_t killerId, uint32_t victimId)
    {
        if ((m_match.phase != MatchPhase::InProgress && m_match.phase != MatchPhase::Overtime) || killerId == victimId)
            return false;
        const auto killer = FindPlayer(killerId);
        const auto victim = FindPlayer(victimId);
        if (killer == m_players.end() || victim == m_players.end() || !killer->isAlive || !victim->isAlive ||
            killer->team == victim->team)
            return false;

        ++killer->kills;
        killer->score += 10;
        ++victim->deaths;
        victim->health = 0.0f;
        victim->isAlive = false;
        victim->respawnTimer = 3.0f;
        uint32_t& teamScore =
            killer->team == static_cast<uint8_t>(ArenaTeam::Cyan) ? m_match.teamCyanScore : m_match.teamMagentaScore;
        ++teamScore;
        if (teamScore >= m_match.scoreLimit ||
            (m_match.phase == MatchPhase::Overtime && m_match.teamCyanScore != m_match.teamMagentaScore))
            m_match.phase = MatchPhase::PostMatch;
        return true;
    }

    void RestartMatch()
    {
        m_match = MatchState{};
        if (m_runtime.IsActive())
        {
            m_match.matchDuration = RuntimeMatchDuration;
            m_match.scoreLimit = RuntimeScoreLimit;
        }
        for (auto& player : m_players)
        {
            const uint32_t id = player.playerId;
            const std::string name = player.name;
            const uint8_t team = player.team;
            player = {};
            player.playerId = id;
            player.name = name;
            player.team = team;
        }
        ResetPlayerTransform(static_cast<uint8_t>(ArenaTeam::Cyan));
        ResetPlayerTransform(static_cast<uint8_t>(ArenaTeam::Magenta));
    }

    [[nodiscard]] const MatchState& GetMatchState() const { return m_match; }
    [[nodiscard]] const std::vector<NetPlayer>& GetPlayers() const { return m_players; }

  private:
    static constexpr uint32_t CyanPlayerId = 1;
    static constexpr uint32_t MagentaPlayerId = 2;
    static constexpr uint32_t RuntimeScoreLimit = 3;
    static constexpr float RuntimeMatchDuration = 90.0f;
    static constexpr float PlayerMoveSpeed = 6.0f;
    static constexpr float ArenaLimit = 9.0f;
    static constexpr float AttackRangeSquared = 9.0f;

    std::vector<NetPlayer>::iterator FindPlayer(uint32_t playerId)
    {
        return std::find_if(m_players.begin(), m_players.end(),
                            [playerId](const NetPlayer& player) { return player.playerId == playerId; });
    }

    std::vector<NetPlayer>::const_iterator FindPlayer(uint32_t playerId) const
    {
        return std::find_if(m_players.cbegin(), m_players.cend(),
                            [playerId](const NetPlayer& player) { return player.playerId == playerId; });
    }

    void ResetRuntimeHandles()
    {
        m_cameraEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
        m_cyanEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
        m_magentaEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
        m_shieldEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
        m_cyanHudEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
        m_magentaHudEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
        m_phaseHudEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    }

    void ResetInputEdges()
    {
        m_cyanAttackHeld = false;
        m_magentaAttackHeld = false;
        m_resetHeld = false;
    }

    void RememberSpawnPoints()
    {
        if (const Transform* cyan = m_runtime.Get<Transform>(m_cyanEntity))
            m_cyanSpawn = cyan->position;
        if (const Transform* magenta = m_runtime.Get<Transform>(m_magentaEntity))
            m_magentaSpawn = magenta->position;
    }

    void ConfigurePlayableMatch()
    {
        m_match.matchDuration = RuntimeMatchDuration;
        m_match.scoreLimit = RuntimeScoreLimit;
        AddPlayer(CyanPlayerId, "Cyan", static_cast<uint8_t>(ArenaTeam::Cyan));
        AddPlayer(MagentaPlayerId, "Magenta", static_cast<uint8_t>(ArenaTeam::Magenta));
        SetReady(CyanPlayerId, true);
        SetReady(MagentaPlayerId, true);
        ResetPlayerTransform(static_cast<uint8_t>(ArenaTeam::Cyan));
        ResetPlayerTransform(static_cast<uint8_t>(ArenaTeam::Magenta));
    }

    void RestartPlayableMatch()
    {
        RestartMatch();
        SetReady(CyanPlayerId, true);
        SetReady(MagentaPlayerId, true);
    }

    void UpdateLobby()
    {
        uint32_t readyCount = 0;
        bool cyanReady = false;
        bool magentaReady = false;
        for (const auto& player : m_players)
        {
            if (!player.isReady)
                continue;
            ++readyCount;
            cyanReady = cyanReady || player.team == static_cast<uint8_t>(ArenaTeam::Cyan);
            magentaReady = magentaReady || player.team == static_cast<uint8_t>(ArenaTeam::Magenta);
        }
        if (readyCount >= m_match.minPlayers && readyCount == m_players.size() && cyanReady && magentaReady)
        {
            m_match.phase = MatchPhase::Countdown;
            m_match.countdownTimer = 5.0f;
        }
    }

    void UpdateMatch(float deltaTime)
    {
        m_match.matchTimer = std::min(m_match.matchDuration, m_match.matchTimer + deltaTime);

        for (auto& player : m_players)
        {
            if (!player.isAlive)
            {
                player.respawnTimer = std::max(0.0f, player.respawnTimer - deltaTime);
                if (player.respawnTimer == 0.0f)
                {
                    player.isAlive = true;
                    player.health = 100.0f;
                    ResetPlayerTransform(player.team);
                }
            }
        }

        if (m_match.teamCyanScore >= m_match.scoreLimit || m_match.teamMagentaScore >= m_match.scoreLimit)
        {
            m_match.phase = MatchPhase::PostMatch;
            return;
        }

        if (m_match.phase == MatchPhase::InProgress && m_match.matchTimer >= m_match.matchDuration)
        {
            m_match.phase =
                m_match.teamCyanScore == m_match.teamMagentaScore ? MatchPhase::Overtime : MatchPhase::PostMatch;
        }
    }

    [[nodiscard]] uint32_t EntityForTeam(uint8_t team) const
    {
        return team == static_cast<uint8_t>(ArenaTeam::Cyan) ? m_cyanEntity : m_magentaEntity;
    }

    void ResetPlayerTransform(uint8_t team)
    {
        if (!IsPlayableArenaTeam(team))
            return;
        const uint32_t entity = EntityForTeam(team);
        if (Transform* transform = m_runtime.Get<Transform>(entity))
        {
            transform->position = team == static_cast<uint8_t>(ArenaTeam::Cyan) ? m_cyanSpawn : m_magentaSpawn;
            if (MeshRenderer* mesh = m_runtime.Get<MeshRenderer>(entity))
                mesh->worldMatrixDirty = true;
        }
    }

    void MovePlayer(uint32_t playerId, float x, float z, float deltaTime)
    {
        const auto player = FindPlayer(playerId);
        if (player == m_players.end() || !player->isAlive ||
            (m_match.phase != MatchPhase::InProgress && m_match.phase != MatchPhase::Overtime))
            return;
        const float length = std::sqrt(x * x + z * z);
        if (length > 1.0f)
        {
            x /= length;
            z /= length;
        }
        const uint32_t entity = EntityForTeam(player->team);
        if (Transform* transform = m_runtime.Get<Transform>(entity))
        {
            transform->position.x =
                std::clamp(transform->position.x + x * PlayerMoveSpeed * deltaTime, -ArenaLimit, ArenaLimit);
            transform->position.z =
                std::clamp(transform->position.z + z * PlayerMoveSpeed * deltaTime, -ArenaLimit, ArenaLimit);
            if (MeshRenderer* mesh = m_runtime.Get<MeshRenderer>(entity))
                mesh->worldMatrixDirty = true;
        }
    }

    [[nodiscard]] bool PlayersAreInAttackRange() const
    {
        const Transform* cyan = m_runtime.Get<Transform>(m_cyanEntity);
        const Transform* magenta = m_runtime.Get<Transform>(m_magentaEntity);
        if (!cyan || !magenta)
            return false;
        const float x = cyan->position.x - magenta->position.x;
        const float z = cyan->position.z - magenta->position.z;
        return x * x + z * z <= AttackRangeSquared;
    }

    void UpdateRuntimeInput(float deltaTime)
    {
        InputManager* input = m_runtime.GetInput();
        if (!input)
            return;

        const bool resetDown = input->IsKeyDown('R');
        if (resetDown && !m_resetHeld)
        {
            RestartPlayableMatch();
            ResetInputEdges();
            m_resetHeld = true;
            return;
        }
        m_resetHeld = resetDown;

        float cyanX = 0.0f;
        float cyanZ = 0.0f;
        if (input->IsKeyDown('W'))
            cyanZ += 1.0f;
        if (input->IsKeyDown('S'))
            cyanZ -= 1.0f;
        if (input->IsKeyDown('D'))
            cyanX += 1.0f;
        if (input->IsKeyDown('A'))
            cyanX -= 1.0f;
        MovePlayer(CyanPlayerId, cyanX, cyanZ, deltaTime);

        float magentaX = 0.0f;
        float magentaZ = 0.0f;
        if (input->IsKeyDown('I'))
            magentaZ += 1.0f;
        if (input->IsKeyDown('K'))
            magentaZ -= 1.0f;
        if (input->IsKeyDown('L'))
            magentaX += 1.0f;
        if (input->IsKeyDown('J'))
            magentaX -= 1.0f;
        MovePlayer(MagentaPlayerId, magentaX, magentaZ, deltaTime);

        const bool cyanAttackDown = input->IsKeyDown('F');
        if (cyanAttackDown && !m_cyanAttackHeld && PlayersAreInAttackRange())
            RecordElimination(CyanPlayerId, MagentaPlayerId);
        m_cyanAttackHeld = cyanAttackDown;

        const bool magentaAttackDown = input->IsKeyDown('O');
        if (magentaAttackDown && !m_magentaAttackHeld && PlayersAreInAttackRange())
            RecordElimination(MagentaPlayerId, CyanPlayerId);
        m_magentaAttackHeld = magentaAttackDown;
    }

    void SyncRuntimeState()
    {
        const auto cyan = FindPlayer(CyanPlayerId);
        const auto magenta = FindPlayer(MagentaPlayerId);
        if (MeshRenderer* mesh = m_runtime.Get<MeshRenderer>(m_cyanEntity))
        {
            mesh->visible = cyan != m_players.end() && cyan->isAlive;
            mesh->emissive = 0.12f;
        }
        if (MeshRenderer* mesh = m_runtime.Get<MeshRenderer>(m_magentaEntity))
        {
            mesh->visible = magenta != m_players.end() && magenta->isAlive;
            mesh->emissive = 0.12f;
        }
        if (MeshRenderer* shield = m_runtime.Get<MeshRenderer>(m_shieldEntity))
            shield->visible = m_match.phase == MatchPhase::InProgress || m_match.phase == MatchPhase::Overtime;

        if (SpriteRenderer* cyanHud = m_runtime.Get<SpriteRenderer>(m_cyanHudEntity))
        {
            cyanHud->sourceRect = Spark::Templates::TemplateRuntimeScene::SheetCell(0, 0);
            cyanHud->visible = cyan != m_players.end();
        }
        if (SpriteRenderer* magentaHud = m_runtime.Get<SpriteRenderer>(m_magentaHudEntity))
        {
            magentaHud->sourceRect = Spark::Templates::TemplateRuntimeScene::SheetCell(1, 0);
            magentaHud->visible = magenta != m_players.end();
        }
        if (SpriteRenderer* phaseHud = m_runtime.Get<SpriteRenderer>(m_phaseHudEntity))
        {
            switch (m_match.phase)
            {
            case MatchPhase::Lobby:
                phaseHud->sourceRect = Spark::Templates::TemplateRuntimeScene::SheetCell(2, 1);
                break;
            case MatchPhase::Countdown:
                phaseHud->sourceRect = Spark::Templates::TemplateRuntimeScene::SheetCell(0, 2);
                break;
            case MatchPhase::InProgress:
                phaseHud->sourceRect = Spark::Templates::TemplateRuntimeScene::SheetCell(1, 2);
                break;
            case MatchPhase::Overtime:
            case MatchPhase::PostMatch:
                phaseHud->sourceRect = Spark::Templates::TemplateRuntimeScene::SheetCell(2, 2);
                break;
            }
            phaseHud->visible = true;
        }

        m_runtime.PlaceHud(m_cameraEntity, m_cyanHudEntity, -0.13f, 0.08f, 0.32f, 0.05f, 0.05f);
        m_runtime.PlaceHud(m_cameraEntity, m_phaseHudEntity, 0.0f, 0.08f, 0.32f, 0.05f, 0.05f);
        m_runtime.PlaceHud(m_cameraEntity, m_magentaHudEntity, 0.13f, 0.08f, 0.32f, 0.05f, 0.05f);
    }

    Spark::IEngineContext* m_context = nullptr;
    Spark::Templates::TemplateRuntimeScene m_runtime;
    MatchState m_match;
    std::vector<NetPlayer> m_players;
    DirectX::XMFLOAT3 m_cyanSpawn{};
    DirectX::XMFLOAT3 m_magentaSpawn{};
    uint32_t m_cameraEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_cyanEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_magentaEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_shieldEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_cyanHudEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_magentaHudEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_phaseHudEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    bool m_cyanAttackHeld = false;
    bool m_magentaAttackHeld = false;
    bool m_resetHeld = false;
};
