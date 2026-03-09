/**
 * @file VisualScriptingSystem.h
 * @brief Visual scripting system with Blueprint-style node editor for Spark Engine
 * @author Spark Engine Team
 * @date 2025
 * 
 * This file implements a comprehensive visual scripting system similar to Unreal's
 * Blueprint system, allowing game logic creation through node-based programming
 * with real-time execution and debugging capabilities.
 */

#pragma once

// Standard library headers first (MSVC /Zc:preprocessor compatibility)
#include <functional>
#include <memory>
#include <queue>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

// Platform headers
#ifdef _WIN32
#include <DirectXMath.h>
#else
#include "Core/Platform.h"
#endif

using namespace DirectX;

// Project headers
#include "../Core/EditorPanel.h"
#include "../SceneSystem/SceneFile.h"

namespace SparkEditor
{

    /**
 * @brief Script variable types
 */
    enum class ScriptVariableType
    {
        BOOLEAN = 0,
        INTEGER = 1,
        FLOAT = 2,
        STRING = 3,
        VECTOR2 = 4,
        VECTOR3 = 5,
        VECTOR4 = 6,
        COLOR = 7,
        OBJECT_REFERENCE = 8,
        COMPONENT_REFERENCE = 9,
        ASSET_REFERENCE = 10,
        ARRAY = 11,
        CUSTOM_STRUCT = 12,
        EXECUTION = 13 ///< Special type for execution flow
    };

    /**
 * @brief Forward declaration for recursive script value array
 */
    struct ScriptValueArray;

    /**
 * @brief Script variable value container
 */
    using ScriptValue = std::variant<bool, int, float, std::string, XMFLOAT2, XMFLOAT3, XMFLOAT4, ObjectID,
                                     std::shared_ptr<ScriptValueArray>>;

    /**
 * @brief Wrapper for recursive script value arrays
 */
    struct ScriptValueArray
    {
        std::vector<ScriptValue> values;
    };

    /**
 * @brief Visual script node categories
 */
    enum class ScriptNodeCategory
    {
        EVENT = 0,        ///< Event nodes (Start, Update, Input, etc.)
        FLOW_CONTROL = 1, ///< Flow control (If, While, For, etc.)
        MATH = 2,         ///< Mathematical operations
        LOGIC = 3,        ///< Boolean logic operations
        COMPARISON = 4,   ///< Comparison operations
        STRING = 5,       ///< String operations
        ARRAY = 6,        ///< Array operations
        OBJECT = 7,       ///< Object/component access
        INPUT = 8,        ///< Input handling
        AUDIO = 9,        ///< Audio system
        GRAPHICS = 10,    ///< Graphics and rendering
        PHYSICS = 11,     ///< Physics system
        ANIMATION = 12,   ///< Animation system
        UI = 13,          ///< User interface
        CUSTOM = 14,      ///< Custom user nodes
        FUNCTION = 15,    ///< Function call nodes
        VARIABLE = 16,    ///< Variable access nodes
        UTILITY = 17      ///< Utility operations
    };

    /**
 * @brief Visual script node types
 */
    enum class ScriptNodeType
    {
        // Event nodes
        EVENT_START = 0,
        EVENT_UPDATE = 1,
        EVENT_INPUT_KEY = 2,
        EVENT_INPUT_MOUSE = 3,
        EVENT_COLLISION = 4,
        EVENT_TRIGGER = 5,
        EVENT_TIMER = 6,
        EVENT_CUSTOM = 7,

        // Flow control nodes
        SEQUENCE = 10,
        BRANCH = 11,
        SWITCH = 12,
        FOR_LOOP = 13,
        WHILE_LOOP = 14,
        DELAY = 15,
        GATE = 16,
        FLIP_FLOP = 17,

        // Math nodes
        ADD = 20,
        SUBTRACT = 21,
        MULTIPLY = 22,
        DIVIDE = 23,
        POWER = 24,
        SQRT = 25,
        SIN = 26,
        COS = 27,
        TAN = 28,
        CLAMP = 29,
        LERP = 30,

        // Logic nodes
        AND = 40,
        OR = 41,
        NOT = 42,
        XOR = 43,

