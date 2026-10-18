// PCH first, by house rule. The body is behind WL_PLATFORM_LINUX, yet it still
// includes the shared engine PCH, as every platform translation unit does.
#include "wlpch.h"
// The declaration being implemented: FileDialogs with two static string-returning
// methods, documented in the header as returning "" when cancelled. Both take a
// `const char* filter` that is never forwarded to zenity.
#include "Wasteland/Utils/PlatformUtils.h"

// The platform gate. On a non-Linux build this file compiles to nothing, so
// OpenFile/SaveFile have NO definition on Windows or macOS -- any call site
// outside a Linux guard is a link error, not a graceful failure. Worth knowing
// when porting a panel that opens a file dialog.
#if defined(WL_PLATFORM_LINUX)

#include <cstdio>
#include <memory>
#include <string>

namespace Wasteland
{

    // File-local rather than namespace-scope: internal linkage keeps RunCommand out
    // of every symbol table, so no header declares it and nothing else can call it.
    //
    // Runs a shell command and captures stdout as text. popen hands the string to
    // /bin/sh, so anything interpolated into it would be a shell injection -- see
    // the two callers below, which build a constant string and nothing more.
    static std::string RunCommand(const std::string &command)
    {
        std::string result;
        // popen, not system: it yields a FILE* to read, which is the only reason this
        // helper exists. Mode "r" reads the child's stdout.
        //
        // DEFECT (blocking, no timeout): popen blocks until the child exits and cannot
        // be cancelled. zenity shows a modal dialog, so this blocks the calling thread
        // for as long as the user takes to answer -- a frozen window on the main thread,
        // with no timeout and no way to abort short of killing the thread.
        FILE *pipe = popen(command.c_str(), "r");
        // Empty-string-on-failure, and it is load-bearing: PlatformUtils.h documents ""
        // for a cancelled dialog, so a failed popen is indistinguishable from a cancel.
        // Desirable here, but it means a missing zenity and a user pressing Cancel give
        // identical results.
        if (!pipe)
            return result;

        // A stack buffer with fgets rather than getline into a string: 256 bytes per
        // iteration, so output longer than one read simply takes more iterations and
        // nothing is truncated.
        //
        // DEFECT (embedded NULs): fgets stops at a newline but not at an embedded NUL,
        // so a filename containing one would truncate the captured string silently. Not
        // reachable through zenity's normal output, so recorded rather than treated as
        // a live bug.
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
        {
            // operator+= rather than a stream, so there is no per-iteration locale or
            // sentry cost. The buffer is reused across iterations, which is what makes
            // append the cheaper choice here.
            result += buffer;
        }

        // DEFECT (ignored return): pclose returns the child's exit status, and it is
        // discarded. A zenity run that failed -- no display, wrong DISPLAY, zenity absent
        // from PATH -- still returns whatever partial stdout was captured and is
        // indistinguishable from success. Not fixed here; comments only.
        pclose(pipe);
        // The trailing-newline trim. Without it every returned path would carry a
        // newline and a caller opening it would fail naming a file that does not exist --
        // a genuinely confusing failure, so this line earns its two lines.
        if (!result.empty() && result.back() == '\n')
            result.pop_back();

        return result;
    }

    // OpenFile. DEFECT (parameter ignored): `filter` is accepted and dropped, so
    // there is no dialog extension filter and a caller asking for ".png" gets the
    // unfiltered chooser. The parameter exists for signature symmetry with the
    // Windows implementation.
    //
    // The command is built as a std::string but never interpolated -- no injection
    // surface today. It is string-typed regardless, which is what would matter if a
    // path were ever concatenated in.
    std::string FileDialogs::OpenFile(const char *filter)
    {
        std::string command = "zenity --file-selection --title=\"Open File\"";
        return RunCommand(command);
    }

    std::string FileDialogs::SaveFile(const char *filter)
    {
        // SaveFile: --save and --confirm-overwrite give a save dialog that confirms
        // clobbering. The empty-string-on-cancel convention comes from RunCommand, not
        // from anything here.
        std::string command = "zenity --file-selection --save --confirm-overwrite --title=\"Save File\"";
        return RunCommand(command);
    }

}

#endif // WL_PLATFORM_LINUX
