#include "EditorLayer.h"

#include <imgui/imgui.h>
#include <cstdlib>
#include <imgui/imgui_internal.h>
#ifdef _WIN32
#include <Windows.h>
#include <dxgi1_4.h>
#endif
#include <glad/glad.h>

#include <cstring>
#include <sstream>
#if !defined(_WIN32)
#include <unistd.h>
#endif

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <Wasteland/Scene/Entity.h>

#include "Wasteland/Scene/SceneSerializer.h"

#include "Wasteland/Utils/PlatformUtils.h"
#include "Platform/NVRHI/NVRHIFramebuffer.h"

#include "Panels/ApiKeyStore.h"
#include "Panels/ToolServer.h"

#include "ImGuizmo.h"

#include "Wasteland/Math/Math.h"

#include <fstream>
#include <filesystem>


namespace
{
	// Minimal PNG writer: 8-bit RGB, no interlacing, zlib "stored" blocks.
	//
	// Written by hand rather than pulled from stb_image_write because that header
	// is not on this project's include path (only stb_image.h is, under
	// vendor\stb_image). A screenshot is a verification aid, not a feature, and
	// adding a vendored file plus a build-path change to save ~40 lines would be
	// the larger change.
	//
	// Stored (uncompressed) deflate blocks are valid zlib, so the output opens
	// anywhere; the files are a few MB, which is fine for a debugging aid.
	void PushU32(std::vector<unsigned char> &v, uint32_t x)
	{
		v.push_back((unsigned char)(x >> 24)); v.push_back((unsigned char)(x >> 16));
		v.push_back((unsigned char)(x >> 8));  v.push_back((unsigned char)x);
	}

	uint32_t Crc32(const unsigned char *data, size_t len)
	{
		static uint32_t table[256];
		static bool init = false;
		if (!init)
		{
			for (uint32_t i = 0; i < 256; i++)
			{
				uint32_t c = i;
				for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
				table[i] = c;
			}
			init = true;
		}
		uint32_t crc = 0xFFFFFFFFu;
		for (size_t i = 0; i < len; i++) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
		return crc ^ 0xFFFFFFFFu;
	}

	void PushChunk(std::vector<unsigned char> &out, const char *type,
				   const std::vector<unsigned char> &payload)
	{
		PushU32(out, (uint32_t)payload.size());
		size_t start = out.size();
		out.insert(out.end(), type, type + 4);
		out.insert(out.end(), payload.begin(), payload.end());
		PushU32(out, Crc32(out.data() + start, out.size() - start));
	}

	bool WritePNG(const std::string &path, int width, int height, const std::vector<unsigned char> &rgb)
	{
		std::vector<unsigned char> raw;
		raw.reserve((size_t)height * ((size_t)width * 3 + 1));
		for (int y = 0; y < height; y++)
		{
			raw.push_back(0); // filter type 0 (None)
			const unsigned char *row = rgb.data() + (size_t)y * width * 3;
			raw.insert(raw.end(), row, row + (size_t)width * 3);
		}

		std::vector<unsigned char> z;
		z.push_back(0x78); z.push_back(0x01);
		size_t pos = 0;
		while (pos < raw.size())
		{
			size_t chunk = raw.size() - pos;
			if (chunk > 65535) chunk = 65535;
			bool final = (pos + chunk >= raw.size());
			z.push_back(final ? 1 : 0);
			z.push_back((unsigned char)(chunk & 0xFF));
			z.push_back((unsigned char)(chunk >> 8));
			z.push_back((unsigned char)(~chunk & 0xFF));
			z.push_back((unsigned char)((~chunk >> 8) & 0xFF));
			z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + chunk);
			pos += chunk;
		}
		uint32_t a = 1, b = 0;
		for (size_t i = 0; i < raw.size(); i++)
		{
			a = (a + raw[i]) % 65521;
			b = (b + a) % 65521;
		}
		PushU32(z, (b << 16) | a);

		std::vector<unsigned char> png = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
		std::vector<unsigned char> ihdr;
		PushU32(ihdr, (uint32_t)width);
		PushU32(ihdr, (uint32_t)height);
		ihdr.push_back(8); // bit depth
		ihdr.push_back(2); // colour type: truecolour RGB
		ihdr.push_back(0); // deflate
		ihdr.push_back(0); // adaptive filtering
		ihdr.push_back(0); // no interlace
		PushChunk(png, "IHDR", ihdr);
		PushChunk(png, "IDAT", z);
		PushChunk(png, "IEND", {});

		std::ofstream f(path, std::ios::binary);
		if (!f.is_open()) return false;
		f.write((const char *)png.data(), (std::streamsize)png.size());
		return f.good();
	}
}


namespace Wasteland
{

// Simple system stats helpers (cross-platform)
// Core-profile-safe GL extension check: glGetString(GL_EXTENSIONS) returns
// NULL on GL >= 3.0 core contexts (this engine runs 4.5 core), so enumerate
// with glGetStringi instead. Safe with no current context (returns false).
static bool HasGLExtension(const char *name)
{
	GLint count = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &count);
	if (count > 0)
	{
		for (GLint i = 0; i < count; i++)
		{
			const char *e = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
			if (e && strcmp(e, name) == 0)
				return true;
		}
		return false;
	}
	// Compat-profile fallback
	const char *ext = (const char *)glGetString(GL_EXTENSIONS);
	return ext && strstr(ext, name) != nullptr;
}

#if defined(_WIN32)
	static unsigned long long FileTimeToULL(const FILETIME &ft)
	{
		return ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
	}

	static double GetCPUUsagePercent()
	{
		static unsigned long long lastIdle = 0, lastTotal = 0;
		FILETIME idleTime, kernelTime, userTime;
		if (!GetSystemTimes(&idleTime, &kernelTime, &userTime))
			return 0.0;

		unsigned long long idle = FileTimeToULL(idleTime);
		unsigned long long kernel = FileTimeToULL(kernelTime);
		unsigned long long user = FileTimeToULL(userTime);

		unsigned long long total = (kernel + user);

		unsigned long long totalDiff = total - lastTotal;
		unsigned long long idleDiff = idle - lastIdle;

		double usage = 0.0;
		if (totalDiff > 0)
			usage = (double)(totalDiff - idleDiff) * 100.0 / (double)totalDiff;

		lastTotal = total;
		lastIdle = idle;
		return usage;
	}

	static void GetMemoryUsageMB(double &usedMB, double &totalMB, double &percent)
	{
		MEMORYSTATUSEX mem;
		mem.dwLength = sizeof(MEMORYSTATUSEX);
		if (!GlobalMemoryStatusEx(&mem))
		{
			usedMB = totalMB = percent = 0.0;
			return;
		}
		totalMB = (double)mem.ullTotalPhys / (1024.0 * 1024.0);
		double availMB = (double)mem.ullAvailPhys / (1024.0 * 1024.0);
		usedMB = totalMB - availMB;
		percent = (totalMB > 0.0) ? (usedMB * 100.0 / totalMB) : 0.0;
	}

	static bool GetGPUVRAMUsagePercent(float &outPercent)
	{
		// (GL extension presence is checked per-path below via HasGLExtension.)

		// NVIDIA GL extension (needs a current GL context — absent under the
		// D3D/Vulkan backends, where HasGLExtension safely returns false).
		if (HasGLExtension("GL_NVX_gpu_memory_info"))
		{
			const GLenum GL_GPU_MEM_TOTAL = 0x9048;
			const GLenum GL_GPU_MEM_CUR = 0x9049;
			GLint totalKB = 0, availKB = 0;
			glGetIntegerv(GL_GPU_MEM_TOTAL, &totalKB);
			glGetIntegerv(GL_GPU_MEM_CUR, &availKB);
			if (totalKB > 0)
			{
				int usedKB = totalKB - availKB;
				outPercent = (float)usedKB * 100.0f / (float)totalKB;
				return true;
			}
		}

		// Fallback: DXGI — any vendor, any backend, no GL context needed.
		// dxgi.dll is loaded dynamically so no link dependency is added.
		{
			HMODULE dxgiMod = LoadLibraryA("dxgi.dll");
			if (dxgiMod)
			{
				typedef HRESULT(WINAPI * PFN_CreateDXGIFactory1)(REFIID riid, void **ppFactory);
				PFN_CreateDXGIFactory1 createFactory =
					(PFN_CreateDXGIFactory1)GetProcAddress(dxgiMod, "CreateDXGIFactory1");
				IDXGIFactory1 *factory = nullptr;
				if (createFactory && SUCCEEDED(createFactory(IID_PPV_ARGS(&factory))) && factory)
				{
					IDXGIAdapter1 *best = nullptr;
					SIZE_T bestDedicated = 0;
					for (UINT i = 0;; i++)
					{
						IDXGIAdapter1 *a = nullptr;
						if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND)
							break;
						if (a)
						{
							DXGI_ADAPTER_DESC1 desc = {};
							if (SUCCEEDED(a->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
								desc.DedicatedVideoMemory > bestDedicated)
							{
								if (best)
									best->Release();
								best = a;
								bestDedicated = desc.DedicatedVideoMemory;
								continue;
							}
							a->Release();
						}
					}
					if (best && bestDedicated > 0)
					{
						IDXGIAdapter3 *adapter3 = nullptr;
						if (SUCCEEDED(best->QueryInterface(IID_PPV_ARGS(&adapter3))) && adapter3)
						{
							DXGI_QUERY_VIDEO_MEMORY_INFO memInfo = {};
							if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memInfo)))
							{
								outPercent = (float)((double)memInfo.CurrentUsage * 100.0 / (double)bestDedicated);
								adapter3->Release();
								best->Release();
								factory->Release();
								FreeLibrary(dxgiMod);
								return true;
							}
							adapter3->Release();
						}
						best->Release();
					}
					factory->Release();
				}
				FreeLibrary(dxgiMod);
			}
		}

		return false;
	}

#elif defined(__linux__)
#include <sys/sysinfo.h>
	static double GetCPUUsagePercent()
	{
		static unsigned long long lastTotal = 0, lastIdle = 0;
		std::ifstream proc("/proc/stat");
		if (!proc.is_open())
			return 0.0;
		std::string line;
		std::getline(proc, line);
		proc.close();

		std::istringstream ss(line);
		std::string cpu;
		unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
		ss >> cpu >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;
		unsigned long long idleAll = idle + iowait;
		unsigned long long total = user + nice + system + idle + iowait + irq + softirq + steal;

		unsigned long long totalDiff = total - lastTotal;
		unsigned long long idleDiff = idleAll - lastIdle;
		double usage = 0.0;
		if (totalDiff > 0)
			usage = (double)(totalDiff - idleDiff) * 100.0 / (double)totalDiff;

		lastTotal = total;
		lastIdle = idleAll;
		return usage;
	}

	static void GetMemoryUsageMB(double &usedMB, double &totalMB, double &percent)
	{
		struct sysinfo info;
		if (sysinfo(&info) != 0)
		{
			usedMB = totalMB = percent = 0.0;
			return;
		}
		unsigned long long total = info.totalram * (unsigned long long)info.mem_unit;
		unsigned long long free = (info.freeram + info.bufferram) * (unsigned long long)info.mem_unit;
		unsigned long long used = total - free;
		totalMB = (double)total / (1024.0 * 1024.0);
		usedMB = (double)used / (1024.0 * 1024.0);
		percent = (totalMB > 0.0) ? (usedMB * 100.0 / totalMB) : 0.0;
	}

	static bool GetGPUVRAMUsagePercent(float &outPercent)
	{
		if (!HasGLExtension("GL_NVX_gpu_memory_info"))
			return false;
		const GLenum GL_GPU_MEM_TOTAL = 0x9048;
		const GLenum GL_GPU_MEM_CUR = 0x9049;
		GLint totalKB = 0, availKB = 0;
		glGetIntegerv(GL_GPU_MEM_TOTAL, &totalKB);
		glGetIntegerv(GL_GPU_MEM_CUR, &availKB);
		if (totalKB > 0)
		{
			int usedKB = totalKB - availKB;
			outPercent = (float)usedKB * 100.0f / (float)totalKB;
			return true;
		}
		return false;
	}

#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <sys/sysctl.h>
	static double GetCPUUsagePercent()
	{
		static natural_t numCPU = 0;
		static processor_info_array_t cpuInfo = nullptr;
		static mach_msg_type_number_t cpuInfoCount = 0;
		static uint64_t lastIdle = 0, lastTotal = 0;

		mach_port_t host = mach_host_self();
		natural_t cpuCount;
		processor_info_array_t infoArray;
		mach_msg_type_number_t infoCount;
		if (host_processor_info(host, PROCESSOR_CPU_LOAD_INFO, &cpuCount, &infoArray, &infoCount) != KERN_SUCCESS)
			return 0.0;

		uint64_t idle = 0, total = 0;
		for (natural_t i = 0; i < cpuCount; ++i)
		{
			processor_cpu_load_info_data_t *cpu = (processor_cpu_load_info_data_t *)(infoArray + i * CPU_STATE_MAX);
			uint64_t user = cpu->cpu_ticks[CPU_STATE_USER];
			uint64_t nice = cpu->cpu_ticks[CPU_STATE_NICE];
			uint64_t system = cpu->cpu_ticks[CPU_STATE_SYSTEM];
			uint64_t idleTicks = cpu->cpu_ticks[CPU_STATE_IDLE];
			idle += idleTicks;
			total += user + nice + system + idleTicks;
		}

		vm_deallocate(mach_task_self(), (vm_address_t)infoArray, infoCount * sizeof(integer_t));

		uint64_t totalDiff = total - lastTotal;
		uint64_t idleDiff = idle - lastIdle;
		double usage = 0.0;
		if (totalDiff > 0)
			usage = (double)(totalDiff - idleDiff) * 100.0 / (double)totalDiff;

		lastTotal = total;
		lastIdle = idle;
		return usage;
	}

	static void GetMemoryUsageMB(double &usedMB, double &totalMB, double &percent)
	{
		uint64_t totalBytes = 0;
		size_t len = sizeof(totalBytes);
		if (sysctlbyname("hw.memsize", &totalBytes, &len, NULL, 0) != 0)
		{
			usedMB = totalMB = percent = 0.0;
			return;
		}

		mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
		vm_statistics64_data_t vmstat;
		if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vmstat, &count) != KERN_SUCCESS)
		{
			usedMB = totalMB = percent = 0.0;
			return;
		}

		uint64_t pageSize = sysconf(_SC_PAGESIZE);
		uint64_t free = (uint64_t)vmstat.free_count * pageSize + (uint64_t)vmstat.inactive_count * pageSize;
		uint64_t used = totalBytes - free;

		totalMB = (double)totalBytes / (1024.0 * 1024.0);
		usedMB = (double)used / (1024.0 * 1024.0);
		percent = (totalMB > 0.0) ? (usedMB * 100.0 / totalMB) : 0.0;
	}

	static bool GetGPUVRAMUsagePercent(float &outPercent)
	{
		if (!HasGLExtension("GL_NVX_gpu_memory_info"))
			return false;
		const GLenum GL_GPU_MEM_TOTAL = 0x9048;
		const GLenum GL_GPU_MEM_CUR = 0x9049;
		GLint totalKB = 0, availKB = 0;
		glGetIntegerv(GL_GPU_MEM_TOTAL, &totalKB);
		glGetIntegerv(GL_GPU_MEM_CUR, &availKB);
		if (totalKB > 0)
		{
			int usedKB = totalKB - availKB;
			outPercent = (float)usedKB * 100.0f / (float)totalKB;
			return true;
		}
		return false;
	}

