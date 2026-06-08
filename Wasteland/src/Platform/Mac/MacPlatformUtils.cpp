#include "wlpch.h"
#include "Wasteland/Utils/PlatformUtils.h"

#if defined(WL_PLATFORM_MACOS)

#include <cstdio>
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
        std::string command = "osascript -e 'try' -e 'set f to POSIX path of (choose file with prompt \"Open File\")' -e 'return f' -e 'on error' -e 'return \"\"' -e 'end try'";
        return RunCommand(command);
    }

    std::string FileDialogs::SaveFile(const char *filter)
    {
        std::string command = "osascript -e 'try' -e 'set f to POSIX path of (choose file name with prompt \"Save File\")' -e 'return f' -e 'on error' -e 'return \"\"' -e 'end try'";
        return RunCommand(command);
    }

}

#endif // WL_PLATFORM_MACOS
