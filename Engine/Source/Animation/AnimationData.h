#pragma once

#include "Types/Math.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Engine
{
    struct VectorKey
    {
        double timeSeconds = 0.0;
        Vector3 value = {};
    };

    struct RotationKey
    {
        double timeSeconds = 0.0;
        Quaternion value = {0.0f, 0.0f, 0.0f, 1.0f};
    };

    struct TRSChannel
    {
        uint32_t nodeIndex = 0;
        std::vector<VectorKey> translations;
        std::vector<VectorKey> scales;
        std::vector<RotationKey> rotations;
    };

    struct ClipData
    {
        std::string name;
        std::string sourceTake;
        std::string skeletonSignature;
        uint32_t nodeCount = 0;
        double durationSeconds = 0.0;
        std::vector<TRSChannel> channels;
    };

    struct AnimationPose
    {
        std::string skeletonSignature;
        std::vector<Matrix> nodeWorld;
        // A skinned palette already transforms mesh-local vertices to model-root space.
        std::vector<std::vector<Matrix>> meshPalettes;
    };
}
