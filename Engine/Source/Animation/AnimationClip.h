#pragma once

#include "Animation/AnimationData.h"
#include "Graphics/ModelData.h"

#include <span>
#include <string>

namespace Engine
{
    class AnimationClip
    {
    public:
        bool Initialize(const ClipData& data, std::string& error);
        bool Sample(double timeSeconds, std::span<const NodeData> nodes,
            std::span<Matrix> out, std::string& error) const;
        const ClipData& Data() const noexcept;

    private:
        ClipData _data;
        bool _initialized = false;
    };
}