        // Comparison nodes
        EQUAL = 50,
        NOT_EQUAL = 51,
        LESS = 52,
        LESS_EQUAL = 53,
        GREATER = 54,
        GREATER_EQUAL = 55,

        // String nodes
        STRING_CONCAT = 60,
        STRING_LENGTH = 61,
        STRING_SUBSTRING = 62,
        STRING_CONTAINS = 63,
        STRING_REPLACE = 64,
        STRING_TO_UPPER = 65,
        STRING_TO_LOWER = 66,

        // Array nodes
        ARRAY_GET = 70,
        ARRAY_SET = 71,
        ARRAY_ADD = 72,
        ARRAY_REMOVE = 73,
        ARRAY_LENGTH = 74,
        ARRAY_CONTAINS = 75,
        ARRAY_FIND = 76,

        // Object nodes
        GET_COMPONENT = 80,
        SET_TRANSFORM = 81,
        GET_TRANSFORM = 82,
        DESTROY_OBJECT = 83,
        INSTANTIATE = 84,
        FIND_OBJECT = 85,

        // Input nodes
        INPUT_KEY_DOWN = 90,
        INPUT_KEY_UP = 91,
        INPUT_MOUSE_BUTTON = 92,
        INPUT_MOUSE_POSITION = 93,
        INPUT_AXIS = 94,

        // Audio nodes
        PLAY_SOUND = 100,
        STOP_SOUND = 101,
        SET_VOLUME = 102,

        // Graphics nodes
        SET_MATERIAL = 110,
        SET_COLOR = 111,
        SET_VISIBILITY = 112,

        // Physics nodes
        ADD_FORCE = 120,
        SET_VELOCITY = 121,
        RAYCAST = 122,

        // Variable nodes
        GET_VARIABLE = 130,
        SET_VARIABLE = 131,

        // Function nodes
        FUNCTION_CALL = 140,
        FUNCTION_RETURN = 141,

        // Custom nodes
        CUSTOM_NODE = 1000
    };

    /**
 * @brief Script node socket (pin)
 */
    struct ScriptSocket
    {
        std::string name;         ///< Socket display name
        ScriptVariableType type;  ///< Data type
        bool isInput;             ///< Input or output socket
        bool isExecution;         ///< Execution flow socket
        ScriptValue defaultValue; ///< Default value for input sockets
        std::string tooltip;      ///< Socket tooltip

        // Visual properties
        XMFLOAT2 position = {0, 0};    ///< Socket position relative to node
        XMFLOAT4 color = {1, 1, 1, 1}; ///< Socket color based on type
        bool isConnected = false;      ///< Whether socket has connections
        bool isRequired = false;       ///< Whether input is required
    };

    /**
 * @brief Connection between script sockets
 */
    struct ScriptConnection
    {
        uint32_t fromNodeID;      ///< Source node ID
        uint32_t fromSocketIndex; ///< Source socket index
        uint32_t toNodeID;        ///< Target node ID
        uint32_t toSocketIndex;   ///< Target socket index

        // Visual properties
        XMFLOAT4 color = {1, 1, 1, 1}; ///< Connection line color
        float thickness = 2.0f;        ///< Connection line thickness
        bool isActive = false;         ///< Whether connection is active during execution
        bool isSelected = false;       ///< Whether connection is selected
    };

    /**
 * @brief Visual script node
 */
    struct ScriptNode
    {
        uint32_t id;                 ///< Unique node ID
        ScriptNodeType type;         ///< Node type
        ScriptNodeCategory category; ///< Node category
        std::string name;            ///< Node display name
        std::string description;     ///< Node description

        // Position and visual properties
        XMFLOAT2 position = {0, 0};                       ///< Node position in graph
        XMFLOAT2 size = {150, 100};                       ///< Node size
        XMFLOAT4 headerColor = {0.2f, 0.3f, 0.5f, 1.0f};  ///< Header background color
        XMFLOAT4 bodyColor = {0.15f, 0.15f, 0.15f, 1.0f}; ///< Body background color

        // Sockets
        std::vector<ScriptSocket> inputSockets;  ///< Input sockets
        std::vector<ScriptSocket> outputSockets; ///< Output sockets

