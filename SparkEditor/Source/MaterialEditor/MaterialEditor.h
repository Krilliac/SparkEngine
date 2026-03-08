/**
 * @file MaterialEditor.h
 * @brief Visual material and shader editor for the Spark Engine Editor
 * @author Spark Engine Team
 * @date 2025
 * 
 * This file implements a professional node-based material editor similar to
 * Unreal Engine's Material Editor and Blender's Shader Editor, allowing
 * visual creation and editing of shaders and materials.
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <DirectXMath.h>
using namespace DirectX;
#include <d3d11.h>
#include <wrl/client.h>
#include <vector>
#include <memory>
#include <unordered_map>
#include <string>
#include <functional>


namespace SparkEditor
{

    /**
 * @brief Node types in the material graph
 */
    enum class MaterialNodeType
    {
        // Input nodes
        TEXTURE_SAMPLE = 0,
        CONSTANT_FLOAT = 1,
        CONSTANT_VECTOR2 = 2,
        CONSTANT_VECTOR3 = 3,
        CONSTANT_VECTOR4 = 4,
        CONSTANT_COLOR = 5,
        TIME = 6,
        UV_COORDINATES = 7,
        WORLD_POSITION = 8,
        WORLD_NORMAL = 9,
        CAMERA_VECTOR = 10,

        // Math nodes
        ADD = 50,
        SUBTRACT = 51,
        MULTIPLY = 52,
        DIVIDE = 53,
        DOT_PRODUCT = 54,
        CROSS_PRODUCT = 55,
        NORMALIZE = 56,
        LENGTH = 57,
        DISTANCE = 58,
        POWER = 59,
        SQRT = 60,
        SIN = 61,
        COS = 62,
        TAN = 63,
        LERP = 64,
        CLAMP = 65,
        SATURATE = 66,

        // Utility nodes
        FRESNEL = 100,
        NOISE = 101,
        VORONOI = 102,
        GRADIENT = 103,
        REMAP = 104,
        SPLIT_VECTOR = 105,
        COMBINE_VECTOR = 106,
        MASK = 107,
        IF = 108,
        SWITCH = 109,

        // Output nodes
        SURFACE_OUTPUT = 200,
        UNLIT_OUTPUT = 201,

        // Custom nodes
        CUSTOM = 1000
    };

    /**
 * @brief Socket types for node connections
 */
    enum class SocketType
    {
        FLOAT = 0,
        VECTOR2 = 1,
        VECTOR3 = 2,
        VECTOR4 = 3,
        COLOR = 4,
        TEXTURE = 5,
        BOOLEAN = 6,
        EXEC = 7 ///< Execution flow (for future use)
    };

    /**
 * @brief Socket direction
 */
    enum class SocketDirection
    {
        INPUT = 0,
        OUTPUT = 1
    };

    /**
 * @brief Material node socket
 */
    struct MaterialSocket
    {
        std::string name;                     ///< Socket display name
        SocketType type;                      ///< Socket data type
        SocketDirection direction;            ///< Input or output
        XMFLOAT4 defaultValue = {0, 0, 0, 0}; ///< Default value for input sockets
        bool isConnected = false;             ///< Whether socket is connected
        std::string description;              ///< Socket description/tooltip
        bool isRequired = false;              ///< Whether input is required

        // Visual properties
        XMFLOAT2 position = {0, 0};    ///< Socket position relative to node
        float radius = 6.0f;           ///< Socket visual radius
        XMFLOAT4 color = {1, 1, 1, 1}; ///< Socket color
    };

    /**
 * @brief Connection between sockets
 */
    struct MaterialConnection
    {
        uint32_t fromNodeID;      ///< Source node ID
        uint32_t fromSocketIndex; ///< Source socket index
        uint32_t toNodeID;        ///< Target node ID
        uint32_t toSocketIndex;   ///< Target socket index

        // Visual properties
        XMFLOAT4 color = {1, 1, 1, 1}; ///< Connection line color
        float thickness = 2.0f;        ///< Connection line thickness
        bool isSelected = false;       ///< Whether connection is selected
    };

    /**
 * @brief Material graph node
 */
    struct MaterialNode
    {
        uint32_t id;           ///< Unique node ID
        MaterialNodeType type; ///< Node type
        std::string name;      ///< Node display name
        std::string category;  ///< Node category

        // Position and size
        XMFLOAT2 position = {0, 0}; ///< Node position in graph
        XMFLOAT2 size = {120, 80};  ///< Node size

        // Sockets
        std::vector<MaterialSocket> inputSockets;  ///< Input sockets
        std::vector<MaterialSocket> outputSockets; ///< Output sockets

        // Properties
        std::unordered_map<std::string, std::string> properties; ///< Node-specific properties
        std::string code;                                        ///< Generated shader code
        std::string description;                                 ///< Node description

        // Visual state
        bool isSelected = false;                             ///< Whether node is selected
        bool isCommentNode = false;                          ///< Whether this is a comment node
        XMFLOAT4 backgroundColor = {0.2f, 0.2f, 0.2f, 1.0f}; ///< Node background color
        XMFLOAT4 titleColor = {0.8f, 0.8f, 0.8f, 1.0f};      ///< Title bar color

        // Preview
        bool hasPreview = false;         ///< Whether node has preview
        uint32_t previewTextureID = 0;   ///< Preview texture ID
        XMFLOAT2 previewSize = {64, 64}; ///< Preview size
    };

    /**
 * @brief Material graph
 */
    struct MaterialGraph
    {
        std::vector<std::unique_ptr<MaterialNode>> nodes; ///< All nodes in graph
        std::vector<MaterialConnection> connections;      ///< All connections
        uint32_t nextNodeID = 1;                          ///< Next available node ID

        // Graph properties
        std::string name = "New Material"; ///< Material name
        std::string description;           ///< Material description
        XMFLOAT2 viewOffset = {0, 0};      ///< Graph view offset
        float viewScale = 1.0f;            ///< Graph view scale

        // Output nodes
        uint32_t surfaceOutputNodeID = 0; ///< Surface output node ID
        uint32_t unlitOutputNodeID = 0;   ///< Unlit output node ID

        // Compilation results
        std::string vertexShaderCode;               ///< Generated vertex shader
        std::string pixelShaderCode;                ///< Generated pixel shader
        std::vector<std::string> compilationErrors; ///< Compilation error messages
        bool isCompiled = false;                    ///< Whether material is compiled
    };

    /**
 * @brief Material preview sphere
 */
    struct MaterialPreview
    {
        enum Shape
        {
            SPHERE = 0,
            CUBE = 1,
            PLANE = 2,
            CYLINDER = 3,
            CUSTOM_MESH = 4
        };

        Shape previewShape = SPHERE;                          ///< Preview shape
        std::string customMeshPath;                           ///< Custom mesh path
        XMFLOAT3 lightDirection = {0.5f, -0.5f, 0.5f};        ///< Preview light direction
        XMFLOAT4 lightColor = {1, 1, 1, 1};                   ///< Preview light color
        float lightIntensity = 1.0f;                          ///< Preview light intensity
        XMFLOAT4 environmentColor = {0.2f, 0.2f, 0.3f, 1.0f}; ///< Environment color
        bool showGrid = true;                                 ///< Show background grid
        bool autoRotate = true;                               ///< Auto-rotate preview
        float rotationSpeed = 0.5f;                           ///< Rotation speed
    };

    /**
 * @brief Professional material and shader editor
 * 
 * Provides a comprehensive node-based material editing system with:
 * - Visual node graph editing
 * - Real-time material preview
 * - Automatic shader generation
 * - Multiple output types (Surface, Unlit, etc.)
 * - Built-in node library
 * - Custom node creation
 * - Material properties panel
 * - Texture management
 * - Performance optimization
 * - Import/export functionality
 * 
 * Inspired by Unreal Engine's Material Editor and Blender's Shader Editor.
 */
    class MaterialEditor : public EditorPanel
    {
      public:
        /**
     * @brief Constructor
     */
        MaterialEditor();

        /**
     * @brief Destructor
     */
        ~MaterialEditor() override;

        /**
     * @brief Initialize the material editor
     * @return true if initialization succeeded
     */
        bool Initialize() override;

        /**
     * @brief Update material editor
     * @param deltaTime Time elapsed since last update
     */
        void Update(float deltaTime) override;

        /**
     * @brief Render material editor UI
     */
        void Render() override;

        /**
     * @brief Shutdown the material editor
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
     * @brief Create new material
     * @param materialName Name for the new material
     */
        void CreateNewMaterial(const std::string& materialName = "New Material");

        /**
     * @brief Load material from file
     * @param filePath Path to material file
     * @return true if material was loaded successfully
     */
        bool LoadMaterial(const std::string& filePath);

        /**
     * @brief Save current material to file
     * @param filePath Path to save material
     * @return true if material was saved successfully
     */
        bool SaveMaterial(const std::string& filePath);

        /**
     * @brief Compile current material to shaders
     * @return true if compilation succeeded
     */
        bool CompileMaterial();

        /**
     * @brief Add node to the graph
     * @param nodeType Type of node to add
     * @param position Position in graph space
     * @return ID of created node, or 0 if failed
     */
        uint32_t AddNode(MaterialNodeType nodeType, const XMFLOAT2& position);

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
     * @brief Disconnect sockets
     * @param toNodeID Target node ID
     * @param toSocketIndex Target socket index
     * @return true if connection was removed
     */
        bool DisconnectSocket(uint32_t toNodeID, uint32_t toSocketIndex);

        /**
     * @brief Get current material graph
     * @return Reference to current material graph
     */
        const MaterialGraph& GetMaterialGraph() const { return m_materialGraph; }

        /**
     * @brief Set material preview shape
     * @param shape Preview shape to use
     */
        void SetPreviewShape(MaterialPreview::Shape shape);

        /**
     * @brief Get material preview settings
     * @return Reference to preview settings
     */
        const MaterialPreview& GetPreviewSettings() const { return m_previewSettings; }

      private:
        /**
     * @brief Render graph editor area
     */
        void RenderGraphEditor();

        /**
     * @brief Render node palette
     */
        void RenderNodePalette();

        /**
     * @brief Render material properties
     */
        void RenderMaterialProperties();

        /**
     * @brief Render material preview
     */
        void RenderMaterialPreview();

        /**
     * @brief Render compilation output
     */
        void RenderCompilationOutput();

        /**
     * @brief Render a single node
     * @param node Node to render
     */
        void RenderNode(MaterialNode* node);

        /**
     * @brief Render node sockets
     * @param node Node containing sockets
     */
        void RenderNodeSockets(MaterialNode* node);

        /**
     * @brief Render connections between nodes
     */
        void RenderConnections();

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
     * @brief Update node previews
     */
        void UpdateNodePreviews();

        /**
     * @brief Create node of specified type
     * @param nodeType Type of node to create
     * @return Unique pointer to created node
     */
        std::unique_ptr<MaterialNode> CreateNode(MaterialNodeType nodeType);

        /**
     * @brief Get node type information
     * @param nodeType Node type
     * @return Node type information
     */
        struct NodeTypeInfo
        {
            std::string name;
            std::string category;
            std::string description;
            std::vector<MaterialSocket> inputSockets;
            std::vector<MaterialSocket> outputSockets;
            XMFLOAT4 headerColor;
            bool hasPreview;
        };
        const NodeTypeInfo& GetNodeTypeInfo(MaterialNodeType nodeType);

        /**
     * @brief Generate shader code for the material
     * @param outVertexShader Output vertex shader code
     * @param outPixelShader Output pixel shader code
     * @return true if generation succeeded
     */
        bool GenerateShaderCode(std::string& outVertexShader, std::string& outPixelShader);

        /**
     * @brief Validate material graph
     * @param outErrors Output error messages
     * @return true if graph is valid
     */
        bool ValidateMaterialGraph(std::vector<std::string>& outErrors);

        /**
     * @brief Convert screen coordinates to graph coordinates
     * @param screenPos Screen position
     * @return Graph position
     */
        XMFLOAT2 ScreenToGraph(const XMFLOAT2& screenPos) const;

        /**
     * @brief Convert graph coordinates to screen coordinates
     * @param graphPos Graph position
     * @return Screen position
     */
        XMFLOAT2 GraphToScreen(const XMFLOAT2& graphPos) const;

        /**
     * @brief Find node at position
     * @param position Position in graph space
     * @return Pointer to node, or nullptr if none found
     */
        MaterialNode* FindNodeAtPosition(const XMFLOAT2& position);

        /**
     * @brief Find socket at position
     * @param position Position in screen space
     * @param outNode Output node containing socket
     * @param outSocketIndex Output socket index
     * @param outIsInput Output whether socket is input
     * @return true if socket was found
     */
        bool FindSocketAtPosition(const XMFLOAT2& position, MaterialNode*& outNode, uint32_t& outSocketIndex,
                                  bool& outIsInput);

        /**
     * @brief Initialize default node types
     */
        void InitializeNodeTypes();

        /**
     * @brief Setup material preview rendering
     * @return true if setup succeeded
     */
        bool SetupPreviewRendering();

        /**
     * @brief Render material preview to texture
     */
        void RenderPreviewToTexture();

      private:
        // Material graph
        MaterialGraph m_materialGraph; ///< Current material graph

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
        XMFLOAT2 m_graphPanStart = {0, 0};   ///< Pan operation start position
        bool m_isPanning = false;            ///< Currently panning view

        // UI layout
        float m_nodeListWidth = 200.0f;   ///< Node palette width
        float m_propertiesWidth = 250.0f; ///< Properties panel width
        float m_previewHeight = 200.0f;   ///< Preview panel height

        // Preview settings
        MaterialPreview m_previewSettings; ///< Material preview settings
        uint32_t m_previewTextureID = 0;   ///< Preview texture ID

        // Node type database
        std::unordered_map<MaterialNodeType, NodeTypeInfo> m_nodeTypeInfo;               ///< Node type information
        std::unordered_map<std::string, std::vector<MaterialNodeType>> m_nodeCategories; ///< Nodes by category

        // Rendering resources (for preview)
        Microsoft::WRL::ComPtr<ID3D11Device> m_device;                 ///< DirectX device
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;         ///< DirectX context
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_previewTexture;      ///< Preview render target
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_previewRTV;   ///< Preview RTV
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_previewSRV; ///< Preview SRV

        // Grid and background
        bool m_showGrid = true;                                ///< Show background grid
        float m_gridSize = 50.0f;                              ///< Grid cell size
        XMFLOAT4 m_gridColor = {0.3f, 0.3f, 0.3f, 0.5f};       ///< Grid line color
        XMFLOAT4 m_backgroundColor = {0.1f, 0.1f, 0.1f, 1.0f}; ///< Graph background color
    };

} // namespace SparkEditor