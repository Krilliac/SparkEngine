/**
 * @file TestFPSGameCameraReloadReal.cpp
 * @brief Fresh-process regression for authored camera reloads and the respawn
 *        loadout/spawn-protection contract through the real FPS DLL.
 */

#include "Game/Game.h"
#include "Game/Player.h"
#include "Camera/SparkEngineCamera.h"
#include "Graphics/GraphicsEngine.h"
#include "Input/InputManager.h"

#include <Spark/EventBus.h>
#include <Spark/EventSystem.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
    void Require(bool condition, const std::string& message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    void ExpectNear(float actual, float expected, const std::string& field)
    {
        Require(std::isfinite(actual) && std::abs(actual - expected) <= 0.0001f,
                field + ": expected " + std::to_string(expected) + ", got " + std::to_string(actual));
    }

    void ExpectVector(const DirectX::XMFLOAT3& actual, const DirectX::XMFLOAT3& expected, const std::string& field)
    {
        ExpectNear(actual.x, expected.x, field + ".x");
        ExpectNear(actual.y, expected.y, field + ".y");
        ExpectNear(actual.z, expected.z, field + ".z");
    }

    void ExpectCamera(const SparkEngineCamera::CameraState& actual, const DirectX::XMFLOAT3& position,
                      const DirectX::XMFLOAT3& rotation, float fov, float nearPlane, float farPlane)
    {
        ExpectVector(actual.position, position, "camera position");
        ExpectVector(actual.rotation, rotation, "camera rotation");
        ExpectNear(actual.defaultFov, fov, "default FOV");
        ExpectNear(actual.currentFov, fov, "current FOV");
        ExpectNear(actual.nearPlane, nearPlane, "near plane");
        ExpectNear(actual.farPlane, farPlane, "far plane");
    }

    class OwnedSceneDirectory
    {
      public:
        explicit OwnedSceneDirectory(const std::filesystem::path& scenes)
        {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            m_path = scenes / ("fps-camera-reload-" + std::to_string(stamp));
            Require(std::filesystem::create_directory(m_path), "could not create a fresh owned scene directory");
        }

        ~OwnedSceneDirectory()
        {
            std::error_code error;
            std::filesystem::remove_all(m_path, error);
        }

        const std::filesystem::path& Path() const { return m_path; }

      private:
        std::filesystem::path m_path;
    };

    void WriteScene(const std::filesystem::path& path, std::string_view text)
    {
        std::ofstream stream(path, std::ios::binary);
        stream << text;
        stream.close();
        Require(!stream.fail(), "could not write scene fixture: " + path.string());
    }
} // namespace

