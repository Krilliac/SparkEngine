/**
 * @file ModelObject.h
 * @brief GameObject that renders .obj model files
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

#include "Game/GameObject.h"
#include "Game/Model.h"
#include <memory>
#include <string>

class GraphicsEngine;

/**
 * @brief GameObject that can load and render .obj model files
 */
class ModelObject : public GameObject
{
public:
    /**
     * @brief Constructor with model file path
     * @param modelPath Path to the .obj file to load
     */
    ModelObject(const std::wstring& modelPath);

    /**
     * @brief Destructor
     */
    ~ModelObject() override = default;

    /**
     * @brief Initialize the model object
     * @param device DirectX device for resource creation
     * @param context DirectX device context for rendering
     * @return HRESULT indicating success or failure
     */
    HRESULT Initialize(ID3D11Device* device, ID3D11DeviceContext* context) override;

    /**
     * @brief Render the model object
     * @param view View transformation matrix
     * @param proj Projection transformation matrix
     */
    void Render(const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj) override;

    /**
     * @brief Update the model object
     * @param deltaTime Time elapsed since last update
     */
    void Update(float deltaTime) override;

    /**
     * @brief Collision callback when this object hits another game object
     * @param target Pointer to the game object that was hit
     */
    void OnHit(GameObject* target) override;

    /**
     * @brief Collision callback when this object hits world geometry
     * @param hitPoint World position where the collision occurred
     * @param normal Surface normal at the collision point
     */
    void OnHitWorld(const DirectX::XMFLOAT3& hitPoint, const DirectX::XMFLOAT3& normal) override;

    /** @brief Set graphics engine for rendering */
    void SetGraphicsEngine(GraphicsEngine* gfx) { m_graphics = gfx; }

protected:
    GraphicsEngine* m_graphics{ nullptr }; ///< Graphics engine for rendering
    std::wstring m_modelPath;              ///< Path to the model file
    std::unique_ptr<Model> m_model;        ///< The loaded model
};
