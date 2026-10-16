#pragma once

#include "Types/Math.h"

namespace Engine
{
    struct CameraData
    {
        Vector3 position = {0.0f, 0.0f, 0.0f};
        Matrix view = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f};
        Matrix projection = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f};
    };

    class Camera
    {
    public:
        bool SetView(Vector3 eye, Vector3 target) noexcept;
        bool SetPerspective(float fovYRadians, float aspect, float nearZ, float farZ) noexcept;
        CameraData Snapshot() const noexcept;

    private:
        CameraData _data;
    };
}
