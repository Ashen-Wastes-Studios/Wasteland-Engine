// PCH, then the interface this file implements. The header is the shared
// one in Utils/, not a Mac-local header -- there is no
// MacPlatformUtils.h, so all three platform backends implement the same
// declaration from PlatformUtils.h.
#include "wlpch.h"
#include "Wasteland/Utils/PlatformUtils.h"

// Every line of the implementation is inside this guard, so on Windows or
// Linux this translation unit compiles to nothing at all. That is how the
// per-platform files avoid needing their own entries in the build system's
// platform filter.
#if defined(WL_PLATFORM_MACOS)

// popen/fgets/pclose and std::string. No Objective-C, no Cocoa: the whole
// implementation shells out to osascript.
#include <cstdio>
#include <string>

namespace Wasteland
{

        // Runs a shell command and captures its stdout.
        // ---
        // This is the file's one piece of real logic; both public functions below
        // are a command string and a call to this.
        // ---
        // DEFECT (documented, not fixed): the return value is the child's exit
        // STATUS, which is discarded. A command that printed nothing because it
        // CRASHED is indistinguishable from a user who cancelled the dialog --
        // and cancel is exactly the empty string both callers treat as "cancelled".
        // Every failure mode therefore surfaces as a silent cancel.
    static std::string RunCommand(const std::string &command)
    {
        std::string result;
                // popen, not system: it needs to read stdout back.
                // ---
                // Security note, stated because it is real: the command string is
                // interpolated into a shell command line. Neither caller interpolates
                // anything into it, so there is no injection today -- but the helper has
                // no quoting of its own, so a future caller that added a filename would
                // need to escape it first.
        FILE *pipe = popen(command.c_str(), "r");
                // A null pipe means popen could not start the child. Returning the empty
                // string makes this the same "user cancelled" answer, which is the
                // behaviour the callers expect from a failed dialog.
        if (!pipe)
            return result;

                // Fixed 256-byte read buffer. fgets stops at a newline or when the buffer
                // is full, so a long line is delivered in several chunks and reassembled
                // correctly by the concatenation below.
        char buffer[256];
                // Reads to end of file. `!= nullptr` is the loop condition, so EOF is the
                // only exit -- a read error is also reported as a null return and is
                // therefore indistinguishable from EOF here.
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
        {
                        // Appending into one std::string rather than collecting lines: the caller
                        // wants a path, not a line list, and a POSIX path cannot contain a newline.
            result += buffer;
        }

                // Reaps the child. The return value (exit status) is discarded -- see the
                // defect note on RunCommand.
                // ---
                // DEFECT (documented, not fixed): the return value is also not checked
                // for a failed pclose, so a child that never ran cleanly leaves nothing
                // observable.
        pclose(pipe);
                // Trailing-newline trim, so the caller gets a bare path with no need to
                // strip it. osascript ends its output with one newline.
        if (!result.empty() && result.back() == '\n')
            result.pop_back();

        return result;
    }

        // OpenFile, per the PlatformUtils.h contract: empty string means the user
        // cancelled.
        // ---
        // DEFECT (documented, not fixed): the `filter` PARAMETER IS IGNORED. The
        // signature takes a filter description -- and the Windows implementation
        // honours the same parameter -- but the osascript command below is a fixed
        // string with no reference to it. Every caller in the tree passes a real
        // filter ("Wasteland Scene (*.wastescene)\0*.wastescene\0"), so on macOS
        // the dialog offers ALL files: the picker a user sees does not match the
        // one they were promised, and selecting the wrong file is only caught
        // later by the parser.
    std::string FileDialogs::OpenFile(const char *filter)
    {
                // The whole dialog, as one osascript invocation split into -e clauses.
                // ---
                // The AppleScript: `try` ... `on error` ... `end try` is what turns a
                // cancelled panel into a caught AppleScript error, which is why the error
                // branch returns "" rather than propagating anything.
                // ---
                // `POSIX path of` converts the HFS path AppleScript would otherwise
                // return (colon-separated, e.g. "Macintosh HD:Users:...") into the slash
                // form the rest of the engine expects.
                // ---
                // The escaped double quotes inside a single-quoted shell string are the
                // one non-obvious bit of quoting here: the outer shell quotes are single,
                // so the AppleScript string delimiters have to be backslash-escaped to
                // survive the shell.
        std::string command = "osascript -e 'try' -e 'set f to POSIX path of (choose file with prompt \"Open File\")' -e 'return f' -e 'on error' -e 'return \"\"' -e 'end try'";
        return RunCommand(command);
    }

        // SaveFile. Identical structure to OpenFile above, differing only in the
        // AppleScript verb (`choose file name` rather than `choose file`) and the
        // prompt text.
    std::string FileDialogs::SaveFile(const char *filter)
    {
        std::string command = "osascript -e 'try' -e 'set f to POSIX path of (choose file name with prompt \"Save File\")' -e 'return f' -e 'on error' -e 'return \"\"' -e 'end try'";
        return RunCommand(command);
    }

}

// Closes the platform guard. The `}` on line 44 above is NOT labelled as
// the namespace close it is -- the namespace opened on line 9 has no
// matching comment here, unlike most files in the engine. Harmless, but a
// reader scanning for `} // namespace` finds nothing to find.
#endif // WL_PLATFORM_MACOS