int main(int argc, char** argv)
{
    try
    {
        Require(argc == 2, "expected the isolated runtime directory argument");
        const auto runtime = std::filesystem::canonical(argv[1]);
        Require(std::filesystem::canonical(argv[0]).parent_path() == runtime,
                "runtime assets must be adjacent to this test executable");
        const auto assets = std::filesystem::canonical(runtime / "Assets");
        Require(assets.parent_path() == runtime && std::filesystem::is_directory(assets / "Models") &&
                    std::filesystem::is_regular_file(assets / "Scenes" / "level1.scene"),
                "required staged FPS assets are missing");
        Require(!std::filesystem::exists(runtime / "Startup.sparkscene"),
                "the legacy reload regression requires an isolated legacy startup");
        const auto scenes = std::filesystem::canonical(assets / "Scenes");
        Require(scenes.parent_path() == assets, "scene fixtures must stay inside the staged runtime assets");
        OwnedSceneDirectory owned(scenes);
        WriteScene(owned.Path() / "a.scene",
                   "[Scene]\nname=ReloadA\n[Object]\ntype=plane\nname=FloorA\nposition=0,0,0\n"
                   "[Camera]\nname=CameraA\nposition=1,2,-20\nrotation=5,10,0\nprojection=perspective\n"
                   "isMain=true\nfov=70\nnearPlane=0.1\nfarPlane=1000\n");
        WriteScene(owned.Path() / "b.scene",
                   "[Scene]\nname=ReloadB\n[Object]\ntype=plane\nname=FloorB\nposition=3,0,4\n"
                   "[Camera]\nname=CameraB\nposition=7,3,-17\nrotation=15,25,0\nprojection=perspective\n"
                   "isMain=true\nfov=95\nnearPlane=0.25\nfarPlane=750\n");
        WriteScene(owned.Path() / "malformed.scene",
                   "[Scene]\nname=Broken\n[Camera]\nname=BrokenCamera\nposition=not-a-transform\n");

        // These are real engine objects. Game supports CPU initialization without
        // a graphics device; no simulation tick or platform input window is needed.
        GraphicsEngine graphics;
        InputManager input;
        Spark::EventBus bus; // declared before Game so it outlives Game's subscription handles
        Game game;
        Require(SUCCEEDED(game.Initialize(&graphics, &input)), "real FPS Game initialization failed");
        SparkEngineCamera* const camera = game.GetCamera();
        Player* const player = game.GetPlayer();
        SceneManager* const scene = game.GetSceneManager();
        Require(camera && player && scene, "the real game did not initialize camera/player/scene state");
        const auto relative = owned.Path().filename().generic_string() + "/";

        Require(game.LoadScene(relative + "a.scene"), "authored scene A failed to load");
        Require(game.GetCamera() == camera && game.GetPlayer() == player && game.GetSceneManager() == scene,
                "reload A replaced runtime owners");
        ExpectCamera(camera->Console_GetState(), {1, 2, -20}, {5, 10, 0}, 70, 0.1f, 1000);
        ExpectVector(player->GetPosition(), {1, 2, -20}, "player position after A");
        Require(scene->FindNode("FloorA") >= 0 && scene->FindNode("CameraA") >= 0, "scene A was not committed");

        Require(game.LoadScene(relative + "b.scene"), "authored scene B failed to load");
        Require(game.GetCamera() == camera && game.GetPlayer() == player && game.GetSceneManager() == scene,
                "reload B replaced runtime owners");
        const auto committedCamera = camera->Console_GetState();
        ExpectCamera(committedCamera, {7, 3, -17}, {15, 25, 0}, 95, 0.25f, 750);
        ExpectVector(player->GetPosition(), {7, 3, -17}, "player position after B");
        Require(scene->FindNode("FloorB") >= 0 && scene->FindNode("CameraB") >= 0 && scene->FindNode("FloorA") < 0,
                "scene B did not replace scene A");
        const int committedCount = scene->GetNodeCount();
        const auto committedPath = scene->GetCurrentFilePath();
        const auto committedPlayerPosition = player->GetPosition();

        Require(!game.LoadScene(relative + "malformed.scene"), "malformed existing scene was accepted");
        Require(game.GetCamera() == camera && game.GetPlayer() == player && game.GetSceneManager() == scene,
                "failed reload replaced runtime owners");
        ExpectCamera(camera->Console_GetState(), committedCamera.position, committedCamera.rotation,
                     committedCamera.defaultFov, committedCamera.nearPlane, committedCamera.farPlane);
        ExpectVector(player->GetPosition(), committedPlayerPosition, "player position after rejected reload");
        Require(scene->GetNodeCount() == committedCount && scene->GetCurrentFilePath() == committedPath &&
                    scene->GetMetadata().sceneName == "ReloadB" && scene->FindNode("FloorB") >= 0 &&
                    scene->FindNode("CameraB") >= 0 && scene->FindNode("BrokenCamera") < 0,
                "failed reload changed the committed scene graph");

        // Respawn restores the class loadout and grants spawn protection. The
        // initial state is the SCOUT loadout Game applied at startup.
        const float loadoutArmor = player->GetArmor();
        const float loadoutShield = player->GetMaxShield();
        Require(player->GetShield() == loadoutShield && player->GetHealth() == player->GetMaxHealth(),
                "player did not start at its class loadout");
        player->TakeDamage(10.0f);
        player->Console_SetArmor(0.0f);
        player->Console_SetHealth(0.0f);
        Require(!player->IsAlive() && !player->IsActive(), "the player was not dead before respawn");
        Require(loadoutArmor > 0.0f, "the respawn armor check needs a class loadout with armor");

        // Drive the respawn through the bus so the Game handler is what runs.
        game.SetEventBus(&bus);
        bus.Publish(Spark::PlayerRespawnEvent{0, 2.0f, 3.0f, 4.0f});
        Require(player->IsAlive() && player->IsActive(), "respawn did not revive the player");
        Require(player->GetHealth() == player->GetMaxHealth(), "respawn did not restore health");
        Require(player->GetArmor() == loadoutArmor, "respawn did not restore the loadout armor");
        Require(player->GetShield() == loadoutShield, "respawn did not restore the loadout shield");
        ExpectVector(player->GetPosition(), {2, 3, 4}, "player position after respawn");
        Require(player->IsSpawnProtected(), "respawn did not start spawn protection");

        const float protectedTotal = player->GetHealth() + player->GetShield() + player->GetArmor();
        player->TakeDamage(50.0f);
        Require(player->GetHealth() + player->GetShield() + player->GetArmor() == protectedTotal,
                "damage inside the spawn-protection window was applied");

        player->Update(Player::kSpawnProtectionSeconds + 0.1f);
        Require(!player->IsSpawnProtected(), "spawn protection did not expire");
        player->TakeDamage(10.0f);
        Require(player->GetHealth() + player->GetShield() + player->GetArmor() < protectedTotal,
                "damage after the spawn-protection window was ignored");

        player->Console_SetGodMode(true);
        const float godTotal = player->GetHealth() + player->GetShield() + player->GetArmor();
        player->TakeDamage(10.0f);
        player->Console_SetGodMode(false);
        Require(player->GetHealth() + player->GetShield() + player->GetArmor() == godTotal,
                "god mode no longer blocks damage");

        game.SetEventBus(nullptr);

        std::cout << "Real FPS DLL camera reload, failed-load retention and respawn protection passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Real FPS DLL camera reload failed: " << error.what() << '\n';
        return 1;
    }
}
