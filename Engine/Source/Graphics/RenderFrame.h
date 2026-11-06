#pragma once

#include "Graphics/Camera.h"
#include "Effects/ParticleData.h"
#include "Types/Math.h"
#include "UI/UiData.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace Engine
{
    class Model;
    struct AnimationPose;

    struct LightData
    {
        Vector3 direction = {0.4f, -1.0f, 0.5f};
        float ambient = 0.28f;
        Vector3 color = {1.0f, 0.96f, 0.9f};
        float intensity = 0.8f;
        float specularIntensity = 0.0f;
        float specularPower = 32.0f;
    };

    struct ShadowData
    {
        bool enabled = false;
        Matrix viewProjection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        float depthBias = 0.001f;
        float normalBias = 0.01f;
        uint32_t pcfRadius = 1;
    };

    struct DrawItem
    {
        std::shared_ptr<const Model> model;
        Matrix world = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        Vector4 tint = {1, 1, 1, 1};
        bool wireframe = false;
        std::shared_ptr<const AnimationPose> pose;
        float fadeCoverage = 1.0f;
        bool castsShadow = true;
        bool receivesShadow = true;
        float alphaCutoff = 0.0f;
    };

    struct RenderFrame
    {
        std::array<float, 4> clearColor = {0.02f, 0.03f, 0.04f, 1.0f};
        CameraData camera;
        LightData light;
        std::vector<DrawItem> draws;
        std::vector<ParticleDraw> particles;
        ShadowData shadow;
        bool instancingEnabled = true;
        UiFrame ui;
        bool frustumCullingEnabled = false;

        void Clear()
        {
            clearColor = {0.02f, 0.03f, 0.04f, 1.0f};
            camera = CameraData{};
            light = LightData{};
            draws.clear();
            particles.clear();
            shadow = ShadowData{};
            instancingEnabled = true;
            frustumCullingEnabled = false;
            ui.rects.clear();
            ui.texts.clear();
            ui.triangles.clear();
        }
    };
}
