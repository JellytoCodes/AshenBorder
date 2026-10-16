#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace Engine
{
    struct ApplicationDesc
    {
        std::wstring title;
        uint32_t width = 1280;
        uint32_t height = 720;
        std::filesystem::path resourceRoot;
        std::filesystem::path shaderRoot;
        std::filesystem::path logFile;
        double fixedSeconds = 1.0 / 60.0;
        uint32_t maxCatchUp = 4;
        bool visible = true;
    };
}
