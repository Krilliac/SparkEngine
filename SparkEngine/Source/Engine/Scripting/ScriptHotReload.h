/**
 * @file ScriptHotReload.h
 * @brief File watcher and hot-reload system for AngelScript scripts
 *
 * Monitors script directories for changes and automatically recompiles
 * modified scripts. Preserves script state across reloads where possible.
 *
 * ## Usage
 * @code
 *   ScriptHotReloadManager hotReload;
 *   hotReload.AddWatchDirectory("Scripts/");
 *   hotReload.SetRecompileCallback([&](const std::string& file) {
 *       scriptEngine.CompileFile(file);
 *   });
 *   hotReload.Start();
 *
 *   // In main loop:
 *   hotReload.PollChanges();  // Check for modified files
 * @endcode
 */

#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Scripting
{

    // ============================================================================
    // File Change Event
    // ============================================================================

    enum class FileChangeType
    {
        Modified,
        Created,
        Deleted,
        Renamed
    };

    struct FileChangeEvent
    {
        std::string filePath;
        FileChangeType type;
        std::chrono::system_clock::time_point timestamp;
    };

    // ============================================================================
    // Recompile Result
    // ============================================================================

    struct RecompileResult
    {
        bool success = false;
        std::string filePath;
        std::string errorMessage;
        int errorLine = 0;
        float compileTimeMs = 0.0f;
    };

    // ============================================================================
    // Script Hot-Reload Manager
    // ============================================================================

    using RecompileCallback = std::function<RecompileResult(const std::string&)>;
    using ErrorCallback = std::function<void(const RecompileResult&)>;

    /**
 * @brief Watches script directories and triggers recompilation on changes
 */
    class ScriptHotReloadManager
    {
      public:
        ScriptHotReloadManager();
        ~ScriptHotReloadManager();

        // -- Configuration --

        /**
     * @brief Add a directory to watch for script changes
     * @param directory Path to watch (relative or absolute)
     * @param recursive Watch subdirectories too
     */
        void AddWatchDirectory(const std::string& directory, bool recursive = true);

        /**
     * @brief Set file extensions to watch (default: .as, .angelscript)
     */
        void SetWatchExtensions(const std::vector<std::string>& extensions);

        /**
     * @brief Set callback for recompiling a changed script
     */
        void SetRecompileCallback(RecompileCallback callback);

        /**
     * @brief Set callback for reporting compilation errors
     */
        void SetErrorCallback(ErrorCallback callback);

        /**
     * @brief Set minimum delay between detecting a change and triggering recompile
     * Prevents rapid re-triggers while the user is still saving.
     */
        void SetDebounceMs(uint32_t ms);

        // -- Lifecycle --

        /**
     * @brief Start watching directories
     */
        void Start();

        /**
     * @brief Stop watching directories
     */
        void Stop();

        /**
     * @brief Check for file changes and trigger recompilation
     * Call this each frame from the main thread.
     * @return Number of files recompiled
     */
        int PollChanges();

        /**
     * @brief Force recompile all watched scripts
     * @return Number of files recompiled
     */
        int RecompileAll();

        // -- State --

        bool IsRunning() const { return m_running; }
        int GetWatchedFileCount() const { return static_cast<int>(m_fileStates.size()); }
        int GetRecompileCount() const { return m_recompileCount; }
        int GetErrorCount() const { return m_errorCount; }
        const std::vector<RecompileResult>& GetRecentErrors() const { return m_recentErrors; }

        // -- Console --

        std::string Console_GetStatus() const;

      private:
        struct FileState
        {
            std::string path;
            std::filesystem::file_time_type lastModified;
            size_t lastSize = 0;
        };

        void ScanDirectory(const std::string& directory, bool recursive);
        bool IsWatchedExtension(const std::string& path) const;
        void ProcessChange(const std::string& filePath);

        std::vector<std::string> m_watchDirs;
        std::vector<std::string> m_extensions = {".as", ".angelscript"};
        std::unordered_map<std::string, FileState> m_fileStates;

        RecompileCallback m_recompileCallback;
        ErrorCallback m_errorCallback;

        // Debounce: track pending changes
        struct PendingChange
        {
            std::string filePath;
            std::chrono::steady_clock::time_point detectedAt;
        };
        std::vector<PendingChange> m_pendingChanges;
        uint32_t m_debounceMs = 300; // 300ms default debounce

        std::atomic<bool> m_running{false};
        int m_recompileCount = 0;
        int m_errorCount = 0;
        std::vector<RecompileResult> m_recentErrors; ///< Last N errors
        static constexpr int MaxRecentErrors = 10;
    };

    // ============================================================================
    // Engine API Registry for AngelScript
    // ============================================================================

    /**
 * @brief Registers engine subsystem APIs with the AngelScript engine
 *
 * Exposes ECS components, physics raycasts, audio, input, and other engine
 * APIs to scripts. Called during script engine initialization.
 */
    struct ScriptAPIRegistry
    {
        /**
     * @brief Categories of engine API available to scripts
     */
        enum class APICategory
        {
            Core,       ///< Math, strings, containers
            ECS,        ///< Entity creation, component access
            Physics,    ///< Raycasts, forces, collision queries
            Audio,      ///< Play/stop sounds, volume, 3D audio
            Input,      ///< Key/mouse/gamepad state queries
            UI,         ///< HUD elements, text rendering
            Scene,      ///< Scene loading, transitions
            AI,         ///< NavMesh queries, behavior tree control
            Animation,  ///< Animation playback, blending
            Networking, ///< Send/receive messages
            Debug       ///< Console logging, debug draw
        };

        /**
     * @brief Describes a registered API function
     */
        struct APIFunction
        {
            std::string name;          ///< Function name in script
            std::string signature;     ///< AngelScript signature
            APICategory category;      ///< Which subsystem it belongs to
            std::string documentation; ///< Brief description
        };

        /**
     * @brief Get all registered API functions
     */
        static const std::vector<APIFunction>& GetRegisteredFunctions()
        {
            static std::vector<APIFunction> functions = {
                // Core
                {"Log", "void Log(const string &in)", APICategory::Core, "Print to console"},
                {"LogWarning", "void LogWarning(const string &in)", APICategory::Core, "Print warning"},
                {"LogError", "void LogError(const string &in)", APICategory::Core, "Print error"},
                {"GetDeltaTime", "float GetDeltaTime()", APICategory::Core, "Frame delta time in seconds"},
                {"GetGameTime", "float GetGameTime()", APICategory::Core, "Total elapsed game time"},

                // ECS
                {"CreateEntity", "uint CreateEntity()", APICategory::ECS, "Create a new entity"},
                {"DestroyEntity", "void DestroyEntity(uint)", APICategory::ECS, "Destroy entity by ID"},
                {"GetPosition", "Vector3 GetPosition(uint)", APICategory::ECS, "Get entity world position"},
                {"SetPosition", "void SetPosition(uint, Vector3)", APICategory::ECS, "Set entity world position"},
                {"GetRotation", "Quaternion GetRotation(uint)", APICategory::ECS, "Get entity rotation"},
                {"SetRotation", "void SetRotation(uint, Quaternion)", APICategory::ECS, "Set entity rotation"},
                {"GetHealth", "float GetHealth(uint)", APICategory::ECS, "Get entity health"},
                {"SetHealth", "void SetHealth(uint, float)", APICategory::ECS, "Set entity health"},
                {"IsAlive", "bool IsAlive(uint)", APICategory::ECS, "Check if entity is alive"},

                // Physics
                {"Raycast", "bool Raycast(Vector3, Vector3, float, RayHit &out)", APICategory::Physics,
                 "Cast ray and get hit info"},
                {"ApplyForce", "void ApplyForce(uint, Vector3)", APICategory::Physics, "Apply force to rigidbody"},
                {"ApplyImpulse", "void ApplyImpulse(uint, Vector3)", APICategory::Physics,
                 "Apply impulse to rigidbody"},
                {"SetVelocity", "void SetVelocity(uint, Vector3)", APICategory::Physics, "Set linear velocity"},
                {"GetVelocity", "Vector3 GetVelocity(uint)", APICategory::Physics, "Get linear velocity"},

                // Audio
                {"PlaySound", "void PlaySound(const string &in)", APICategory::Audio, "Play a sound effect by name"},
                {"PlaySoundAt", "void PlaySoundAt(const string &in, Vector3)", APICategory::Audio,
                 "Play 3D sound at position"},
                {"StopSound", "void StopSound(const string &in)", APICategory::Audio, "Stop a playing sound"},
                {"SetVolume", "void SetVolume(float)", APICategory::Audio, "Set master volume [0, 1]"},

                // Input
                {"IsKeyDown", "bool IsKeyDown(int)", APICategory::Input, "Check if key is currently held"},
                {"IsKeyPressed", "bool IsKeyPressed(int)", APICategory::Input, "Check if key was just pressed"},
                {"GetMousePosition", "Vector2 GetMousePosition()", APICategory::Input, "Get mouse screen position"},
                {"GetMouseDelta", "Vector2 GetMouseDelta()", APICategory::Input, "Get mouse movement delta"},
                {"IsMouseButtonDown", "bool IsMouseButtonDown(int)", APICategory::Input, "Check mouse button state"},
                {"GetGamepadAxis", "float GetGamepadAxis(int, int)", APICategory::Input, "Get gamepad axis value"},

                // UI
                {"DrawText", "void DrawText(const string &in, float, float, float)", APICategory::UI,
                 "Draw text at screen position"},
                {"DrawProgressBar", "void DrawProgressBar(float, float, float, float, float)", APICategory::UI,
                 "Draw progress bar (x, y, w, h, value)"},

                // Scene
                {"LoadScene", "void LoadScene(const string &in)", APICategory::Scene, "Load scene by name"},
                {"GetCurrentScene", "string GetCurrentScene()", APICategory::Scene, "Get current scene name"},

                // AI
                {"FindPath", "bool FindPath(Vector3, Vector3, array<Vector3> &out)", APICategory::AI,
                 "Find NavMesh path between two points"},
                {"GetRandomNavPoint", "Vector3 GetRandomNavPoint()", APICategory::AI, "Random walkable NavMesh point"},

                // Animation
                {"PlayAnimation", "void PlayAnimation(uint, const string &in)", APICategory::Animation,
                 "Play animation clip on entity"},
                {"SetAnimationSpeed", "void SetAnimationSpeed(uint, float)", APICategory::Animation,
                 "Set animation playback speed"},

                // Debug
                {"DebugDrawLine", "void DebugDrawLine(Vector3, Vector3, Color)", APICategory::Debug,
                 "Draw debug line for one frame"},
                {"DebugDrawSphere", "void DebugDrawSphere(Vector3, float, Color)", APICategory::Debug,
                 "Draw debug sphere for one frame"},
            };
            return functions;
        }

        /**
     * @brief Get functions for a specific category
     */
        static std::vector<APIFunction> GetFunctionsByCategory(APICategory category)
        {
            std::vector<APIFunction> result;
            for (const auto& fn : GetRegisteredFunctions())
            {
                if (fn.category == category)
                {
                    result.push_back(fn);
                }
            }
            return result;
        }

        /**
     * @brief Get count of registered functions per category
     */
        static int GetFunctionCount(APICategory category)
        {
            int count = 0;
            for (const auto& fn : GetRegisteredFunctions())
            {
                if (fn.category == category)
                    count++;
            }
            return count;
        }
    };

} // namespace Spark::Scripting
