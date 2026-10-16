#pragma once

#include "Animation/AnimationData.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Engine
{
    class AnimationClip;
    class Model;

    enum class PlaybackMode
    {
        Loop,
        Clamp
    };

    class Animator
    {
    public:
        bool SetModel(std::shared_ptr<const Model> model, std::string& error);
        bool SetClip(std::shared_ptr<const AnimationClip> clip, PlaybackMode mode,
            std::string& error);
        bool Advance(double dt, std::string& error);
        double TimeSeconds() const noexcept;
        std::shared_ptr<const AnimationPose> Pose() const noexcept;
        void Reset() noexcept;

    private:
        std::shared_ptr<const Model> _model;
        std::shared_ptr<const AnimationClip> _clip;
        std::shared_ptr<const AnimationPose> _pose;
        std::vector<uint32_t> _nodeOrder;
        PlaybackMode _mode = PlaybackMode::Loop;
        double _timeSeconds = 0.0;
    };
}
