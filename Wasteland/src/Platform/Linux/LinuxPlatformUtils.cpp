#include "wlpch.h"
#include "Wasteland/Utils/PlatformUtils.h"

#if defined(WL_PLATFORM_LINUX)

#include <cstdio>
#include <memory>
#include <string>

namespace Wasteland
{

    static std::string RunCommand(const std::string &command)
    {
        std::string result;
        FILE *pipe = popen(command.c_str(), "r");
        if (!pipe)
            return result;

        char buffer[256];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
        {
            result += buffer;
        }

        pclose(pipe);
        if (!result.empty() && result.back() == '\n')
            result.pop_back();

        return result;
    }

    std::string FileDialogs::OpenFile(const char *filter)
    {
        std::string command = "zenity --file-selection --title=\"Open File\"";
        return RunCommand(command);
    }

    std::string FileDialogs::SaveFile(const char *filter)
    {
        std::string command = "zenity --file-selection --save --confirm-overwrite --title=\"Save File\"";
        return RunCommand(command);
    }

}

#endif // WL_PLATFORM_LINUX