        // Node-specific data
        std::unordered_map<std::string, ScriptValue> properties; ///< Node properties
        std::string code;                                        ///< Generated code fragment

        // Execution state
        bool isBreakpoint = false; ///< Whether node has breakpoint
        bool isExecuting = false;  ///< Whether node is currently executing
        bool hasError = false;     ///< Whether node has execution error
        std::string errorMessage;  ///< Error message if any

        // Visual state
        bool isSelected = false;  ///< Whether node is selected
        bool isCollapsed = false; ///< Whether node is collapsed
        bool isEnabled = true;    ///< Whether node is enabled

        /**
     * @brief Execute node logic
     * @param inputs Input values
     * @param outputs Output values to set
     * @param context Execution context
     * @return true if execution succeeded
     */
        virtual ~ScriptNode() = default;

        virtual bool Execute(const std::vector<ScriptValue>& inputs, std::vector<ScriptValue>& outputs,
                             class ScriptExecutionContext* context) = 0;
    };

    /**
 * @brief Script function definition
 */
    struct ScriptFunction
    {
        std::string name;                     ///< Function name
        std::string description;              ///< Function description
        std::vector<ScriptSocket> parameters; ///< Function parameters
        std::vector<ScriptSocket> returns;    ///< Return values
        uint32_t entryNodeID = 0;             ///< Entry node ID
        uint32_t returnNodeID = 0;            ///< Return node ID
        bool isPublic = true;                 ///< Whether function is public
        bool isPure = false;                  ///< Whether function has no side effects
    };

    /**
 * @brief Script variable definition
 */
    struct ScriptVariable
    {
        std::string name;                   ///< Variable name
        std::string description;            ///< Variable description
        ScriptVariableType type;            ///< Variable type
        ScriptValue defaultValue;           ///< Default value
        bool isPublic = false;              ///< Whether variable is exposed in inspector
        bool isConstant = false;            ///< Whether variable is constant
        std::string category = "Variables"; ///< Variable category for grouping
    };

    /**
 * @brief Visual script graph
 */
    struct ScriptGraph
    {
        std::string name = "New Script";                ///< Script name
        std::string description;                        ///< Script description
        std::vector<std::unique_ptr<ScriptNode>> nodes; ///< All nodes in graph
        std::vector<ScriptConnection> connections;      ///< All connections
        std::vector<ScriptFunction> functions;          ///< Script functions
        std::vector<ScriptVariable> variables;          ///< Script variables

        uint32_t nextNodeID = 1;                     ///< Next available node ID
        ObjectID targetObjectID = INVALID_OBJECT_ID; ///< Target object for script

        // Graph view properties
        XMFLOAT2 viewOffset = {0, 0}; ///< Graph view offset
        float viewScale = 1.0f;       ///< Graph view scale

        // Execution state
        bool isCompiled = false;                    ///< Whether script is compiled
        std::vector<std::string> compilationErrors; ///< Compilation errors
        std::vector<uint8_t> bytecode;              ///< Compiled bytecode

        /**
     * @brief Find node by ID
     * @param nodeID Node ID to find
     * @return Pointer to node, or nullptr if not found
     */
        ScriptNode* FindNode(uint32_t nodeID);

        /**
     * @brief Add node to graph
     * @param node Node to add
     * @return Node ID
     */
        uint32_t AddNode(std::unique_ptr<ScriptNode> node);

        /**
     * @brief Remove node from graph
     * @param nodeID Node ID to remove
     * @return true if node was removed
     */
        bool RemoveNode(uint32_t nodeID);

        /**
     * @brief Create connection between sockets
     * @param connection Connection to create
     * @return true if connection was created
     */
        bool CreateConnection(const ScriptConnection& connection);

        /**
     * @brief Remove connection
     * @param fromNodeID Source node ID
     * @param fromSocket Source socket index
     * @return true if connection was removed
     */
        bool RemoveConnection(uint32_t fromNodeID, uint32_t fromSocket);

        /**
     * @brief Validate graph for compilation
     * @param errors Output error messages
     * @return true if graph is valid
     */
        bool Validate(std::vector<std::string>& errors) const;

