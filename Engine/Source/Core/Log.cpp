#include "Core/Log.h"

#include <Windows.h>

#include <cstdio>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>

namespace
{
    std::ofstream gFile;
    std::mutex gMutex;
    bool gFileErrorReported = false;

    void DebugOutput(const std::string& line)
    {
        if (line.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max()))
        {
            const int length = static_cast<int>(line.size());
            const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                line.data(), length, nullptr, 0);
            if (count > 0)
            {
                std::wstring wide(static_cast<std::size_t>(count), L'\0');
                if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                    line.data(), length, wide.data(), count) == count)
                {
                    OutputDebugStringW(wide.c_str());
                    return;
                }
            }
        }

        OutputDebugStringA(line.c_str());
    }

    void Emit(const std::string& line)
    {
        DebugOutput(line);
        std::fwrite(line.data(), 1, line.size(), stderr);
        std::fflush(stderr);
    }
}

namespace Engine::Log
{
    bool Open(const std::filesystem::path& file)
    {
        std::lock_guard lock(gMutex);
        if (file.empty())
        {
            Emit("Log::Open rejected an empty path.\n");
            return false;
        }

        std::error_code error;
        const std::filesystem::path parent = file.parent_path();
        if (!parent.empty())
        {
            std::filesystem::create_directories(parent, error);
            if (error)
            {
                Emit("Log directory creation failed: " + error.message() + "\n");
                return false;
            }
        }

        std::ofstream candidate(file, std::ios::out | std::ios::app | std::ios::binary);
        if (!candidate.is_open())
        {
            Emit("Log file open failed.\n");
            return false;
        }

        gFile = std::move(candidate);
        gFileErrorReported = false;
        return true;
    }

    void Write(std::string_view message)
    {
        std::lock_guard lock(gMutex);
        std::string line(message);
        if (line.empty() || line.back() != '\n')
        {
            line.push_back('\n');
        }
        Emit(line);

        if (gFile.is_open())
        {
            gFile.write(line.data(), static_cast<std::streamsize>(line.size()));
            gFile.flush();
            if (!gFile && !gFileErrorReported)
            {
                Emit("Log file write failed; debugger and stderr output remain active.\n");
                gFileErrorReported = true;
            }
        }
    }

    void Error(std::string_view message, long code)
    {
        std::ostringstream text;
        text << message << " [code=0x" << std::uppercase << std::hex
            << std::setw(8) << std::setfill('0') << static_cast<unsigned long>(code) << ']';
        Write(text.str());
    }

    void Close()
    {
        std::lock_guard lock(gMutex);
        if (gFile.is_open())
        {
            gFile.close();
        }
        gFileErrorReported = false;
    }
}