#else
	// Unknown platform: provide stubs
	static double GetCPUUsagePercent() { return 0.0; }
	static void GetMemoryUsageMB(double &usedMB, double &totalMB, double &percent) { usedMB = totalMB = percent = 0.0; }
	static bool GetGPUVRAMUsagePercent(float &outPercent) { return false; }
#endif

	static const uint32_t s_MapWidth = 24;
	static const char *s_MapTiles =
		"WWWWWWWWWWWWWWWWWWWWWWWW"
		"WWWWWWWDDDDDWWWWWWWWWWWW"
		"WWWWWDDDDDDDDDDWWWWWWWWW"
		"WWWWDDDDDDDDDDDDCDWWWWWW"
		"WWWDDDDDDDDDDDDDDDDDWWWW"
		"WWDDDDWWWDDDDDDDDDDDDWWW"
		"WDDDDDWWWDDDDDDDDDDDDDWW"
		"WWDDDDDDDDDDDDDDDDDDDWWW"
		"WWWWDDDDDDDDDDDDDDDDWWWW"
		"WWWWWDDDDDDDDDDDDDDWWWWW"
		"WWWWWWDDDDDDDDDDDWWWWWWW"
		"WWWWWWWDDDDDDDDDWWWWWWWW"
		"WWWWWWWWWWDDDDWWWWWWWWWW"
		"WWWWWWWWWWWWWWWWWWWWWWWW";

	EditorLayer::EditorLayer()
		: Layer("EditorLayer"), m_CameraController(1280.0f / 720.0f), m_SquareColor({0.2f, 0.3f, 0.8f, 1.0f})
	{
	}

	void EditorLayer::OnAttach()
	{
		WL_PROFILE_FUNCTION();

		ApiKeyStore::Instance().Load();

		// Bridge: when AI tools write files, auto-open in Script Editor.
		// Queue the path; the UI thread drains in OnImGuiRender (ImGui is single-threaded).
		ToolServer::Instance().SetFileWrittenCallback([this](const std::string &path) {
			std::filesystem::path p(path);
				if (p.extension() == ".py")
			{
				std::lock_guard<std::mutex> lock(m_PendingFileOpensMutex);
				m_PendingFileOpens.push_back(p.string());
			}
		});

		// Give ToolServer access to the active scene for entity inspection
		ToolServer::Instance().SetScene(m_ActiveScene.get());

		m_CheckerboardTexture = nullptr;

		FramebufferSpecification fbSpec;
		fbSpec.Attachments = {FramebufferTextureFormat::RGBA8, FramebufferTextureFormat::RED_INTEGER, FramebufferTextureFormat::Depth};
		fbSpec.Width = 1280;
		fbSpec.Height = 720;
		m_Framebuffer = Framebuffer::Create(fbSpec);
		m_FramebufferAPI = RendererAPI::GetAPI();

		m_EditorScene = CreateRef<Scene>();
		m_ActiveScene = m_EditorScene;

		// The content browser roots itself at the active project, which
		// Project::Bootstrap already selected before the layer was created.
		m_ContentBrowserPanel.ResetToRoot();

		m_EditorCamera = EditorCamera(30.0f, 1.778f, 0.1f, 10000.0f);

		m_CameraController.SetZoomLevel(5.0f);

		// Load the bootstrap project's start scene.
		//
		// Project::Bootstrap opens the project itself (env var, --project, or the
		// recent-projects list) BEFORE this layer exists, so OnProjectChanged --
		// the only other place that loads StartScene -- never ran. The result was
		// an empty editor: an empty hierarchy and a black viewport for every
		// project opened at startup, which is most of them. Reusing
		// OnProjectChanged here also wires the ToolServer callbacks.

		// Deliberately LAST: it calls NewScene() and SetScene(), which replace
		// m_ActiveScene, so every panel must already hold a context.
		m_SceneHierarchyPanel.SetContext(m_ActiveScene);
		OnProjectChanged();
	}

	void EditorLayer::OnDetach()
	{
		WL_PROFILE_FUNCTION();
	}

	// Reads the bound framebuffer's colour attachment 0 and writes a PNG.
	//
	// Backend-specific by necessity: there is no cross-backend readback API on
	// Framebuffer, and glReadPixels / a D3D12 copy-to-buffer are genuinely
	// different code. Both paths are implemented rather than stubbing one,
	// because the Nova path this exists to verify is the DX12 one.
	//
	// DX12 reads the Nova output image itself rather than the default
	// framebuffer -- under ray tracing the traced image never reaches the default
	// framebuffer at all -- so the two branches do not agree on WHAT they read,
	// only on the PNG they write.
	//
	// Returns false rather than throwing on failure, so a failed capture shows
	// up in the log instead of taking the editor down mid-frame.
	bool EditorLayer::CaptureFramebufferPNG(const std::string &path)
	{
		if (!m_Framebuffer)
			return false;

		// READS THE DEFAULT FRAMEBUFFER (the window), NOT m_Framebuffer.
		//
		// m_Framebuffer is the 2D renderer's own offscreen target; the 3D scene
		// and the ImGui viewport both present to the window's default
		// framebuffer. Binding m_Framebuffer here produced a uniformly black
		// image while reporting success -- the first version of this function
		// read the wrong buffer entirely.
		if (m_FramebufferAPI == RendererAPI::API::None)
			return false;

		// DX12 NVRHI. Handled FIRST and before the glReadPixels path below, for a
		// reason that is about ordering rather than preference: the Nova path tracer
		// renders into its own UAV image and never touches the default framebuffer,
		// so there is nothing for a GL readback to read on this backend. Worse, the
		// GL viewport query below would measure a context this backend never set,
		// yielding a plausible-looking width/height for an image that does not
		// exist. Reporting failure is what made this backend look broken rather
		// than unsupported.
		//
		// The readback goes through Renderer3D rather than reaching for the DX12
		// device here: Renderer3D owns the Nova instance and knows which backend is
		// active, and the copy has to be recorded on the same command queue and
		// fenced against the same frame ring as the rendering that produced the
		// image. Doing the copy here would mean duplicating all of that.
		if (m_FramebufferAPI == RendererAPI::API::NVRHI_DX12)
		{
			std::vector<unsigned char> rgba;
			uint32_t rtWidth = 0, rtHeight = 0;
			if (!Renderer3D::ReadbackRayTraceTarget(rgba, rtWidth, rtHeight))
			{
				// Distinct from the "unsupported API" warning further down: the API IS
				// supported, so this means there was nothing rendered to read (ray
				// tracing off, or a frame captured before the first Nova flush).
				WL_CORE_WARN("EditorLayer: DX12 screenshot found no Nova output to read; "
							 "ray tracing may be disabled or nothing has rendered yet");
				return false;
			}

			// RGBA to RGB. The PNG writer emits colour type 2 (truecolour RGB), and
			// dropping the alpha is what the OpenGL branch below does with the same
			// four-channel read, so the two backends produce byte-identical files for
			// the same picture.
			std::vector<unsigned char> pixels((size_t)rtWidth * (size_t)rtHeight * 3);
			for (uint32_t y = 0; y < rtHeight; ++y)
			{
				const unsigned char *src = rgba.data() + (size_t)y * rtWidth * 4;
				unsigned char *dst = pixels.data() + (size_t)y * rtWidth * 3;
				for (uint32_t x = 0; x < rtWidth; ++x)
				{
					dst[x * 3 + 0] = src[x * 4 + 0];
					dst[x * 3 + 1] = src[x * 4 + 1];
					dst[x * 3 + 2] = src[x * 4 + 2];
				}
			}
			return WritePNG(path, (int)rtWidth, (int)rtHeight, pixels);
		}

		// GL_VIEWPORT, not the window size: on a HiDPI display the drawable
		// framebuffer is larger than the window, and glReadPixels measures in
		// framebuffer pixels. GLFW headers are not available in this project,
		// so the viewport state is queried through GL instead.
		GLint vp[4] = { 0, 0, 0, 0 };
		glGetIntegerv(GL_VIEWPORT, vp);
		int width = vp[2];
		int height = vp[3];
		if (width <= 0 || height <= 0)
			return false;

		std::vector<unsigned char> pixels((size_t)width * (size_t)height * 3);

		if (m_FramebufferAPI == RendererAPI::API::OpenGL)
		{
			// glReadPixels returns bottom-up rows and RGBA; the PNG writer wants
			// top-down RGB.
			std::vector<unsigned char> rgba((size_t)width * (size_t)height * 4);
			glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
			glPixelStorei(GL_PACK_ALIGNMENT, 1);
			glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
			for (int y = 0; y < height; ++y)
			{
				const unsigned char *src = rgba.data() + (size_t)(height - 1 - y) * width * 4;
				unsigned char *dst = pixels.data() + (size_t)y * width * 3;
				for (int x = 0; x < width; ++x)
				{
					dst[x * 3 + 0] = src[x * 4 + 0];
					dst[x * 3 + 1] = src[x * 4 + 1];
					dst[x * 3 + 2] = src[x * 4 + 2];
				}
			}
		}
		else
		{
			// DX11 and Vulkan. Both render through NVRHI, and both put the Nova
			// output in a backend-native image whose owner -- DX11Nova /
			// VulkanNova -- has no readback. Reading the swap chain back instead
			// would need a copy-to-buffer the NVRHI framebuffer does not expose, and
			// would capture the window rather than the traced image anyway. So this
			// path is honestly unsupported rather than silently producing a black
			// image, which is exactly what it used to do on DX12 too.
			WL_CORE_WARN("EditorLayer: screenshot not implemented for API {0}; "
						 "only OpenGL and DX12 readback exist",
						 (int)m_FramebufferAPI);
			return false;
		}

		return WritePNG(path, width, height, pixels);
	}

	void EditorLayer::OnUpdate(Timestep ts)
	{
		WL_PROFILE_FUNCTION();

		// FRAMEBUFFER SCREENSHOT. Needed because there was no way to verify
		// rendering headlessly: a clean log does not prove pixels are correct
		// (a deliberately broken shader produced no error output at all), and
		// every Nova technique in the Phase -1 work is meant to be judged on the
		// image, not the absence of a crash.
		//
		// F12 captures. WL_SCREENSHOT=<path> plus WL_SCREENSHOT_FRAME=<n> makes
		// it hands-free, which is the only form usable from a script or CI.
		// The file is written on the NEXT present, so the capture contains the
		// frame as displayed.
		++m_FrameCounter;
		{
			const char *shotPath = std::getenv("WL_SCREENSHOT");
			int shotFrame = 0;
			if (const char *f = std::getenv("WL_SCREENSHOT_FRAME"))
				shotFrame = std::atoi(f);
			bool wantShot = (shotPath && shotFrame > 0 && m_FrameCounter == shotFrame);
			if (!shotPath && Input::IsKeyPressed(WL_KEY_F12))
				wantShot = true;

			if (wantShot)
			{
				std::string outPath = shotPath ? shotPath : "wasteland_screenshot.png";
				if (CaptureFramebufferPNG(outPath))
					WL_CORE_INFO("EditorLayer: screenshot written to {0}", outPath);
				else
					WL_CORE_ERROR("EditorLayer: screenshot FAILED ({0})", outPath);
			}
		}

		Wasteland::Renderer3D::NotifyFrameTime((double)ts.GetMilliseconds());

		if (m_FramebufferAPI != RendererAPI::GetAPI())
		{
			// GPU objects of the old API are dead: rebuild the 2D renderer's
			// shaders/buffers against the new API before recreating the framebuffer
			// (Renderer2D::Init runs inside, resetting the stats it needs).
			Renderer2D::RecreateResources();
			Renderer3D::RecreateResources();
			// Material textures are backend GPU objects loaded at scene
			// deserialization: re-create them under the new API or every
			// textured draw samples stale (dead GL names / null SRVs).
			if (m_ActiveScene)
				m_ActiveScene->ReloadMaterialTextures();
			if (m_EditorScene && m_EditorScene != m_ActiveScene)
				m_EditorScene->ReloadMaterialTextures();
			FramebufferSpecification spec = m_Framebuffer->GetSpecification();
			m_Framebuffer = Framebuffer::Create(spec);
			m_FramebufferAPI = RendererAPI::GetAPI();
		}

		// Resize
		if (FramebufferSpecification spec = m_Framebuffer->GetSpecification();
			m_ViewportSize.x > 0.0f && m_ViewportSize.y > 0.0f && // zero sized framebuffer is invalid
			(spec.Width != m_ViewportSize.x || spec.Height != m_ViewportSize.y))
		{
			m_Framebuffer->Resize((uint32_t)m_ViewportSize.x, (uint32_t)m_ViewportSize.y);
			m_CameraController.OnResize(m_ViewportSize.x, m_ViewportSize.y);
			Renderer3D::ResizeRayTraceTarget((uint32_t)m_ViewportSize.x, (uint32_t)m_ViewportSize.y);
			m_EditorCamera.SetViewportSize(m_ViewportSize.x, m_ViewportSize.y);
			m_ActiveScene->OnViewportResize((uint32_t)m_ViewportSize.x, (uint32_t)m_ViewportSize.y);
		}

		// Render
		Renderer2D::ResetStats();
		Renderer3D::ResetStats();

		bool isRayTracing = Renderer3D::IsRayTracingEnabled();

		if (!isRayTracing)
		{
			// Standard raster pass needs the framebuffer bound
			m_Framebuffer->Bind();
			RenderCommand::SetClearColor({0.1f, 0.1f, 0.1f, 1.0f});
			RenderCommand::Clear();
			m_Framebuffer->ClearAttachment(1, -1);
		}
		else
		{
			// Ray tracing bypasses the default framebuffer entirely!
			// We just need to clear out your 2D debug overlay stats/pipeline states
			RenderCommand::SetClearColor({0.1f, 0.1f, 0.1f, 1.0f});
		}

		switch (m_SceneState)
		{
		case SceneState::Edit:
		{
			if (m_ViewportFocused)
			{
				m_CameraController.OnUpdate(ts);
			}

			m_EditorCamera.OnUpdate(ts);

			m_ActiveScene->OnUpdateEditor(ts, m_EditorCamera);
			break;
		}
		case SceneState::Play:
		{
			if (!m_IsPaused)
				m_ActiveScene->OnUpdateRuntime(ts);
			break;
		}
		case SceneState::Simulate:
		{
			m_EditorCamera.OnUpdate(ts);

			m_ActiveScene->OnUpdateSimulation(ts, m_EditorCamera);
			break;
		}
		}

		// Handle non-raytraced system layouts (like entity picking and overlays)
		if (!isRayTracing)
		{
			auto [mx, my] = ImGui::GetMousePos();
			mx -= m_ViewportBounds[0].x;
			my -= m_ViewportBounds[0].y;
			glm::vec2 viewportSize = m_ViewportBounds[1] - m_ViewportBounds[0];
			my = viewportSize.y - my;
			int mouseX = (int)mx;
			int mouseY = (int)my;

			if (mouseX >= 0 && mouseY >= 0 && mouseX < (int)viewportSize.x && mouseY < (int)viewportSize.y)
			{
				glm::ivec2 currentPos = {mouseX, mouseY};
				bool mouseMoved = currentPos != m_LastMousePixelPos;
				m_PickFrameCounter++;
				if (mouseMoved || m_PickFrameCounter >= 15)
				{
					int pixelData = m_Framebuffer->ReadPixel(1, mouseX, mouseY);
					m_HoveredEntity = pixelData == -1 ? Entity() : Entity((entt::entity)pixelData, m_ActiveScene.get());
					m_LastMousePixelPos = currentPos;
					m_PickFrameCounter = 0;
				}
			}

			OnOverlayRender();
			m_Framebuffer->Unbind(); // Safely unbind the active frame buffer
		}
		else
		{
			// Reset entity mouse states when tracing rays to avoid reading dead FBO memory bounds
			m_HoveredEntity = Entity();
		}
	}


	const char *EditorLayer::WorkflowName(Workflow workflow)
	{
		switch (workflow)
		{
		case Workflow::Scene:
			return "Scene";
		case Workflow::Scripting:
			return "Scripting";
		case Workflow::Visual:
			return "Visual Blocks";
		case Workflow::Review:
			return "Review";
		}
		return "Scene";
	}

	void EditorLayer::ApplyLayout(Workflow workflow)
	{
		m_Workflow = workflow;

		ImGuiIO &io = ImGui::GetIO();
		if (!(io.ConfigFlags & ImGuiConfigFlags_DockingEnable))
			return;

		// Ratios are fractions of the dockspace. They are the whole point of
		// a hardcoded layout: the viewport dominates in Scene, the code in
		// Scripting, the graph in Visual -- without dragging a single splitter.
		// Left stays narrow in every workflow because it always holds the
		// same two scene panels; the centre keeps the majority for the panel
		// the workflow is actually about.
		float leftRatio = 0.20f;
		float rightRatio = 0.24f;
		float bottomRatio = 0.30f;
		switch (workflow)
		{
		case Workflow::Scripting:
			leftRatio = 0.20f;
			rightRatio = 0.26f;
			bottomRatio = 0.22f;
			break;
		case Workflow::Visual:
			leftRatio = 0.20f;
			rightRatio = 0.30f;
			bottomRatio = 0.20f;
			break;
		case Workflow::Review:
			leftRatio = 0.22f;
			rightRatio = 0.28f;
			bottomRatio = 0.26f;
			break;
		case Workflow::Scene:
			break;
		}

		// Visibility first. A hidden panel still holds its dock tab, so an
		// unrelated tool left visible just shows an empty frame.
		m_SceneHierarchyPanel.SetHierarchyVisible(true);
		m_SceneHierarchyPanel.SetPropertiesVisible(true);
		m_ContentBrowserPanel.SetVisible(true);
		m_ScriptEditorPanel.SetVisible(workflow == Workflow::Scripting);
		m_ScriptInspectorPanel.SetVisible(workflow == Workflow::Scripting);
		m_NPCBehaviorPanel.SetVisible(workflow == Workflow::Visual);
		m_VisualBlocksPanel.SetVisible(workflow == Workflow::Visual);
		m_TerrainAssetPanel.SetVisible(false);
		m_AIChatPanel.SetVisible(workflow == Workflow::Review);
		m_ModelDownloaderPanel.SetVisible(workflow == Workflow::Scene);
		m_ShowViewport = workflow != Workflow::Review;

		// ONE rebuild, on demand. RemoveNode/AddNode/Finish discards the
		// whole dock tree, so this runs on the click (and once at startup),
		// NEVER per frame -- per-frame is what made every panel move
		// continuously and left the editor unusable.
		ImGuiID dockspace = ImGui::GetID("MyDockSpace");
		ImGui::DockBuilderRemoveNode(dockspace);
		ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
		ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->WorkSize);

		ImGuiID rest = dockspace;
		ImGuiID left = rest;
		if (leftRatio > 0.0f)
			left = ImGui::DockBuilderSplitNode(rest, ImGuiDir_Left, leftRatio, nullptr, &rest);
		ImGuiID right = rest;
		ImGuiID centre = rest;
		if (rightRatio > 0.0f)
			right = ImGui::DockBuilderSplitNode(rest, ImGuiDir_Right, rightRatio, nullptr, &centre);
		ImGuiID bottom = centre;
		ImGuiID top = centre;
		if (bottomRatio > 0.0f)
			bottom = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down, bottomRatio, nullptr, &top);

		auto dock = [](const char *name, ImGuiID node) {
			ImGui::DockBuilderDockWindow(name, node);
		};

		// Every panel the editor owns. Listed exhaustively because a window
		// DockBuilderDockWindow never sees reverts to a free-floating window on
		// first use -- that is exactly how Settings, Stats and Node Palette
		// ended up loose on screen in every workflow.
		const char *const kPanels[] = {
			"Scene Hierarchy",
			"Properties",
			"Content Browser",
			"Viewport",
			"Script Editor",
			"Script Inspector",
			"NPC Behavior",
			"Node Palette",
			"Visual Blocks",
			"AVB Graph",
			"Terrain Asset",
			"AI Chatbot",
			"Model Downloader",
			"Settings",
			"Stats",
		};

		// Left rail: hierarchy over properties. The centre: this workflow's
		// primary panel. Both regions are therefore always occupied -- an
		// empty dock region reads like a broken layout.
		ImGuiID hierarchy = left;
		ImGuiID properties = left;
		if (left != 0)
			properties = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.5f, &hierarchy, nullptr);
		dock("Scene Hierarchy", hierarchy);
		dock("Properties", properties);
		dock("Content Browser", bottom);

		const char *primary = "Viewport";
		switch (workflow)
		{
		case Workflow::Scripting:
			primary = "Script Editor";
			break;
		case Workflow::Visual:
			primary = "Visual Blocks";
			break;
		case Workflow::Review:
			primary = "AI Chatbot";
			break;
		case Workflow::Scene:
			break;
		}
		dock(primary, top);

		// Everything not primary is docked into the side rail: docked but
		// hidden unless the workflow uses it, so no panel floats and no
		// region sits empty.
		for (const char *name : kPanels)
		{
			if (std::strcmp(name, primary) == 0 ||
				std::strcmp(name, "Scene Hierarchy") == 0 ||
				std::strcmp(name, "Properties") == 0 ||
				std::strcmp(name, "Content Browser") == 0)
				continue;
			// Model Downloader shares the bottom rail with the browser in
			// Scene; the sidebar is the tidier home everywhere else.
			if (workflow == Workflow::Scene && std::strcmp(name, "Model Downloader") == 0)
				dock(name, bottom);
			else
				dock(name, right);
		}

		ImGui::DockBuilderFinish(dockspace);
		WL_CORE_INFO("Applied '{0}' editor layout", WorkflowName(workflow));
	}

	void EditorLayer::OnImGuiRender()
	{
		// Note: Switch this to true to enable dockspace
		static bool dockingEnabled = true;
		if (dockingEnabled)
		{
			static bool dockspaceOpen = true;
			static bool opt_fullscreen = true;
			static bool opt_padding = false;
			static ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_None;

			ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
			if (opt_fullscreen)
			{
				const ImGuiViewport *viewport = ImGui::GetMainViewport();
				ImGui::SetNextWindowPos(viewport->WorkPos);
				ImGui::SetNextWindowSize(viewport->WorkSize);
				ImGui::SetNextWindowViewport(viewport->ID);
				ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
				ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
				window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
				window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
			}
			else
			{
				dockspace_flags &= ~ImGuiDockNodeFlags_PassthruCentralNode;
			}

			if (dockspace_flags & ImGuiDockNodeFlags_PassthruCentralNode)
				window_flags |= ImGuiWindowFlags_NoBackground;

			if (!opt_padding)
				ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
			ImGui::Begin("DockSpace Demo", &dockspaceOpen, window_flags);
			if (!opt_padding)
				ImGui::PopStyleVar();

			if (opt_fullscreen)
				ImGui::PopStyleVar(2);

			// Submit the DockSpace
			ImGuiIO &io = ImGui::GetIO();
			if (io.ConfigFlags & ImGuiConfigFlags_DockingEnable)
			{
				ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
				// Dock BEFORE submitting the dockspace, and only on this one
				// frame. ImGui reads the saved imgui.ini layout, so without
				// this the editor opens with whatever was last persisted --
				// including panels floating free. Guarded by m_LayoutApplied
				// because ApplyLayout destroys the whole dock tree; running it
				// every frame is what destroyed the layout earlier.
				// Applied HERE, before DockSpace() is submitted for this frame.
				// Building it after submission -- straight from the menu -- left the
				// submitted tree and the builder disagreeing, so switching layouts and
				// coming back silently undocked the panels.
				if (!m_LayoutApplied || m_PendingWorkflow != m_Workflow)
				{
					if (!m_LayoutApplied)
					{
										// WASTELAND_EDITOR_WORKFLOW seeds the layout the editor opens in.
										// Clicking a menu is the only other route and that cannot be
										// automated -- screenshots and CI have no cursor.
										if (const char *seed = std::getenv("WASTELAND_EDITOR_WORKFLOW"))
										{
															const std::string name(seed);
															if (name == "Scripting")
																				m_Workflow = Workflow::Scripting;
															else if (name == "Visual" || name == "Visual Blocks")
																				m_Workflow = Workflow::Visual;
															else if (name == "Review")
																				m_Workflow = Workflow::Review;
															else if (name != "Scene")
																				WL_CORE_WARN("Unknown WASTELAND_EDITOR_WORKFLOW '{0}'", name);
										}
										m_PendingWorkflow = m_Workflow;
					}
					const Workflow requested = m_PendingWorkflow;
					m_LayoutApplied = true;
					m_Workflow = requested;
					ApplyLayout(requested);

					// WASTELAND_EDITOR_GRAPH opens a graph at startup. Opening one
					// otherwise means clicking through the content browser, which
					// screenshots and CI cannot do.
					if (const char *graphPath = std::getenv("WASTELAND_EDITOR_GRAPH"))
						m_VisualBlocksPanel.OpenGraph(graphPath);
				}
				ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);
			}

			if (ImGui::BeginMenuBar())
			{
				if (ImGui::BeginMenu("File"))
				{
					if (ImGui::MenuItem("New"))
						NewScene();

					if (ImGui::MenuItem("New Terrain Asset..."))
						NewTerrainAsset();

					if (ImGui::MenuItem("Open..."))
						OpenScene();

					if (ImGui::MenuItem("Save As..."))
						SaveSceneAs();

					ImGui::Separator();

					if (ImGui::MenuItem("Exit"))
						Wasteland::Application::Get().Close();
					ImGui::EndMenu();
				}

				UI_ProjectMenu();

				if (ImGui::BeginMenu("Scripts"))
				{
					if (ImGui::MenuItem("New"))
						NewScript();

					if (ImGui::MenuItem("Open"))
						OpenScript();

					if (ImGui::MenuItem("Save", "Ctrl+S", false, m_ScriptEditorPanel.IsOpen()))
						m_ScriptEditorPanel.SaveFile();

					ImGui::Separator();

					if (ImGui::MenuItem("Close Script", nullptr, false, m_ScriptEditorPanel.IsOpen()))
						m_ScriptEditorPanel.CloseFile();

					ImGui::EndMenu();
				}

				if (ImGui::BeginMenu("View"))
				{
					ImGui::MenuItem("Viewport", nullptr, &m_ShowViewport);
					bool hierarchy = m_SceneHierarchyPanel.IsHierarchyVisible();
					if (ImGui::MenuItem("Scene Hierarchy", nullptr, &hierarchy))
						m_SceneHierarchyPanel.SetHierarchyVisible(hierarchy);
					bool properties = m_SceneHierarchyPanel.IsPropertiesVisible();
					if (ImGui::MenuItem("Properties", nullptr, &properties))
						m_SceneHierarchyPanel.SetPropertiesVisible(properties);
					bool browser = m_ContentBrowserPanel.IsVisible();
					if (ImGui::MenuItem("Content Browser", nullptr, &browser))
						m_ContentBrowserPanel.SetVisible(browser);
					bool scriptEditor = m_ScriptEditorPanel.IsVisible();
					if (ImGui::MenuItem("Script Editor", nullptr, &scriptEditor))
						m_ScriptEditorPanel.SetVisible(scriptEditor);
					bool inspector = m_ScriptInspectorPanel.IsVisible();
					if (ImGui::MenuItem("Script Inspector", nullptr, &inspector))
						m_ScriptInspectorPanel.SetVisible(inspector);
					bool npc = m_NPCBehaviorPanel.IsVisible();
					if (ImGui::MenuItem("NPC Behavior", nullptr, &npc))
						m_NPCBehaviorPanel.SetVisible(npc);
					// General visual scripting (.wlblocks). Its own entry, not a
					// sub-item of NPC Behavior: the two are sibling formats with
					// different editors, and nesting one under the other is what
					// made them look like one feature.
					bool vb = m_VisualBlocksPanel.IsVisible();
					if (ImGui::MenuItem("Visual Blocks", nullptr, &vb))
						m_VisualBlocksPanel.SetVisible(vb);
					bool avb = m_AvbGraphPanel.IsVisible();
					if (ImGui::MenuItem("AVB Graph", nullptr, &avb))
						m_AvbGraphPanel.SetVisible(avb);
					bool terrain = m_TerrainAssetPanel.IsVisible();
					if (ImGui::MenuItem("Terrain Asset", nullptr, &terrain))
						m_TerrainAssetPanel.SetVisible(terrain);
					ImGui::MenuItem("Stats", nullptr, &m_ShowStats);
					ImGui::MenuItem("Settings", nullptr, &m_ShowSettings);
					bool chatVisible = m_AIChatPanel.IsVisible();
					if (ImGui::MenuItem("AI Chatbot", nullptr, &chatVisible))
						m_AIChatPanel.SetVisible(chatVisible);

					bool dlVisible = m_ModelDownloaderPanel.IsVisible();
					if (ImGui::MenuItem("Model Downloader", nullptr, &dlVisible))
						m_ModelDownloaderPanel.SetVisible(dlVisible);

					ImGui::EndMenu();
				}

					// Hardcoded layouts, beside View because that is where the panel
					// show/hide toggles already live: 'what is on screen' and 'where
					// it sits' are then answered in one place.
					if (ImGui::BeginMenu("Layout"))
					{
						const Workflow workflows[] = {Workflow::Scene, Workflow::Scripting,
															 Workflow::Visual, Workflow::Review};
						for (Workflow workflow : workflows)
						{
												// Checked rather than a plain item: the active layout shows a
												// tick, so the current state can be read without moving it.
												const bool active = m_Workflow == workflow;
												if (ImGui::MenuItem(WorkflowName(workflow), nullptr, active))
												{
																		if (!active)
												m_PendingWorkflow = workflow;
												}
						}
						ImGui::Separator();
						if (ImGui::MenuItem("Reset to \"Scene\""))
							m_PendingWorkflow = Workflow::Scene;
						ImGui::EndMenu();
					}

				ImGui::EndMenuBar();
			}

			// Drain pending file opens from AI tool calls (must be on UI thread).
		{
			std::lock_guard<std::mutex> lock(m_PendingFileOpensMutex);
			for (const auto &p : m_PendingFileOpens)
			{
				// By EXTENSION, not unconditionally to the Script Editor:
				// the drain used to open every queued path there, so a
				// graph handed in by a tool landed in a text editor
				// showing YAML. Dispatched through the same path the
				// content browser uses.
				OpenVisualFile(p);
			}
			m_PendingFileOpens.clear();
		}

		m_SceneHierarchyPanel.OnImGuiRender();
		m_ContentBrowserPanel.OnImGuiRender();

		// Drain the content browser's Open request.
		//
		// Dispatch lives here rather than in the browser because the browser
		// does not know which editor owns which extension. Drained AFTER the
		// browser renders (so the request exists) and BEFORE the target panel
		// renders (so a tree opened this frame is already drawn this frame
		// rather than appearing one frame late).
		{
			std::string openRequest = m_ContentBrowserPanel.ConsumeOpenRequest();
			if (!openRequest.empty())
			{
				OpenVisualFile(openRequest);
			}
		}

		m_ScriptInspectorPanel.SetEntity(m_SceneHierarchyPanel.GetSelectedEntity());
		m_ScriptInspectorPanel.OnImGuiRender();
		m_NPCBehaviorPanel.SetEntity(m_SceneHierarchyPanel.GetSelectedEntity());
		m_NPCBehaviorPanel.OnImGuiRender();
		m_VisualBlocksPanel.OnImGuiRender();
		m_AvbGraphPanel.OnImGuiRender();
		m_TerrainAssetPanel.OnImGuiRender(Project::Get().GetAssetRoot(),
			Project::Get().GetRoot(), m_ActiveScene,
			m_SceneState == SceneState::Edit);
		m_ScriptEditorPanel.OnImGuiRender();

			m_AIChatPanel.OnImGuiRender();
			m_ModelDownloaderPanel.OnImGuiRender();

			// In-game HUD: only while playing, and only over the viewport, so
			// it never competes with the editor panels. Reads the same
			// components the scripts write, so HUD and gameplay cannot
			// disagree about health/ammo.
			if (m_SceneState == SceneState::Play)
				RenderGameplayHUD();

			if (m_ShowStats)
			{
				ImGui::Begin("Stats");

			std::string name = "None";
			if (m_HoveredEntity)
			{
				if (m_HoveredEntity.HasComponent<TagComponent>())
					name = m_HoveredEntity.GetComponent<TagComponent>().Tag;
				else
					name = "Unnamed Entity";
			}
			ImGui::Text("Hovered Entity: %s", name.c_str());

			ImGui::Separator();

			ImGui::Text("Application FPS: %.1f", io.Framerate);
			ImGui::Text("Frame Time: %.3f ms", 1000.0f / io.Framerate);

			// System stats (throttled to ~4Hz to avoid per-frame driver/OS calls)
#ifdef _WIN32
			static double cpu = 0.0;
			static double usedMB = 0.0, totalMB = 0.0, memPct = 0.0;
			static float gpuPct = 0.0f;
			static bool haveGPU = false;
			static int statsFrameCounter = 0;
			if (++statsFrameCounter >= 15)
			{
				statsFrameCounter = 0;
				cpu = GetCPUUsagePercent();
				GetMemoryUsageMB(usedMB, totalMB, memPct);
				haveGPU = GetGPUVRAMUsagePercent(gpuPct);
			}

			ImGui::Text("CPU Usage: %.1f%%", cpu);
			ImGui::Text("RAM: %.0f / %.0f MB (%.0f%%)", usedMB, totalMB, memPct);
			if (haveGPU)
				ImGui::Text("GPU VRAM Usage: %.1f%%", gpuPct);
			else
				ImGui::Text("GPU VRAM Usage: N/A");
#else
			ImGui::Text("CPU Usage: N/A");
			ImGui::Text("RAM: N/A");
			ImGui::Text("GPU VRAM Usage: N/A");
#endif

			ImGui::Separator();

			auto stats = Wasteland::Renderer2D::GetStats();
			ImGui::Text("Renderer2D Stats:");
			ImGui::Text("Draw Calls: %d", stats.DrawCalls);
			ImGui::Text("Quads: %d", stats.QuadCount);
			ImGui::Text("Vertices: %d", stats.GetTotalVertexCount());
			ImGui::Text("Indices: %d", stats.GetTotalIndexCount());

			ImGui::Separator();

			auto stats3D = Wasteland::Renderer3D::GetStats();
			ImGui::Text("Renderer3D Stats:");
			ImGui::Text("Draw Calls: %d", stats3D.DrawCalls);
			ImGui::Text("Quads: %d", stats3D.QuadCount);
			ImGui::Text("Vertices: %d", stats3D.GetTotalVertexCount());
			ImGui::Text("Indices: %d", stats3D.GetTotalIndexCount());

			ImGui::End();
			}

			if (m_ShowSettings)
			{
				ImGui::Begin("Settings");

			ImGui::Checkbox("Show physics colliders", &m_ShowPhysicsColliders);

			// Rendering API Selection
			ImGui::Separator();
			ImGui::Text("Rendering Backend");

			static const char *apiItems[] = {"OpenGL", "DirectX 11", "DirectX 12", "Vulkan"};
			int currentAPI = (int)Wasteland::RendererAPI::GetAPI() - 1; // -1 because None = 0
			if (currentAPI < 0)
				currentAPI = 0;

			if (ImGui::Combo("Rendering API", &currentAPI, apiItems, IM_ARRAYSIZE(apiItems)))
			{
				Wasteland::RendererAPI::API newAPI = (Wasteland::RendererAPI::API)(currentAPI + 1);

				// Switch window context (also updates RenderCommand API atomically when switch executes)
				Wasteland::Application::Get().GetWindow().SwitchRendererAPI(newAPI);

				WL_CORE_INFO("Switched to rendering API: {0}", apiItems[currentAPI]);
			}

			ImGui::Separator();
			ImGui::Text("Ray Tracing Settings");

			bool rtEnabled = Wasteland::Renderer3D::IsRayTracingEnabled();
			if (ImGui::Checkbox("Nova Rendering Pipeline", &rtEnabled))
			{
				Wasteland::Renderer3D::SetRayTracingEnabled(rtEnabled);
			}

			ImGui::Separator();
			ImGui::Text("Ray Tracing Settings");

			static const char *qualityItems[] = {"Low", "Medium", "High", "Ultra"};
			int currentQuality = (int)Wasteland::Renderer3D::GetQualityPreset();
			if (ImGui::Combo("Quality Preset", &currentQuality, qualityItems, IM_ARRAYSIZE(qualityItems)))
			{
				Wasteland::Renderer3D::SetQualityPreset((Wasteland::QualityPreset)currentQuality);
			}

			float renderScale = Wasteland::Renderer3D::GetRenderScale();
			if (ImGui::SliderFloat("Resolution Scale", &renderScale, 0.25f, 1.0f))
			{
				Wasteland::Renderer3D::SetRenderScale(renderScale);
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Fraction of viewport resolution the path tracer runs at.\nLower = faster (upscaled in composite). Resets when the preset changes.");

			ImGui::Separator();
			ImGui::Text("Performance (older GPUs)");

			const char *gpuTier = Wasteland::Renderer3D::GetGPUTierName();
			ImGui::Text("GPU: %s", (gpuTier && gpuTier[0]) ? gpuTier : "Unknown");
			float curFPS = Wasteland::Renderer3D::GetCurrentFPS();
			float gpuMs = Wasteland::Renderer3D::GetGPUFrameTimeMs();
			if (curFPS > 0.5f)
				ImGui::Text("Tracer: %.0f fps (%.1f ms GPU)%s", curFPS, gpuMs,
					Wasteland::Renderer3D::IsDynamicResolutionActive() ? " [SCALED]" : "");
			else
				ImGui::Text("Tracer: measuring...");

			bool drsEnabled = Wasteland::Renderer3D::IsDynamicResolutionEnabled();
			if (ImGui::Checkbox("Dynamic Resolution (hold target FPS)", &drsEnabled))
			{
				Wasteland::Renderer3D::SetDynamicResolutionEnabled(drsEnabled);
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Auto-adjusts trace resolution between Min Scale and the\npreset/slider ceiling to hold the target framerate.\nKeeps Ultra above 30fps on RTX 20-series.");

			float targetFPS = Wasteland::Renderer3D::GetTargetFPS();
			if (ImGui::SliderFloat("Target FPS", &targetFPS, 20.0f, 60.0f))
			{
				Wasteland::Renderer3D::SetTargetFPS(targetFPS);
			}

			float dynMin = Wasteland::Renderer3D::GetDynamicMinScale();
			if (ImGui::SliderFloat("Dynamic Min Scale", &dynMin, 0.25f, 1.0f))
			{
				Wasteland::Renderer3D::SetDynamicMinScale(dynMin);
			}

			if (ImGui::Button("Tune for RTX 20-series (Ultra 30fps)"))
			{
				Wasteland::Renderer3D::SetQualityPreset(Wasteland::QualityPreset::Ultra);
				Wasteland::Renderer3D::SetDynamicResolutionEnabled(true);
				Wasteland::Renderer3D::SetTargetFPS(30.0f);
				Wasteland::Renderer3D::SetDynamicMinScale(0.5f);
				Wasteland::Renderer3D::SetVolumetricStepScale(0.75f);
				Wasteland::Renderer3D::SetNeuralRenderingEnabled(true);
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Ultra preset + dynamic resolution at 30fps target.\nVolumetric quality 0.75x, neural GI on (carries diffuse\nlighting when the scaler drops resolution).");

			// D3: the "Samples Per Pixel" slider used to live here. It drove
			// Renderer3D::SetSamplesPerPixel, which only stores a value no shader
			// reads -- there is no per-pixel sample loop in the Nova path -- so the
			// control did nothing visible while looking authoritative. Removed
			// rather than left in place. Multi-sampling returns with the temporal
			// work in Phase 4.

			bool accumulate = Wasteland::Renderer3D::IsRayTracingAccumulate();
			if (ImGui::Checkbox("Accumulate", &accumulate))
			{
				Wasteland::Renderer3D::SetRayTracingAccumulate(accumulate);
			}

			ImGui::Separator();
			ImGui::Text("Neural Rendering (OpenGL)");

			bool neuralEnabled = Wasteland::Renderer3D::IsNeuralRenderingEnabled();
			if (ImGui::Checkbox("Neural Textures + Lighting", &neuralEnabled))
			{
				Wasteland::Renderer3D::SetNeuralRenderingEnabled(neuralEnabled);
			}

			float neuralTexStrength = Wasteland::Renderer3D::GetNeuralTextureStrength();
			if (ImGui::SliderFloat("Neural Texture Detail", &neuralTexStrength, 0.0f, 1.0f))
			{
				Wasteland::Renderer3D::SetNeuralTextureStrength(neuralTexStrength);
			}

			float neuralLightStrength = Wasteland::Renderer3D::GetNeuralLightStrength();
			if (ImGui::SliderFloat("Neural Light Strength", &neuralLightStrength, 0.0f, 1.0f))
			{
				Wasteland::Renderer3D::SetNeuralLightStrength(neuralLightStrength);
			}

			float neuralMatStrength = Wasteland::Renderer3D::GetNeuralMaterialStrength();
			if (ImGui::SliderFloat("Neural Material Strength", &neuralMatStrength, 0.0f, 1.0f))
			{
				Wasteland::Renderer3D::SetNeuralMaterialStrength(neuralMatStrength);
			}

			ImGui::Separator();
			ImGui::Text("Sky Settings");

			glm::vec3 skyBottomColor = Wasteland::Renderer3D::GetSkyBottomColor();
			float bottomColor[3] = {skyBottomColor.r, skyBottomColor.g, skyBottomColor.b};
			if (ImGui::ColorEdit3("Sky Bottom Color", bottomColor))
			{
				Wasteland::Renderer3D::SetSkyBottomColor(glm::vec3(bottomColor[0], bottomColor[1], bottomColor[2]));
			}

			glm::vec3 skyTopColor = Wasteland::Renderer3D::GetSkyTopColor();
			float topColor[3] = {skyTopColor.r, skyTopColor.g, skyTopColor.b};
			if (ImGui::ColorEdit3("Sky Top Color", topColor))
			{
				Wasteland::Renderer3D::SetSkyTopColor(glm::vec3(topColor[0], topColor[1], topColor[2]));
			}

			ImGui::Separator();
			ImGui::Text("Volumetric Atmosphere");

			bool fogEnabled = Wasteland::Renderer3D::IsVolumetricFogEnabled();
			if (ImGui::Checkbox("Volumetric Fog", &fogEnabled))
			{
				Wasteland::Renderer3D::SetVolumetricFogEnabled(fogEnabled);
			}

			bool cloudsEnabled = Wasteland::Renderer3D::IsVolumetricCloudsEnabled();
			if (ImGui::Checkbox("Volumetric Clouds", &cloudsEnabled))
			{
				Wasteland::Renderer3D::SetVolumetricCloudsEnabled(cloudsEnabled);
			}

			glm::vec3 sunDir = Wasteland::Renderer3D::GetSunDirection();
			float sunArr[3] = {sunDir.x, sunDir.y, sunDir.z};
			if (ImGui::DragFloat3("Sun Direction", sunArr, 0.02f, -1.0f, 1.0f))
			{
				Wasteland::Renderer3D::SetSunDirection(glm::vec3(sunArr[0], sunArr[1], sunArr[2]));
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Fallback sun for fog/clouds. A Directional Light component\nin the scene overrides this (direction + color) and casts god rays.");

			float volQuality = Wasteland::Renderer3D::GetVolumetricStepScale();
			if (ImGui::SliderFloat("Volume Quality", &volQuality, 0.25f, 2.0f))
			{
				Wasteland::Renderer3D::SetVolumetricStepScale(volQuality);
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Global multiplier for per-volume march steps.\nLower it if volumetrics cost too much performance.");

			// Global atmospheric fog controls
			bool globalFog = Wasteland::Renderer3D::IsGlobalFogEnabled();
			if (ImGui::Checkbox("Global Fog", &globalFog))
			{
				Wasteland::Renderer3D::SetGlobalFogEnabled(globalFog);
			}
			if (ImGui::IsItemHovered())
			 ImGui::SetTooltip("Distance-based haze applied to ALL surfaces\n(independent of box volumes).");
			if (Wasteland::Renderer3D::IsGlobalFogEnabled())
			{
				float density = Wasteland::Renderer3D::GetGlobalFogDensity();
				if (ImGui::SliderFloat("  Density", &density, 0.0001f, 0.05f, "%.4f"))
				{
					Wasteland::Renderer3D::SetGlobalFogDensity(density);
				}
				float atten = Wasteland::Renderer3D::GetGlobalFogDistanceAttenuation();
				if (ImGui::SliderFloat("  Distance Atten", &atten, 0.01f, 5.0f, "%.2f"))
				{
					Wasteland::Renderer3D::SetGlobalFogDistanceAttenuation(atten);
				}
				float baseHeight = Wasteland::Renderer3D::GetGlobalFogBaseHeight();
				if (ImGui::SliderFloat("  Base Height", &baseHeight, -100.0f, 500.0f, "%.1f"))
				{
					Wasteland::Renderer3D::SetGlobalFogBaseHeight(baseHeight);
				}
				float heightFalloff = Wasteland::Renderer3D::GetGlobalFogHeightFalloff();
				if (ImGui::SliderFloat("  Height Falloff", &heightFalloff, 0.01f, 10.0f, "%.2f"))
				{
					Wasteland::Renderer3D::SetGlobalFogHeightFalloff(heightFalloff);
				}
			}

			// God rays controls
			bool godRays = Wasteland::Renderer3D::IsGodRaysEnabled();
			if (ImGui::Checkbox("God Rays", &godRays))
			{
				Wasteland::Renderer3D::SetGodRaysEnabled(godRays);
			}
			if (ImGui::IsItemHovered())
			 ImGui::SetTooltip("Screen-space light shafts from the sun.");
			if (Wasteland::Renderer3D::IsGodRaysEnabled())
			{
				float intensity = Wasteland::Renderer3D::GetGodRayIntensity();
				if (ImGui::SliderFloat("  Intensity", &intensity, 0.0f, 2.0f, "%.2f"))
				{
					Wasteland::Renderer3D::SetGodRayIntensity(intensity);
				}
				float decay = Wasteland::Renderer3D::GetGodRayDecay();
				if (ImGui::SliderFloat("  Decay", &decay, 0.0f, 1.0f, "%.2f"))
				{
					Wasteland::Renderer3D::SetGodRayDecay(decay);
				}
				int samples = Wasteland::Renderer3D::GetGodRaySamples();
				if (ImGui::SliderInt("  Samples", &samples, 1, 64))
				{
					Wasteland::Renderer3D::SetGodRaySamples(samples);
				}
				float density = Wasteland::Renderer3D::GetGodRayDensity();
				if (ImGui::SliderFloat("  Density", &density, 0.0f, 5.0f, "%.2f"))
				{
					Wasteland::Renderer3D::SetGodRayDensity(density);
				}
			}

			ImGui::Separator();
			ImGui::Text("Camera Effects (Post-Processing)");
			ImGui::Separator();

			// Bloom
			bool bloomEnabled = Wasteland::Renderer3D::IsBloomEnabled();
			if (ImGui::Checkbox("Bloom", &bloomEnabled))
			{
				Wasteland::Renderer3D::SetBloomEnabled(bloomEnabled);
			}
			if (Wasteland::Renderer3D::IsBloomEnabled())
			{
				float bloomIntensity = Wasteland::Renderer3D::GetBloomIntensity();
				if (ImGui::SliderFloat("  Intensity", &bloomIntensity, 0.0f, 5.0f, "%.2f"))
				{
					Wasteland::Renderer3D::SetBloomIntensity(bloomIntensity);
				}
				float bloomThreshold = Wasteland::Renderer3D::GetBloomThreshold();
				if (ImGui::SliderFloat("  Threshold", &bloomThreshold, 0.0f, 1.0f, "%.2f"))
				{
					Wasteland::Renderer3D::SetBloomThreshold(bloomThreshold);
				}
			}

			// Bilateral Blur (denoise)
			bool bilateralBlurEnabled = Wasteland::Renderer3D::IsBilateralBlurEnabled();
			if (ImGui::Checkbox("Bilateral Blur (Denoise)", &bilateralBlurEnabled))
			{
				Wasteland::Renderer3D::SetBilateralBlurEnabled(bilateralBlurEnabled);
			}
			if (Wasteland::Renderer3D::IsBilateralBlurEnabled())
			{
				int bilateralBlurPasses = Wasteland::Renderer3D::GetBilateralBlurPasses();
				if (ImGui::SliderInt("  Passes", &bilateralBlurPasses, 0, 4))
				{
					Wasteland::Renderer3D::SetBilateralBlurPasses(bilateralBlurPasses);
				}
			}

			ImGui::Separator();
			ImGui::Text("Experimental Nova Reconstruction");
			bool svfgEnabled = Wasteland::Renderer3D::IsSVGFEnabled();
			if (ImGui::Checkbox("SVGF Denoiser", &svfgEnabled))
				Wasteland::Renderer3D::SetSVGFEnabled(svfgEnabled);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Fixed four-step A-Trous denoiser. Experimental; off by default.");

			bool restirEnabled = Wasteland::Renderer3D::IsReSTIREnabled();
			if (ImGui::Checkbox("ReSTIR DI", &restirEnabled))
				Wasteland::Renderer3D::SetReSTIREnabled(restirEnabled);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Experimental temporal diffuse-emissive sampling for the active Nova/GL path. Off by default.");

			ImGui::Separator();

			// Vignette
			bool vignetteEnabled = Wasteland::Renderer3D::IsVignetteEnabled();
			if (ImGui::Checkbox("Vignette", &vignetteEnabled))
			{
				Wasteland::Renderer3D::SetVignetteEnabled(vignetteEnabled);
			}
			if (Wasteland::Renderer3D::IsVignetteEnabled())
			{
				float vignetteIntensity = Wasteland::Renderer3D::GetVignetteIntensity();
				if (ImGui::SliderFloat("  Intensity", &vignetteIntensity, 0.0f, 2.0f, "%.2f"))
				{
					Wasteland::Renderer3D::SetVignetteIntensity(vignetteIntensity);
				}
				float vignetteRadius = Wasteland::Renderer3D::GetVignetteRadius();
				if (ImGui::SliderFloat("  Radius", &vignetteRadius, 0.0f, 1.0f, "%.2f"))
				{
					Wasteland::Renderer3D::SetVignetteRadius(vignetteRadius);
				}
				float vignetteSoftness = Wasteland::Renderer3D::GetVignetteSoftness();
				if (ImGui::SliderFloat("  Softness", &vignetteSoftness, 0.0f, 1.0f, "%.2f"))
				{
					Wasteland::Renderer3D::SetVignetteSoftness(vignetteSoftness);
				}
			}

			ImGui::Separator();

			// Chromatic Aberration
			bool chromaticAberrationEnabled = Wasteland::Renderer3D::IsChromaticAberrationEnabled();
			if (ImGui::Checkbox("Chromatic Aberration", &chromaticAberrationEnabled))
			{
				Wasteland::Renderer3D::SetChromaticAberrationEnabled(chromaticAberrationEnabled);
			}
			if (Wasteland::Renderer3D::IsChromaticAberrationEnabled())
			{
				float chromaticAberrationIntensity = Wasteland::Renderer3D::GetChromaticAberrationIntensity();
				if (ImGui::SliderFloat("  Intensity", &chromaticAberrationIntensity, 0.0f, 1.0f, "%.2f"))
				{
					Wasteland::Renderer3D::SetChromaticAberrationIntensity(chromaticAberrationIntensity);
				}
			}

			ImGui::Separator();

			// Color Grading
			bool colorGradingEnabled = Wasteland::Renderer3D::IsColorGradingEnabled();
			if (ImGui::Checkbox("Color Grading", &colorGradingEnabled))
			{
				Wasteland::Renderer3D::SetColorGradingEnabled(colorGradingEnabled);
			}
			if (Wasteland::Renderer3D::IsColorGradingEnabled())
			{
				float colorTemperature = Wasteland::Renderer3D::GetColorTemperature();
				if (ImGui::SliderFloat("  Temperature", &colorTemperature, -1.0f, 1.0f, "%.2f"))
				{
					Wasteland::Renderer3D::SetColorTemperature(colorTemperature);
				}
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("-1 = cold (blue), 0 = neutral, +1 = warm (orange)");
				float colorTint = Wasteland::Renderer3D::GetColorTint();
				if (ImGui::SliderFloat("  Tint", &colorTint, -1.0f, 1.0f, "%.2f"))
				{
					Wasteland::Renderer3D::SetColorTint(colorTint);
				}
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("-1 = green, 0 = neutral, +1 = magenta");
				float colorGamma = Wasteland::Renderer3D::GetColorGamma();
				if (ImGui::SliderFloat("  Gamma", &colorGamma, 0.1f, 3.0f, "%.2f"))
				{
					Wasteland::Renderer3D::SetColorGamma(colorGamma);
				}
			}

			ImGui::Separator();

			// Film Grain
			bool filmGrainEnabled = Wasteland::Renderer3D::IsFilmGrainEnabled();
			if (ImGui::Checkbox("Film Grain", &filmGrainEnabled))
			{
				Wasteland::Renderer3D::SetFilmGrainEnabled(filmGrainEnabled);
			}
			if (Wasteland::Renderer3D::IsFilmGrainEnabled())
			{
				float filmGrainIntensity = Wasteland::Renderer3D::GetFilmGrainIntensity();
				if (ImGui::SliderFloat("  Intensity", &filmGrainIntensity, 0.0f, 1.0f, "%.2f"))
				{
					Wasteland::Renderer3D::SetFilmGrainIntensity(filmGrainIntensity);
				}
			}

			ImGui::TextWrapped("Add Volumetric Fog / Volumetric Clouds components to an entity; its Transform defines the volume box.");

			ImGui::End();
			}

			if (m_ShowViewport)
			{
				ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0, 0});
				ImGui::Begin("Viewport");

			ImVec2 minBound = ImGui::GetCursorScreenPos();

			m_ViewportFocused = ImGui::IsWindowFocused();
			m_ViewportHovered = ImGui::IsWindowHovered();
			Application::Get().GetImGuiLayer()->BlockEvents(!m_ViewportFocused && !m_ViewportHovered);

			ImVec2 viewportPanelSize = ImGui::GetContentRegionAvail();
			m_ViewportSize = {viewportPanelSize.x, viewportPanelSize.y};

			ImVec2 maxBound = {minBound.x + m_ViewportSize.x, minBound.y + m_ViewportSize.y};
			m_ViewportBounds[0] = {minBound.x, minBound.y};
			m_ViewportBounds[1] = {maxBound.x, maxBound.y};

			// ImGui's TexID is not a texture: it is whatever the ACTIVE backend
			// binds, and that differs per backend (an SRV pointer on DX11, a GPU
			// descriptor handle on DX12, a VkDescriptorSet on Vulkan, a GL name on
			// OpenGL). So the handle is asked of the backend that owns the
			// attachment rather than assembled here from whatever the texture
			// happens to be addressable as -- that is what made DX12 bind a
			// truncated nvrhi handle pointer as a descriptor and draw nothing.
			uint32_t textureID = 0;
			void *textureTexID = nullptr;
			if (Wasteland::Renderer3D::IsRayTracingEnabled())
			{
				textureID = Wasteland::Renderer3D::GetRayTraceTargetID();
				if (RendererAPI::GetAPI() == RendererAPI::API::NVRHI_Vulkan)
				{
					void *rawTexture = Wasteland::Renderer3D::GetRayTraceTargetNativeSRV();
					NVRHIFramebuffer *nvrhiFramebuffer =
						dynamic_cast<NVRHIFramebuffer *>(m_Framebuffer.get());
					if (rawTexture && nvrhiFramebuffer)
					{
						textureTexID = nvrhiFramebuffer->GetExternalTextureImGuiTexID(
							nvrhi::TextureHandle(static_cast<nvrhi::ITexture *>(rawTexture)));
					}
				}
				else
				{
					// Already in the active backend's own form: the DX12 Nova path
					// inside Renderer3D converts its output to a descriptor handle (see
					// GetRayTraceTargetNativeSRV), so no conversion belongs here.
					textureTexID = Wasteland::Renderer3D::GetRayTraceTargetNativeSRV();
				}
			}

			else
			{
				textureID = m_Framebuffer->GetColorAttachmentRendererID();
				textureTexID = m_Framebuffer->GetColorAttachmentImGuiTexID(0);
			}

			// The integer id is a real TexID on OpenGL ONLY, where it IS the GL
			// texture name. Under NVRHI it is a 64-bit texture handle truncated to
			// 32 bits, so using it as a fallback would reinstate the garbage bind
			// this accessor exists to replace. Restricted to GL, a null handle on
			// any other backend means "skip the draw", not "bind this instead".
			if (!textureTexID && RendererAPI::GetAPI() == RendererAPI::API::OpenGL)
				textureTexID = reinterpret_cast<void *>(static_cast<uintptr_t>(textureID));

			// A null TexID makes the backend skip the image. That remains the correct
			// degradation when a backend genuinely cannot hand ImGui a token it
			// binds -- and binding the truncated pointer instead is strictly
			// worse, since the draw would sample whatever descriptor it landed
			// on. All four backends are now expected to supply a real handle:
			// DX11 an SRV pointer, DX12 a heap descriptor, Vulkan a descriptor
			// set from ImGui_ImplVulkan_AddTexture, OpenGL a texture name. A
			// null here therefore means a real failure worth saying out loud.
			if (textureTexID)
			{
				ImGui::Image(textureTexID,
							 ImVec2{m_ViewportSize.x, m_ViewportSize.y},
							 ImVec2{0, 1}, ImVec2{1, 0});
			}
			else
			{
				// ONCE per session rather than per frame: this runs every frame
				// while the viewport is open, and a warning per frame would bury
				// the log rather than explain anything.
				static bool warned = false;
				if (!warned)
				{
					warned = true;
					WL_CORE_WARN("EditorLayer: viewport has no ImGui texture handle on API {0}; "
								 "the image is skipped rather than bound to a truncated handle",
								 (int)RendererAPI::GetAPI());
				}
			}

			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
				{
					// Payload is a UTF-8 project-relative path.
					const char *relative = static_cast<const char *>(payload->Data);
					OpenScene(Project::Get().GetAssetPath(relative));
				}

				ImGui::EndDragDropTarget();
			}

			// Gizmos
			Entity selectedEntity = m_SceneHierarchyPanel.GetSelectedEntity();
			if (m_ActiveScene && selectedEntity && m_GizmoType != -1 && m_ActiveScene->IsEntityValid(selectedEntity))
			{
				ImGuizmo::SetOrthographic(false);
				ImGuizmo::SetDrawlist();
				ImGuizmo::SetRect(m_ViewportBounds[0].x, m_ViewportBounds[0].y, m_ViewportSize.x, m_ViewportSize.y);

				const glm::mat4 &cameraProjection = m_EditorCamera.GetProjection();
				glm::mat4 cameraView = m_EditorCamera.GetViewMatrix();

				auto &tc = selectedEntity.GetComponent<TransformComponent>();
				glm::mat4 transform = tc.GetTransform();

				bool snap = Input::IsKeyPressed(WL_KEY_LEFT_CONTROL);
				float snapValue = 0.5f;
				if (m_GizmoType == ImGuizmo::OPERATION::ROTATE)
					snapValue = 45.0f;

				float snapValues[3] = {snapValue, snapValue, snapValue};

				ImGuizmo::Manipulate(glm::value_ptr(cameraView), glm::value_ptr(cameraProjection),
									 (ImGuizmo::OPERATION)m_GizmoType, ImGuizmo::LOCAL, glm::value_ptr(transform),
									 nullptr, snap ? snapValues : nullptr);

				if (ImGuizmo::IsUsing())
				{
					glm::vec3 translation, rotation, scale;
					Math::DecomposeTransform(transform, translation, rotation, scale);

					glm::vec3 deltaRotation = rotation - tc.Rotation;
					tc.Translation = translation;
					tc.Rotation += deltaRotation;
					tc.Scale = scale;
				}
			}

			ImGui::End();
			ImGui::PopStyleVar();
			}

			UI_Toolbar();

			ImGui::End(); // End DockSpace

			UI_NewScriptModal();
		UI_NewProjectModal();
		}
		else
		{
			ImGui::Begin("Settings");

			auto stats = Renderer2D::GetStats();
			ImGui::Text("Renderer2D Stats:");
			ImGui::Text("Draw Calls: %d", stats.DrawCalls);
			ImGui::Text("Quads: %d", stats.QuadCount);
			ImGui::Text("Vertices: %d", stats.GetTotalVertexCount());
			ImGui::Text("Indices: %d", stats.GetTotalIndexCount());

			ImGui::Separator();

			auto stats3D = Renderer3D::GetStats();
			ImGui::Text("Renderer3D Stats:");
			ImGui::Text("Draw Calls: %d", stats3D.DrawCalls);
			ImGui::Text("Quads: %d", stats3D.QuadCount);
			ImGui::Text("Vertices: %d", stats3D.GetTotalVertexCount());

			bool rtEnabled = Renderer3D::IsRayTracingEnabled();
			if (ImGui::Checkbox("Nova Rendering Pipeline", &rtEnabled))
			{
				Renderer3D::SetRayTracingEnabled(rtEnabled);
			}
			ImGui::Text("Indices: %d", stats3D.GetTotalIndexCount());

			ImGui::ColorEdit4("Square Color", glm::value_ptr(m_SquareColor));

			uint32_t textureID = m_CheckerboardTexture->GetRendererID();
			void *srv = m_CheckerboardTexture->GetNativeSRV();
			ImGui::Image(srv ? srv : (void *)textureID, ImVec2{1280.0f, 720.0f}, ImVec2{0, 1}, ImVec2{1, 0});
			ImGui::End();
		}
	}

	void EditorLayer::UI_Toolbar()
	{
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 2));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(0, 0));

		ImGui::Begin("##toolbar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

		float size = ImGui::GetWindowHeight() - 4.0f;
		float totalWidth = (size * 2.0f) + ImGui::GetStyle().ItemSpacing.x;
		ImGui::SameLine((ImGui::GetWindowContentRegionMax().x * 0.5f) - (totalWidth * 0.5f));
		if (ImGui::Button("Play"))
		{
			if (m_SceneState == SceneState::Edit)
				OnScenePlay();
		}

		ImGui::SameLine();
		if (ImGui::Button("Simulate"))
		{
			if (m_SceneState == SceneState::Edit)
				OnSceneSimulate();
		}

		ImGui::SameLine();
		if (ImGui::Button("Stop"))
		{
			if (m_SceneState == SceneState::Play)
				OnSceneStop();
			else if (m_SceneState == SceneState::Simulate)
				OnSceneStop();
		}

		ImGui::PopStyleVar();
		ImGui::PopStyleVar();

		ImGui::End();
	}

	void EditorLayer::UI_NewScriptModal()
	{
		if (m_ShowNewScriptModal)
			ImGui::OpenPopup("New Script");

		if (ImGui::BeginPopupModal("New Script", &m_ShowNewScriptModal, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::Text("Enter script name:");
			ImGui::InputText("##ScriptName", m_NewScriptBuffer, IM_ARRAYSIZE(m_NewScriptBuffer));

			if (ImGui::Button("Create", ImVec2(120, 0)))
			{
				if (strlen(m_NewScriptBuffer) > 0)
				{
					std::filesystem::path scriptDir = Project::Get().GetAssetPath("scripts");
					if (!std::filesystem::exists(scriptDir))
						std::filesystem::create_directories(scriptDir);

					std::string fileName = m_NewScriptBuffer;
					if (fileName.find(".py") == std::string::npos)
						fileName += ".py";

					std::filesystem::path scriptPath = scriptDir / fileName;
					std::ofstream outFile(scriptPath);

					if (outFile.is_open())
					{
						outFile << "# Wasteland Engine Script\n";
						outFile << "import sys\n";
						outFile << "import os\n";
						outFile << "sys.path.append(os.path.dirname(__file__))\n";
						outFile << "import Wasteland\n";
						outFile << "\n";
						outFile << "class " << m_NewScriptBuffer << ":\n";
						outFile << "	def __init__(self):\n";
						outFile << "		pass\n";
						outFile << "\n";
						outFile << "	def OnUpdateEntity(self, dt):\n";
						outFile << "		pass\n";
						outFile << "\n";
						outFile.close();
						WL_CORE_INFO("Created new script at: {0}", scriptPath.string());
						m_ScriptEditorPanel.OpenFile(scriptPath);
					}
				}
				m_ShowNewScriptModal = false;
				ImGui::CloseCurrentPopup();
			}

			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(120, 0)))
			{
				m_ShowNewScriptModal = false;
				ImGui::CloseCurrentPopup();
			}

			ImGui::EndPopup();
		}
	}

	void EditorLayer::OnEvent(Event &e)
	{
		m_CameraController.OnEvent(e);
		m_EditorCamera.OnEvent(e);

		EventDispatcher dispatcher(e);
		dispatcher.Dispatch<KeyPressedEvent>(WL_BIND_EVENT_FN(EditorLayer::OnKeyPressed));
		dispatcher.Dispatch<MouseButtonPressedEvent>(WL_BIND_EVENT_FN(EditorLayer::OnMouseButtonPressed));
	}

	bool EditorLayer::OnKeyPressed(KeyPressedEvent &e)
	{
		if (e.GetRepeatCount() > 0)
			return false;

		bool control = Input::IsKeyPressed(WL_KEY_LEFT_CONTROL) || Input::IsKeyPressed(WL_KEY_RIGHT_CONTROL);
		bool shift = Input::IsKeyPressed(WL_KEY_LEFT_SHIFT) || Input::IsKeyPressed(WL_KEY_RIGHT_SHIFT);
		switch (e.GetKeyCode())
		{
		case WL_KEY_N:
		{
			if (control)
				NewScene();
			break;
		}
		case WL_KEY_O:
		{
			if (control)
				OpenScene();
			break;
		}
		case WL_KEY_S:
		{
			if (control)
			{
				if (shift)
					SaveSceneAs();
				else
					SaveScene();
			}
			break;
		}
		case WL_KEY_D:
		{
			if (control)
				OnDuplicateEntity();
			break;
		}
		case WL_KEY_Q:
			m_GizmoType = -1;
			break;
		case WL_KEY_W:
			m_GizmoType = ImGuizmo::OPERATION::TRANSLATE;
			break;
		case WL_KEY_E:
			m_GizmoType = ImGuizmo::OPERATION::ROTATE;
			break;
		case WL_KEY_R:
			m_GizmoType = ImGuizmo::OPERATION::SCALE;
			break;
		case WL_KEY_ESCAPE:
			if (m_SceneState == SceneState::Play)
			{
				m_IsPaused = !m_IsPaused;
				// Escape is the player's way out of a fight, so it has to give
				// the cursor back -- a paused game with an invisible pointer is
				// a dead end, since ImGui cannot be clicked either. Re-entering
				// the unpaused state re-captures.
				Application::Get().GetWindow().SetMouseCaptured(!m_IsPaused);
			}
			break;
		}
		return false;
	}

	bool EditorLayer::OnMouseButtonPressed(MouseButtonPressedEvent &e)
	{
		if (e.GetMouseButton() == WL_MOUSE_BUTTON_LEFT)
		{
			if (m_IsPaused && m_ViewportHovered)
			{
				m_IsPaused = false;
				return false;
			}

			if (m_ViewportHovered && !ImGuizmo::IsOver() && !Input::IsKeyPressed(WL_KEY_LEFT_ALT))
				m_SceneHierarchyPanel.SetSelectedEntity(m_HoveredEntity);
		}
		return false;
	}

	void EditorLayer::RenderGameplayHUD()
	{
		if (!m_ActiveScene)
			return;

		// The gameplay actors are attached to the primary camera entity (that
		// is the entity a Player.py/Weapon.py pair runs on), so the HUD reads
		// from the same place the scripts write to.
		Entity actor = m_ActiveScene->GetPrimaryCameraEntity();
		if (!actor || !actor.IsValid())
			return;

		ImGuiIO &io = ImGui::GetIO();
		ImVec2 display = io.DisplaySize;

		// ---- Crosshair: a small gap-plus-dot at screen centre. Drawn with
		// the foreground draw list so it needs no window of its own. ----
		{
			// DisplaySize is already in screen space, so the draw-list origin
			// is the viewport's top-left corner (the editor passes a viewport
			// offset through ImGuiConfigFlags::ViewportsOffset, which this
			// draw list does not carry).
			ImVec2 center(display.x * 0.5f, display.y * 0.5f);

			const float gap = 6.0f;
			const float len = 7.0f;
			const float thickness = 1.5f;
			ImU32 col = actor.HasComponent<HealthComponent>() &&
								actor.GetComponent<HealthComponent>().IsDead
							? IM_COL32(180, 40, 40, 220)
							: IM_COL32(255, 255, 255, 220);

			ImDrawList *drawList = ImGui::GetBackgroundDrawList();
			drawList->AddLine(ImVec2(center.x - gap - len, center.y), ImVec2(center.x - gap, center.y), col, thickness);
			drawList->AddLine(ImVec2(center.x + gap, center.y), ImVec2(center.x + gap + len, center.y), col, thickness);
			drawList->AddLine(ImVec2(center.x, center.y - gap - len), ImVec2(center.x, center.y - gap), col, thickness);
			drawList->AddLine(ImVec2(center.x, center.y + gap), ImVec2(center.x, center.y + gap + len), col, thickness);

			// ---- Hit marker: four diagonal ticks that appear on a confirmed
			// hit. Distinct from the crosshair in shape, not just colour, so it
			// still reads for a colour-blind player. ----
			//
			// Timed from WeaponFeedbackComponent, which Weapon.py writes. The
			// HUD only renders it; it never decides whether a shot hit, so
			// there is no second opinion on that to disagree with.
			if (actor.TryGetComponent<WeaponFeedbackComponent>() &&
				actor.GetComponent<WeaponFeedbackComponent>().HitMarkerTimer > 0.0f)
			{
				WeaponFeedbackComponent &fb = actor.GetComponent<WeaponFeedbackComponent>();

				// Fade out over the marker's life rather than popping off at
				// the end. `fraction` is how much of the duration is left, so
				// it falls 1 -> 0 across the effect.
				float fraction = fb.HitMarkerDuration > 0.0f
									 ? fb.HitMarkerTimer / fb.HitMarkerDuration
									 : 0.0f;
				fraction = std::max(0.0f, std::min(1.0f, fraction));

				// A kill is a bigger, red marker; a body hit is small and
				// white. One extra bool in the component buys the distinction.
				const float markerLen = fb.LastShotKilled ? 11.0f : 7.0f;
				const float markerGap = fb.LastShotKilled ? 5.0f : 8.0f;
				ImU32 markerCol = fb.LastShotKilled
									  ? IM_COL32(255, 70, 70, static_cast<int>(255.0f * fraction))
									  : IM_COL32(255, 255, 255, static_cast<int>(235.0f * fraction));

				const float d = markerGap + markerLen;
				const ImVec2 c = center;
				drawList->AddLine(ImVec2(c.x - d, c.y - d), ImVec2(c.x - markerGap, c.y - markerGap), markerCol, 2.0f);
				drawList->AddLine(ImVec2(c.x + markerGap, c.y - markerGap), ImVec2(c.x + d, c.y - d), markerCol, 2.0f);
				drawList->AddLine(ImVec2(c.x - d, c.y + d), ImVec2(c.x - markerGap, c.y + markerGap), markerCol, 2.0f);
				drawList->AddLine(ImVec2(c.x + markerGap, c.y + markerGap), ImVec2(c.x + d, c.y + d), markerCol, 2.0f);
			}

			// ---- Muzzle flash ----
			// A warm bloom at the bottom-centre of the screen, sized as a
			// fraction of the viewport so it scales with resolution instead
			// of being a fixed pixel blob on a 4K display.
			//
			// Drawn in screen space rather than in the world: a world-space
			// flash would need a model or particle system, and what the player
			// needs to see is that the gun went off, not where the barrel is
			// pointing -- the barrel is off-screen in a first-person view.
			if (actor.TryGetComponent<WeaponFeedbackComponent>() &&
				actor.GetComponent<WeaponFeedbackComponent>().MuzzleFlashTimer > 0.0f)
			{
				WeaponFeedbackComponent &fb = actor.GetComponent<WeaponFeedbackComponent>();
				float flash = fb.MuzzleFlashDuration > 0.0f
								  ? fb.MuzzleFlashTimer / fb.MuzzleFlashDuration
								  : 0.0f;
				flash = std::max(0.0f, std::min(1.0f, flash));

				// Additive blend: a flash should ADD light. The overlay draw
				// list draws on top with alpha blending, which dims as it
				// fades -- the opposite of what a muzzle flash should do. So
				// the strength is carried in the colour intensity and the
				// alpha stays low, giving a brief warm pop rather than a grey
				// veil over the screen.
				const float radius = std::min(display.x, display.y) * (0.10f + 0.06f * flash);
				const ImVec2 flashCenter(display.x * 0.5f, display.y * 0.82f);
				// Stack three falloff rings: a wide dim halo, a mid ring, and
				// a small hot core. One circle reads as a flat disc; the
				// layering is what makes it read as light.
				for (int i = 0; i < 3; ++i)
				{
					float ringScale = 1.0f - static_cast<float>(i) * 0.3f;
					ImU32 flashCol = IM_COL32(255, static_cast<int>(200 - 40 * i),
											 static_cast<int>(90 - 30 * i), static_cast<int>(90.0f * flash * ringScale));
					drawList->AddCircleFilled(flashCenter, radius * ringScale, flashCol, 24);
				}
			}

			// ---- Sprint / aim screen tint ----
			// A subtle edge vignette keyed to the resolved view state, so the
			// player can feel sprinting and aiming peripherally without having
			// to look at the HUD. Read from ViewStateComponent because the
			// player script is the only thing that can resolve "shift is down"
			// into "actually sprinting".
			if (actor.TryGetComponent<ViewStateComponent>())
			{
				ViewStateComponent &view = actor.GetComponent<ViewStateComponent>();
				if (view.IsSprinting || view.IsAiming)
				{
					// Aiming tints cool and tightens inward; sprinting tints
					// warm and pushes outward. Opposite colours so the two read
					// as different states rather than "some effect".
					const ImU32 tint = view.IsAiming ? IM_COL32(120, 170, 255, 34)
													 : IM_COL32(255, 170, 90, 30);
					const float inset = view.IsAiming ? display.x * 0.18f : 0.0f;
					// Four edge bars rather than a real gradient: ImGui has no
					// cheap way to do a radial vignette, and bars read fine at
					// low alpha.
					const float barH = display.y * 0.035f;
					drawList->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(display.x, barH), tint);
					drawList->AddRectFilled(ImVec2(0.0f, display.y - barH), ImVec2(display.x, display.y), tint);
					if (inset > 0.0f)
					{
						const float barW = display.x * 0.02f;
						drawList->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(barW, display.y), tint);
						drawList->AddRectFilled(ImVec2(display.x - barW, 0.0f), ImVec2(display.x, display.y), tint);
					}
				}
			}
		}

		// ---- Health / ammo panel, bottom-left ----
		if (actor.HasComponent<HealthComponent>() || actor.HasComponent<WeaponComponent>())
		{
			const ImVec2 pad(16.0f, 16.0f);
			ImGui::SetNextWindowPos(ImVec2(pad.x, display.y - 190.0f), ImGuiCond_Always);
			ImGui::SetNextWindowBgAlpha(0.55f);
			// Second arg is p_open; a null pointer means the window has no
			// close button and the return value is ignored.
			ImGui::Begin("##GameplayHUD", nullptr,
						 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
							 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
							 ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
							 ImGuiWindowFlags_NoNav);

			if (actor.HasComponent<HealthComponent>())
			{
				const auto &health = actor.GetComponent<HealthComponent>();
				float frac = health.Max > 0.0f ? health.Current / health.Max : 0.0f;
				frac = glm::clamp(frac, 0.0f, 1.0f);

				// Green when healthy, red as it drains, so the bar itself
				// communicates the state without needing to read the number.
				ImVec4 tint = frac > 0.5f ? ImVec4(0.25f, 0.85f, 0.35f, 1.0f)
								: frac > 0.25f ? ImVec4(0.95f, 0.75f, 0.2f, 1.0f)
											   : ImVec4(0.9f, 0.25f, 0.25f, 1.0f);

				ImGui::Text("Health");
				ImGui::PushStyleColor(ImGuiCol_PlotHistogram, tint);
				ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f));
				ImGui::PopStyleColor();
				ImGui::Text("%.0f / %.0f", health.Current, health.Max);
			}

			if (actor.HasComponent<WeaponComponent>())
			{
				const auto &weapon = actor.GetComponent<WeaponComponent>();
				ImGui::Separator();
				if (weapon.IsReloading)
				{
					// Reloading is a timed state, so show it as one rather
					// than as a number that silently changes.
					float frac = weapon.ReloadTime > 0.0f
									  ? 1.0f - (weapon.ReloadTimer / weapon.ReloadTime)
									  : 1.0f;
					ImGui::Text("Reloading...");
					ImGui::ProgressBar(glm::clamp(frac, 0.0f, 1.0f), ImVec2(-1.0f, 0.0f));
				}
				else
				{
					ImGui::Text("Ammo  %d / %d", weapon.CurrentAmmo, weapon.MaxAmmo);
					ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
									   "Reserve  %d", weapon.ReserveAmmo);
				}
			}

			ImGui::End();
		}

		// ---- Interaction prompt, just under the crosshair ----
		// Drawn when the player is looking at an InteractableComponent.
		// Which entity that is, and what it should say, is gameplay state
		// owned by Interactor.py; the HUD only renders the nearest one.
		// Viewing through InteractableComponent means only opted-in entities
		// are visited, rather than scanning every entity in the scene.
		for (auto entityHandle : m_ActiveScene->GetAllEntitiesWith<Wasteland::InteractableComponent>())
		{
			Entity candidate(entityHandle, m_ActiveScene.get());
			if (!candidate.IsValid())
				continue;
			const auto *interactable = candidate.TryGetComponent<Wasteland::InteractableComponent>();
			if (!interactable || !interactable->Enabled || interactable->Label.empty())
				continue;

			ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f + 46.0f),
									ImGuiCond_Always, ImVec2(0.5f, 0.0f));
			ImGui::SetNextWindowBgAlpha(0.0f);
			ImGui::Begin("##InteractPrompt", nullptr,
						 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
							 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
							 ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
							 ImGuiWindowFlags_NoNav);
			ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "%s", interactable->Label.c_str());
			ImGui::End();
			break;  // nearest one wins; the interactor only ever labels one
		}

		// ---- Death banner, centred ----
		if (actor.HasComponent<HealthComponent>() && actor.GetComponent<HealthComponent>().IsDead)
		{
			ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.35f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
			ImGui::SetNextWindowBgAlpha(0.0f);
			ImGui::Begin("##DeathBanner", nullptr,
						 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
							 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
							 ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
							 ImGuiWindowFlags_NoNav);

			// Scaled to the viewport so it reads the same at 1080p and 4K.
			float scale = glm::clamp(display.y / 1080.0f, 0.75f, 3.0f);
			ImGui::SetWindowFontScale(scale);
			ImGui::TextColored(ImVec4(0.86f, 0.13f, 0.23f, 1.0f), "YOU DIED");
			ImGui::SetWindowFontScale(1.0f);

			ImGui::End();
		}
	}

	void EditorLayer::OnOverlayRender()
	{
		if (m_SceneState == SceneState::Play)
		{
			Entity camera = m_ActiveScene->GetPrimaryCameraEntity();
			if (!camera)
				return;

			Renderer2D::BeginScene(camera.GetComponent<CameraComponent>().Camera, camera.GetComponent<TransformComponent>().GetTransform());
			Renderer3D::BeginScene(camera.GetComponent<CameraComponent>().Camera, camera.GetComponent<TransformComponent>().GetTransform());
		}
		else
		{
			Renderer2D::BeginScene(m_EditorCamera);
			Renderer3D::BeginScene(m_EditorCamera);
		}

		if (m_ShowPhysicsColliders)
		{
			// Box Colliders
			{
				auto view = m_ActiveScene->GetAllEntitiesWith<TransformComponent, BoxCollider2DComponent>();
				for (auto entity : view)
				{
					auto [tc, bc2d] = view.get<TransformComponent, BoxCollider2DComponent>(entity);

					glm::vec3 translation = tc.Translation + glm::vec3(bc2d.Offset, 0.001f);
					glm::vec3 scale = tc.Scale * glm::vec3(bc2d.Size * 2.0f, 1.0f);

					glm::mat4 transform = glm::translate(glm::mat4(1.0f), translation) * glm::rotate(glm::mat4(1.0f), tc.Rotation.z, glm::vec3(0.0f, 0.0f, 1.0f)) * glm::scale(glm::mat4(1.0f), scale);

					Renderer2D::DrawRect(transform, glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
				}
			}

			// Circle Colliders
			{
				auto view = m_ActiveScene->GetAllEntitiesWith<TransformComponent, CircleCollider2DComponent>();
				for (auto entity : view)
				{
					auto [tc, cc2d] = view.get<TransformComponent, CircleCollider2DComponent>(entity);

					glm::vec3 translation = tc.Translation + glm::vec3(cc2d.Offset, 0.001f);
					glm::vec3 scale = tc.Scale * glm::vec3(cc2d.Radius * 2.0f);

					glm::mat4 transform = glm::translate(glm::mat4(1.0f), translation) * glm::scale(glm::mat4(1.0f), scale);

					Renderer2D::DrawCircle(transform, glm::vec4(0.0f, 1.0f, 0.0f, 1.0f), 0.015f);
				}
			}
		}

		Renderer2D::EndScene();
	}

	void EditorLayer::NewScene()
	{
		m_ActiveScene = CreateRef<Scene>();
		m_ActiveScene->OnViewportResize((uint32_t)m_ViewportSize.x, (uint32_t)m_ViewportSize.y);
		m_SceneHierarchyPanel.SetContext(m_ActiveScene);

		m_EditorScenePath = std::filesystem::path();
	}

	void EditorLayer::OpenScene()
	{
		std::string filepath = FileDialogs::OpenFile("Wasteland Scene (*.wastescene)\0*.wastescene\0");
		if (!filepath.empty())
		{
			OpenScene(filepath);
		}
	}

	void EditorLayer::OpenScene(const std::filesystem::path &path)
	{
		if (m_SceneState != SceneState::Edit)
			OnSceneStop();

		Ref<Scene> newScene = CreateRef<Scene>();
		newScene->OnViewportResize((uint32_t)m_ViewportSize.x, (uint32_t)m_ViewportSize.y);

		m_SceneHierarchyPanel.SetContext(newScene);

		SceneSerializer serializer(newScene);
		if (serializer.Deserialize(path.string()))
		{
			m_EditorScene = newScene;
			m_EditorScene->OnViewportResize((uint32_t)m_ViewportSize.x, (uint32_t)m_ViewportSize.y);
			m_SceneHierarchyPanel.SetContext(m_EditorScene);

			m_ActiveScene = m_EditorScene;
			m_EditorScenePath = path;
		}
	}

	void EditorLayer::SaveScene()
	{
		if (!m_EditorScenePath.empty())
			SerializeScene(m_ActiveScene, m_EditorScenePath);
		else
			SaveSceneAs();
	}

	void EditorLayer::SaveSceneAs()
	{
		std::string filepath = FileDialogs::SaveFile("Wasteland Scene (*.wastescene)\0*.wastescene\0");
		if (!filepath.empty())
		{
			if (SerializeScene(m_ActiveScene, filepath))
				m_EditorScenePath = filepath;
		}
	}

	void EditorLayer::NewScript()
	{
		m_ShowNewScriptModal = true;
		memset(m_NewScriptBuffer, 0, sizeof(m_NewScriptBuffer));
	}

	void EditorLayer::OpenScript()
	{
		std::string filepath = FileDialogs::OpenFile("Python Script (*.py)\0*.py\0");

		if (!filepath.empty())
		{
			m_ScriptEditorPanel.OpenFile(filepath);
			WL_CORE_INFO("Opened script in editor: {0}", filepath);
		}
	}

	void EditorLayer::NewTerrainAsset()
	{
		Project& project = Project::Get();
		if (!project.IsOpen())
		{
			WL_CORE_WARN("Create a project before creating a terrain asset");
			return;
		}

		const std::string filepath = FileDialogs::SaveFile(
			"Wasteland Terrain (*.wlterrain)\0*.wlterrain\0");
		if (filepath.empty())
			return;

		m_TerrainAssetPanel.SetVisible(true);
		m_TerrainAssetPanel.CreateNewAsset(filepath, project.GetAssetRoot());
	}

	void EditorLayer::NewProject()
	{
		m_ShowNewProjectModal = true;
		m_ProjectError.clear();
		memset(m_NewProjectBuffer, 0, sizeof(m_NewProjectBuffer));
	}

	void EditorLayer::OpenProject()
	{
		std::string filepath = FileDialogs::OpenFile("Wasteland Project (*.wasteland)\0*.wasteland\0\0All files (*.*)\0*.*\0\0");
		if (!filepath.empty())
			OpenProject(filepath);
	}

	void EditorLayer::OpenProject(const std::filesystem::path &path)
	{
		// A project switch invalidates the open scene, so leave play/simulate
		// mode first rather than leaving the layer half in a runtime state.
		if (m_SceneState != SceneState::Edit)
			OnSceneStop();

		std::string error;
		if (!Project::Get().Open(path, &error))
		{
			m_ProjectError = error;
			WL_CORE_ERROR("EditorLayer: could not open project '{0}': {1}", path.string(), error);
			return;
		}

		OnProjectChanged();
	}

	// Pushes the project's persisted render settings into the renderer.
	//
	// These values used to live only as Renderer3D statics, so a reopened project
	// silently reverted to engine defaults -- render scale, the entire
	// post-processing chain and the ray-tracing toggle were all lost. Applied
	// through the same setters the UI uses, so there is one code path that can
	// change any of them.
	void EditorLayer::ApplyRenderSettings()
	{
		const Project::Settings::RenderSettings &r = Project::Get().GetSettings().Render;

		// RenderScale 0 is the "never chosen" sentinel; forcing 0 would collapse
		// the render target, so leave the engine default alone.
		if (r.RenderScale > 0.0f)
			Renderer3D::SetRenderScale(r.RenderScale);

		Renderer3D::SetBloomEnabled(r.BloomEnabled);
		Renderer3D::SetBloomThreshold(r.BloomThreshold);
		Renderer3D::SetBloomIntensity(r.BloomIntensity);

		Renderer3D::SetVignetteEnabled(r.VignetteEnabled);
		Renderer3D::SetVignetteIntensity(r.VignetteIntensity);
		Renderer3D::SetVignetteRadius(r.VignetteRadius);
		Renderer3D::SetVignetteSoftness(r.VignetteSoftness);

		Renderer3D::SetChromaticAberrationEnabled(r.ChromaticAberrationEnabled);
		Renderer3D::SetChromaticAberrationIntensity(r.ChromaticAberrationIntensity);

		Renderer3D::SetColorGradingEnabled(r.ColorGradingEnabled);
		Renderer3D::SetColorTemperature(r.ColorTemperature);
		Renderer3D::SetColorTint(r.ColorTint);
		Renderer3D::SetColorGamma(r.ColorGamma);

		Renderer3D::SetFilmGrainEnabled(r.FilmGrainEnabled);
		Renderer3D::SetFilmGrainIntensity(r.FilmGrainIntensity);

		// Last: it forces a resize, so it happens after the rest.
		if (r.HasRayTracingValue)
			Renderer3D::SetRayTracingEnabled(r.RayTracingEnabled);
	}

	// Reads the renderer's current render settings back into the project, so the
	// next Save() writes what is actually on screen.
	void EditorLayer::CaptureRenderSettings()
	{
		Project::Settings::RenderSettings &r = Project::Get().GetSettings().Render;

		r.RenderScale = Renderer3D::GetRenderScale();
		r.RayTracingEnabled = Renderer3D::IsRayTracingEnabled();
		r.HasRayTracingValue = true;

		r.BloomEnabled = Renderer3D::GetBloomEnabled();
		r.BloomThreshold = Renderer3D::GetBloomThreshold();
		r.BloomIntensity = Renderer3D::GetBloomIntensity();

		r.VignetteEnabled = Renderer3D::GetVignetteEnabled();
		r.VignetteIntensity = Renderer3D::GetVignetteIntensity();
		r.VignetteRadius = Renderer3D::GetVignetteRadius();
		r.VignetteSoftness = Renderer3D::GetVignetteSoftness();

		r.ChromaticAberrationEnabled = Renderer3D::GetChromaticAberrationEnabled();
		r.ChromaticAberrationIntensity = Renderer3D::GetChromaticAberrationIntensity();

		r.ColorGradingEnabled = Renderer3D::GetColorGradingEnabled();
		r.ColorTemperature = Renderer3D::GetColorTemperature();
		r.ColorTint = Renderer3D::GetColorTint();
		r.ColorGamma = Renderer3D::GetColorGamma();

		r.FilmGrainEnabled = Renderer3D::GetFilmGrainEnabled();
		r.FilmGrainIntensity = Renderer3D::GetFilmGrainIntensity();
	}

	void EditorLayer::OnProjectChanged()
	{
		Project &project = Project::Get();

		// The content browser was browsing the previous project's tree.
		m_ContentBrowserPanel.ResetToRoot();
		m_TerrainAssetPanel.Reset();

		// Entities hold absolute texture paths resolved from the old asset
		// root, so the open scene has to go. Reload the project's start
		// scene when it names one.
		NewScene();

		if (!project.GetSettings().StartScene.empty())
		{
			const std::filesystem::path startScene =
				project.GetAssetPath(project.GetSettings().StartScene);
			if (std::filesystem::is_regular_file(startScene))
			{
				OpenScene(startScene);
				WL_CORE_INFO("EditorLayer: loaded start scene {0}", startScene.string());
			}
			else
			{
				WL_CORE_WARN("EditorLayer: start scene '{0}' not found", startScene.string());
			}
		}

		// Same reason as the scene load above: OnProjectChanged already ran
		// before this layer existed, so the render settings it applies never
		// were. Without this, a project opened at startup comes up on
		// engine-default post-processing and render scale regardless of
		// what the project file says.
		ApplyRenderSettings();

		// The tool server reads and writes files relative to the project.
		ToolServer::Instance().SetScene(m_ActiveScene.get());
		ToolServer::Instance().SetProjectChangedCallback([this]() { OnProjectChanged(); });

		// Apply AFTER the callbacks are wired, so a project opened by the tool
		// server gets the same treatment as one opened from the menu.
		ApplyRenderSettings();

		WL_CORE_INFO("EditorLayer: switched to project '{0}'", project.GetName());
	}

	void EditorLayer::UI_NewProjectModal()
	{
		if (m_ShowNewProjectModal)
			ImGui::OpenPopup("New Project");

		if (ImGui::BeginPopupModal("New Project", &m_ShowNewProjectModal, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::Text("Project name:");
			ImGui::InputText("##ProjectName", m_NewProjectBuffer, IM_ARRAYSIZE(m_NewProjectBuffer));

			// Show where it will actually land -- Documents is often
			// redirected to OneDrive, so a hardcoded %USERPROFILE% guess
			// would be wrong on most installs.
			const std::string safeName = Project::SanitizeName(m_NewProjectBuffer);
			if (!safeName.empty())
			{
				const std::filesystem::path target = Project::GetUserProjectsDirectory() / safeName;
				ImGui::TextDisabled("Location: %s", target.string().c_str());
			}

			if (!m_ProjectError.empty())
				ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", m_ProjectError.c_str());

			if (ImGui::Button("Create", ImVec2(120, 0)))
			{
				if (strlen(m_NewProjectBuffer) > 0)
				{
					// Seed from the editor's own assets so the new project
					// starts with the engine's built-in shaders; without them
					// the renderer would fail to find its shaders.
					const std::filesystem::path seed = Project::Get().GetAssetRoot();

					std::string error;
					if (Project::Create(m_NewProjectBuffer, seed, &error))
					{
						OnProjectChanged();
						m_ShowNewProjectModal = false;
						ImGui::CloseCurrentPopup();
					}
					else
					{
						// Leave the modal open so the error is readable and
						// the name can be corrected.
						m_ProjectError = error;
					}
				}
			}

			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(120, 0)))
			{
				m_ShowNewProjectModal = false;
				m_ProjectError.clear();
				ImGui::CloseCurrentPopup();
			}

			ImGui::EndPopup();
		}
	}

	void EditorLayer::UI_ProjectMenu()
	{
		Project &project = Project::Get();

		if (ImGui::BeginMenu("Project"))
		{
			if (ImGui::MenuItem("New Project..."))
				NewProject();

			if (ImGui::MenuItem("Open Project..."))
				OpenProject();

			ImGui::Separator();

			if (ImGui::BeginMenu("Recent", !Project::GetRecentProjects().empty()))
			{
				for (const std::string &recent : Project::GetRecentProjects())
				{
					// Show just the project folder name; the full path is
					// long enough to push the menu off screen.
					std::filesystem::path nameOnly = std::filesystem::path(recent).parent_path().filename();
					if (ImGui::MenuItem(nameOnly.string().c_str()))
						OpenProject(recent);
				}
				ImGui::EndMenu();
			}

			ImGui::Separator();

			if (project.IsOpen())
			{
				ImGui::MenuItem(project.GetName().c_str(), nullptr, false, false);

				if (ImGui::MenuItem("Save Project Settings"))
					// Pull the live render state into the project first, so the file
					// records what is actually on screen rather than whatever was
					// loaded at startup.
					CaptureRenderSettings();
					project.Save();

				if (ImGui::MenuItem("Reveal in File Browser"))
				{
#if defined(_WIN32)
					// "explorer /select" focuses the folder in Explorer.
					const std::string command = "explorer.exe \"" + project.GetRoot().string() + "\"";
					system(command.c_str());
#elif defined(__APPLE__)
					const std::string command = "open \"" + project.GetRoot().string() + "\"";
					system(command.c_str());
#else
					const std::string command = "xdg-open \"" + project.GetRoot().string() + "\"";
					system(command.c_str());
#endif
				}
			}
			else
			{
				ImGui::MenuItem("No project open", nullptr, false, false);
			}

			ImGui::EndMenu();
		}
	}

	bool EditorLayer::SerializeScene(Ref<Scene> scene, const std::filesystem::path &path)
	{
		SceneSerializer serializer(scene);
		std::string error;
		if (!serializer.SerializeChecked(path.string(), &error))
		{
			WL_CORE_ERROR("Failed to save scene '{0}': {1}", path.string(), error);
			return false;
		}
		return true;
	}

	void EditorLayer::OnScenePlay()
	{
		if (m_SceneState == SceneState::Simulate)
			OnSceneStop();

		m_IsPaused = false;
		m_SceneState = SceneState::Play;

		m_ActiveScene = Scene::Copy(m_EditorScene);
		m_ActiveScene->OnRuntimeStart();

		m_SceneHierarchyPanel.SetContext(m_ActiveScene);

		// Grab the cursor for first-person look. Done here rather than in the
		// player script because the editor owns the play-mode transition and
		// therefore owns the paired release -- a script that only captured
		// would strand the user with an invisible pointer if it failed to
		// load, errored, or the scene had no player at all.
		Application::Get().GetWindow().SetMouseCaptured(true);
	}

	void EditorLayer::OnSceneSimulate()
	{
		if (m_SceneState == SceneState::Play)
			OnSceneStop();

		m_IsPaused = false;
		m_SceneState = SceneState::Simulate;

		m_ActiveScene = Scene::Copy(m_EditorScene);
		m_ActiveScene->OnSimulationStart();

		m_SceneHierarchyPanel.SetContext(m_ActiveScene);
	}

	void EditorLayer::OnSceneStop()
	{
		WL_CORE_ASSERT(m_SceneState == SceneState::Play || m_SceneState == SceneState::Simulate, "OnSceneStop called outside Play or Simulate!");

		m_IsPaused = false;

		if (m_SceneState == SceneState::Play)
			m_ActiveScene->OnRuntimeStop();
		else if (m_SceneState == SceneState::Simulate)
			m_ActiveScene->OnSimulationStop();

		m_SceneState = SceneState::Edit;

		m_ActiveScene = m_EditorScene;

		m_SceneHierarchyPanel.SetContext(m_ActiveScene);

		// Release the cursor taken in OnScenePlay. Without this the user is
		// left with an invisible pointer and has to kill the process to get
		// it back. Done unconditionally rather than only for the Play branch,
		// because entering Simulate from Play also passes through here.
		Application::Get().GetWindow().SetMouseCaptured(false);
	}

	void EditorLayer::OnDuplicateEntity()
	{
		if (m_SceneState != SceneState::Edit)
			return;

		Entity selectedEntity = m_SceneHierarchyPanel.GetSelectedEntity();
		if (selectedEntity)
			m_EditorScene->DuplicateEntity(selectedEntity);
	}

	// Route a path to the panel that owns its extension. One dispatcher,
	// shared by the content browser and the pending-open drain: those two
	// had already drifted, which is how a .wlblocks graph ended up shown
	// as raw YAML in the Script Editor.
	void EditorLayer::OpenVisualFile(const std::string &path)
	{
		const std::filesystem::path p(path);
		const std::string ext = p.extension().string();

		if (ext == ".py")
		{
			m_ScriptEditorPanel.OpenFile(p);
			m_ScriptEditorPanel.SetVisible(true);
		}
		else if (ext == ".wlbtree")
		{
			m_NPCBehaviorPanel.OpenTree(p.string());
		}
		else if (ext == ".wlblocks")
		{
			m_VisualBlocksPanel.OpenGraph(p.string());
		}
		else if (ext == ".wlavb")
		{
			m_AvbGraphPanel.OpenGraph(p.string());
		}
		else if (ext == ".wlterrain")
		{
			m_TerrainAssetPanel.SetVisible(true);
			m_TerrainAssetPanel.OpenAsset(p, Project::Get().GetAssetRoot());
		}
		else
		{
			WL_CORE_WARN("No editor for '{0}'", ext);
		}
	}
}