        /**
     * @brief Compile graph to bytecode
     * @return true if compilation succeeded
     */
        bool Compile();
    };

    /**
 * @brief Script execution context
 */
    class ScriptExecutionContext
    {
      public:
        /**
     * @brief Constructor
     * @param targetObject Target object for script execution
     */
        ScriptExecutionContext(ObjectID targetObject);

        /**
     * @brief Get variable value
     * @param name Variable name
     * @return Variable value
     */
        ScriptValue GetVariable(const std::string& name) const;

        /**
     * @brief Set variable value
     * @param name Variable name
     * @param value New value
     */
        void SetVariable(const std::string& name, const ScriptValue& value);

        /**
     * @brief Get target object
     * @return Target object ID
     */
        ObjectID GetTargetObject() const { return m_targetObject; }

        /**
     * @brief Get component from target object
     * @tparam T Component type
     * @return Pointer to component, or nullptr if not found
     */
        template <typename T> T* GetComponent() const;

        /**
     * @brief Log message
     * @param message Message to log
     * @param level Log level
     */
        void Log(const std::string& message, const std::string& level = "INFO");

        /**
     * @brief Get delta time
     * @return Current frame delta time
     */
        float GetDeltaTime() const;

        /**
     * @brief Check if execution should stop
     * @return true if execution should be halted
     */
        bool ShouldStop() const { return m_shouldStop; }

        /**
     * @brief Request execution stop
     */
        void RequestStop() { m_shouldStop = true; }

      private:
        ObjectID m_targetObject;                                  ///< Target object ID
        std::unordered_map<std::string, ScriptValue> m_variables; ///< Variable values
        bool m_shouldStop = false;                                ///< Execution stop flag
    };

    /**
 * @brief Script execution engine
 */
    class ScriptExecutor
    {
      public:
        /**
     * @brief Constructor
     */
        ScriptExecutor();

        /**
     * @brief Destructor
     */
        ~ScriptExecutor();

        /**
     * @brief Execute script graph
     * @param graph Script graph to execute
     * @param context Execution context
     * @param startNodeID Starting node (0 for event-based start)
     * @return true if execution completed successfully
     */
        bool ExecuteGraph(const ScriptGraph& graph, ScriptExecutionContext& context, uint32_t startNodeID = 0);

        /**
     * @brief Execute single node
     * @param node Node to execute
     * @param context Execution context
     * @return true if execution succeeded
     */
        bool ExecuteNode(ScriptNode* node, ScriptExecutionContext& context);

        /**
     * @brief Start debugging session
     * @param graph Graph to debug
     * @param context Execution context
     */
        void StartDebugging(const ScriptGraph& graph, ScriptExecutionContext& context);

        /**
     * @brief Stop debugging session
     */
        void StopDebugging();

        /**
     * @brief Step to next node in debugging
     */
        void StepNext();

        /**
     * @brief Continue execution in debugging
     */
        void Continue();

        /**
     * @brief Set breakpoint on node
     * @param nodeID Node ID to set breakpoint
     * @param enabled Whether breakpoint is enabled
     */
        void SetBreakpoint(uint32_t nodeID, bool enabled);

        /**
     * @brief Check if debugging is active
     * @return true if debugging session is active
     */
        bool IsDebugging() const { return m_isDebugging; }

        /**
     * @brief Get current execution node (during debugging)
     * @return Current node ID, or 0 if not debugging
     */
        uint32_t GetCurrentNode() const { return m_currentNode; }

      private:
        /**
     * @brief Execute nodes in topological order
     * @param graph Script graph
     * @param context Execution context
     * @param startNodes Starting nodes
     * @return true if execution succeeded
     */
        bool ExecuteTopological(const ScriptGraph& graph, ScriptExecutionContext& context,
                                const std::vector<uint32_t>& startNodes);

        /**
     * @brief Get execution order for nodes
     * @param graph Script graph
     * @param startNodes Starting nodes
     * @return Execution order
     */
        std::vector<uint32_t> GetExecutionOrder(const ScriptGraph& graph, const std::vector<uint32_t>& startNodes);

