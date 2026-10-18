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
        // Zero leaves foreground rendering uncapped.
        uint32_t maxFramesPerSecond = 0;
        // Zero preserves the legacy Sleep(8) inactive backoff.
        uint32_t backgroundFramesPerSecond = 0;
        bool vsync = false;
        // Zero leaves the DXGI queue limit unchanged.
        uint32_t maximumFrameLatency = 0;
    };
}
