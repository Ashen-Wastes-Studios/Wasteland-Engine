// CrashHandler.h -- unhandled-exception backtrace printer.
//
// WHY THIS EXISTS
// ---------------
// Backend bring-up has produced several faults (access violations inside
// Resize, inside ImGui draws, inside a present) on a machine where no debugger
// is installed. Guessing from log tails costs a full 3-5 minute rebuild-and-run
// cycle per hypothesis. This installs an unhandled-exception filter that prints
// the faulting address, module and the first frames of the stack WITH SYMBOL
// NAMES (dbghelp resolves against the .pdb sitting next to the .exe), so one
// run identifies the function instead of bisecting by instrumentation.
//
// Overhead when nothing crashes: one SetUnhandledExceptionFilter call.
//
// Symbolisation depends on the .pdb being discoverable. Release builds here do
// emit one next to the binary; when it is missing the frames are still printed
// as module+offset and can be resolved later with `dumpbin /disasm` or a
// symbolizer, which is strictly better than nothing.
//
// This is diagnostic scaffolding, not error recovery: it never resumes.

#pragma once

#ifdef WL_PLATFORM_WINDOWS

#include <cstdio>
#include <cstdlib>

// PSAPI_VERSION 1 maps GetModuleBaseNameA onto the kernel32 K32GetModuleBaseNameA
// export, which avoids linking psapi.lib in a header that client projects include
// without touching their project files.
#define PSAPI_VERSION 1
#include <windows.h>
#include <psapi.h>
#include <dbghelp.h>

namespace Wasteland
{
	namespace Detail
	{
		// Bounded so a deep/looping stack cannot spam a terminal capture.
		constexpr int kMaxFrames = 40;

		inline LONG WINAPI CrashFilter(EXCEPTION_POINTERS *info)
		{
			// stderr, unbuffered: the process is about to die and the capture is a
			// redirected file, so anything buffered in stdio would be lost.
			fprintf(stderr, "\n*** UNHANDLED EXCEPTION 0x%08lX at %p ***\n",
				(unsigned long)info->ExceptionRecord->ExceptionCode,
				info->ExceptionRecord->ExceptionAddress);

			HANDLE process = GetCurrentProcess();
			SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
			if (!SymInitialize(process, nullptr, TRUE))
				fprintf(stderr, "(SymInitialize failed: %lu)\n", (unsigned long)GetLastError());

			// Captured HERE rather than in the throwing frame: by the time the filter
			// runs, the throwing frames have already been unwound -- capturing here
			// with CaptureStackBackTrace yields the FILTER's stack (kernel +
			// UnhandledExceptionFilter), not the bug. The exception's ContextRecord
			// is the only faithful copy of the faulting stack, so StackWalk64 walks
			// from that instead.
			SYMBOL_INFO *symbol = (SYMBOL_INFO *)calloc(sizeof(SYMBOL_INFO) + 512, 1);
			if (!symbol)
				return EXCEPTION_EXECUTE_HANDLER;
			symbol->MaxNameLen = 511;
			symbol->SizeOfStruct = sizeof(SYMBOL_INFO);

			// x64 only: this engine builds x64 on Windows, so Rip/Rsp/Rbp are the
			// right seeds. A 32-bit build would need Eip and the non-64-bit
			// SymFunctionTableAccess/SymGetModuleBase callbacks.
			CONTEXT ctx = *info->ContextRecord;
			STACKFRAME64 frame = {};
			frame.AddrPC.Offset = ctx.Rip;
			frame.AddrPC.Mode = AddrModeFlat;
			frame.AddrFrame.Offset = ctx.Rbp;
			frame.AddrFrame.Mode = AddrModeFlat;
			frame.AddrStack.Offset = ctx.Rsp;
			frame.AddrStack.Mode = AddrModeFlat;

			HANDLE thread = GetCurrentThread();
			int printed = 0;
			for (int i = 0; i < kMaxFrames; ++i)
			{
				if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &ctx,
								 nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
					break;
				if (!frame.AddrPC.Offset)
					break;
				++printed;

				DWORD64 addr = frame.AddrPC.Offset;
				DWORD64 displacement = 0;
				// The module each frame lives in, so a frame in d3d12.dll / nvwgf2umx.dll
				// is distinguishable from one in the executable.
				HMODULE mod = nullptr;
				char modName[MAX_PATH] = "?";
				if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
										   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
									   (LPCSTR)(uintptr_t)addr, &mod))
					GetModuleBaseNameA(process, mod, modName, MAX_PATH);

				if (SymFromAddr(process, addr, &displacement, symbol))
				{
					DWORD lineDisplacement = 0;
					IMAGEHLP_LINE64 line = {};
					line.SizeOfStruct = sizeof(line);
					const bool hasLine = SymGetLineFromAddr64(process, addr, &lineDisplacement, &line) != FALSE;
					fprintf(stderr, "  #%02d %s!%s+0x%llx%s%s:%lu\n", i, modName,
						symbol->Name, (unsigned long long)displacement,
						hasLine ? "  (" : "", hasLine ? line.FileName : "",
						hasLine ? (unsigned long)line.LineNumber : 0UL);
				}
				else
				{
					fprintf(stderr, "  #%02d %s+0x%llx (no symbol)\n", i, modName,
						(unsigned long long)(addr - (DWORD64)(uintptr_t)mod));
				}
			}

			// FALLBACK for a call through a null function pointer: RIP is 0, so the
			// PC-based walk above cannot start and prints nothing, yet the return
			// addresses of the enclosing frames are still live on the stack. Scanning
			// the first 512 bytes below Rsp for values that land inside a loaded module
			// recovers the caller chain without a debugger.
			if (printed == 0)
			{
				fprintf(stderr, "  (RIP is not walkable -- scanning the stack for return addresses)\n");
				const DWORD64 *sp = (const DWORD64 *)(uintptr_t)ctx.Rsp;
				int shown = 0;
				for (int i = 0; i < 64 && shown < 12; ++i)
				{
					DWORD64 candidate = sp[i];
					if (candidate < 0x10000)
						continue;
					HMODULE mod = nullptr;
					if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
												GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
											(LPCSTR)(uintptr_t)candidate, &mod))
						continue;
					char modName[MAX_PATH] = "?";
					GetModuleBaseNameA(process, mod, modName, MAX_PATH);
					DWORD64 displacement = 0;
					if (SymFromAddr(process, candidate, &displacement, symbol))
						fprintf(stderr, "  ^%02d %s!%s+0x%llx\n", i, modName, symbol->Name,
							(unsigned long long)displacement);
					else
						fprintf(stderr, "  ^%02d %s+0x%llx\n", i, modName,
							(unsigned long long)(candidate - (DWORD64)(uintptr_t)mod));
					++shown;
				}
			}

			free(symbol);
			fflush(stderr);
			return EXCEPTION_EXECUTE_HANDLER;
		}
	}

	// Installs the filter. Safe to call once, at the top of main().
	inline void InstallCrashHandler()
	{
		SetUnhandledExceptionFilter(&Detail::CrashFilter);
	}
}

// dbghelp is needed only by the handler above. The pragma keeps the client
// project files unchanged -- Wasteland.lib consumers do not otherwise link it.
#pragma comment(lib, "dbghelp.lib")
// psapi for GetModuleBaseNameA. PSAPI_VERSION 1 above is supposed to remap that
// onto kernel32's K32... export, but it did not (LNK2019), so link the library.
#pragma comment(lib, "psapi.lib")

#endif // WL_PLATFORM_WINDOWS