        /**
     * @brief Evaluate socket value
     * @param graph Script graph
     * @param nodeID Node ID
     * @param socketIndex Socket index
     * @param context Execution context
     * @return Socket value
     */
        ScriptValue EvaluateSocket(const ScriptGraph& graph, uint32_t nodeID, uint32_t socketIndex,
                                   ScriptExecutionContext& context);

      private:
        bool m_isDebugging = false;                       ///< Debugging state
        uint32_t m_currentNode = 0;                       ///< Current debugging node
        std::unordered_map<uint32_t, bool> m_breakpoints; ///< Node breakpoints
        std::queue<uint32_t> m_executionQueue;            ///< Execution queue for debugging
        bool m_stepMode = false;                          ///< Step-by-step execution mode
    };

    /**
 * @brief Visual scripting system with Blueprint-style editor
 * 
 * Provides comprehensive visual scripting capabilities including:
 * - Node-based programming interface
 * - Real-time script compilation and execution
 * - Visual debugging with breakpoints
 * - Custom node creation and registration
 * - Component integration and event handling
 * - Performance optimization and profiling
 * - Script serialization and version control
 * 
 * Inspired by Unreal Engine's Blueprint system and Unity's Visual Scripting.
 */
    class VisualScriptingSystem : public EditorPanel
    {
      public:
        /**
     * @brief Constructor
     */
        VisualScriptingSystem();

        /**
     * @brief Destructor
     */
        ~VisualScriptingSystem() override;

        /**
     * @brief Initialize the visual scripting system
     * @return true if initialization succeeded
     */
        bool Initialize() override;

        /**
     * @brief Update visual scripting system
     * @param deltaTime Time elapsed since last update
     */
        void Update(float deltaTime) override;

        /**
     * @brief Render visual scripting UI
     */
        void Render() override;

        /**
     * @brief Shutdown the visual scripting system
     */
        void Shutdown() override;

        /**
     * @brief Handle panel events
     * @param eventType Event type
     * @param eventData Event data
     * @return true if event was handled
     */
        bool HandleEvent(const std::string& eventType, void* eventData) override;

        /**
     * @brief Create new script graph
     * @param name Script name
     * @param targetObject Target object for script
     */
        void CreateNewScript(const std::string& name = "New Script", ObjectID targetObject = INVALID_OBJECT_ID);

        /**
     * @brief Load script from file
     * @param filePath Path to script file
     * @return true if script was loaded successfully
     */
        bool LoadScript(const std::string& filePath);

        /**
     * @brief Save current script to file
     * @param filePath Path to save script
     * @return true if script was saved successfully
     */
        bool SaveScript(const std::string& filePath);

        /**
     * @brief Compile current script
     * @return true if compilation succeeded
     */
        bool CompileScript();

        /**
     * @brief Execute current script
     * @param targetObject Target object for execution
     * @return true if execution succeeded
     */
        bool ExecuteScript(ObjectID targetObject);

        /**
     * @brief Add node to current graph
     * @param nodeType Type of node to add
     * @param position Position in graph space
     * @return ID of created node, or 0 if failed
     */
        uint32_t AddNode(ScriptNodeType nodeType, const XMFLOAT2& position);

        /**
     * @brief Remove node from graph
     * @param nodeID ID of node to remove
     * @return true if node was removed
     */
        bool RemoveNode(uint32_t nodeID);

        /**
     * @brief Connect two sockets
     * @param fromNodeID Source node ID
     * @param fromSocketIndex Source socket index
     * @param toNodeID Target node ID
     * @param toSocketIndex Target socket index
     * @return true if connection was created
     */
        bool ConnectSockets(uint32_t fromNodeID, uint32_t fromSocketIndex, uint32_t toNodeID, uint32_t toSocketIndex);

        /**
     * @brief Start debugging current script
     * @param targetObject Target object for debugging
     */
        void StartDebugging(ObjectID targetObject);

        /**
     * @brief Stop debugging session
     */
        void StopDebugging();

        /**
     * @brief Get current script graph
     * @return Reference to current script graph
     */
        const ScriptGraph& GetCurrentScript() const { return m_currentScript; }

        /**
     * @brief Register custom node type
     * @param nodeType Node type information
     * @param factory Node creation factory
     */
        void RegisterCustomNode(const std::string& typeName, std::function<std::unique_ptr<ScriptNode>()> factory);

      private:
        /**
     * @brief Render script graph editor
     */
        void RenderScriptEditor();

        /**
     * @brief Render node palette
     */
        void RenderNodePalette();

        /**
     * @brief Render script properties
     */
        void RenderScriptProperties();

        /**
     * @brief Render debugging interface
     */
        void RenderDebugInterface();

        /**
     * @brief Render execution log
     */
        void RenderExecutionLog();

        /**
     * @brief Render a single script node
     * @param node Node to render
     */
        void RenderScriptNode(ScriptNode* node);

        /**
     * @brief Render connections between nodes
     */
        void RenderConnections();

        /**
     * @brief Handle node creation from palette
     */
        void HandleNodeCreation();

        /**
     * @brief Handle node dragging
     */
        void HandleNodeDragging();

        /**
     * @brief Handle connection creation
     */
        void HandleConnectionCreation();

        /**
     * @brief Handle node selection
     */
        void HandleNodeSelection();

        /**
     * @brief Create node of specified type
     * @param nodeType Type of node to create
     * @return Unique pointer to created node
     */
        std::unique_ptr<ScriptNode> CreateNode(ScriptNodeType nodeType);

        /**
     * @brief Get node type color
     * @param category Node category
     * @return Header color for node type
     */
        XMFLOAT4 GetNodeCategoryColor(ScriptNodeCategory category) const;

        /**
     * @brief Get socket type color
     * @param type Socket type
     * @return Color for socket type
     */
        XMFLOAT4 GetSocketTypeColor(ScriptVariableType type) const;

        /**
     * @brief Initialize built-in node types
     */
        void InitializeBuiltInNodes();

        /**
     * @brief Update script execution
     */
        void UpdateExecution();

      private:
        // Current script
        ScriptGraph m_currentScript; ///< Current script being edited

        // Execution
        std::unique_ptr<ScriptExecutor> m_executor;                 ///< Script execution engine
        std::unique_ptr<ScriptExecutionContext> m_executionContext; ///< Execution context

        // Editor state
        bool m_isDraggingNode = false;       ///< Currently dragging a node
        bool m_isCreatingConnection = false; ///< Currently creating a connection
        uint32_t m_draggedNodeID = 0;        ///< ID of node being dragged
        XMFLOAT2 m_dragOffset = {0, 0};      ///< Drag offset from node origin

        // Connection creation state
        uint32_t m_connectionStartNodeID = 0;  ///< Connection start node
        uint32_t m_connectionStartSocket = 0;  ///< Connection start socket
        bool m_connectionStartIsInput = false; ///< Whether start socket is input

        // Selection
        std::vector<uint32_t> m_selectedNodes; ///< Selected node IDs

        // View state
        XMFLOAT2 m_graphViewOffset = {0, 0}; ///< Graph view offset
        float m_graphViewScale = 1.0f;       ///< Graph view scale

        // UI layout
        float m_nodeListWidth = 250.0f;   ///< Node palette width
        float m_propertiesWidth = 300.0f; ///< Properties panel width
        float m_debugHeight = 200.0f;     ///< Debug panel height

        // Node types registry
        std::unordered_map<ScriptNodeType, std::function<std::unique_ptr<ScriptNode>()>> m_nodeFactories;
        std::unordered_map<std::string, std::vector<ScriptNodeType>> m_nodeCategories;

        // Custom nodes
        std::unordered_map<std::string, std::function<std::unique_ptr<ScriptNode>()>> m_customNodes;

        // Execution state
        bool m_isExecuting = false;              ///< Whether script is executing
        bool m_isDebugging = false;              ///< Whether debugging is active
        std::vector<std::string> m_executionLog; ///< Execution log messages

        // Performance monitoring
        float m_lastExecutionTime = 0.0f; ///< Last execution time in ms
        int m_executedNodesCount = 0;     ///< Number of nodes executed
    };

} // namespace SparkEditor