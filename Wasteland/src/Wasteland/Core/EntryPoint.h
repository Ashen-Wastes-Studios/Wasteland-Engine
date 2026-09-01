#pragma once

#include <filesystem>

#ifdef WL_PLATFORM_WINDOWS

extern Wasteland::Application *Wasteland::CreateApplication();

namespace
{
	void ResolveAssetWorkingDirectory(const std::filesystem::path &exePath)
	{
		std::filesystem::path exeDir = exePath.parent_path();
		std::vector<std::filesystem::path> candidateRoots =
			{
				exeDir,
				exeDir.parent_path(),
				std::filesystem::current_path(),
				exeDir / "..",
			};

		for (const auto &root : candidateRoots)
		{
			std::filesystem::path assetsPath = root / "assets";
			if (std::filesystem::exists(assetsPath) && std::filesystem::is_directory(assetsPath))
			{
				std::filesystem::current_path(root);
				return;
			}
		}
	}
}

int main(int argc, char **argv)
{
	if (argc > 0)
		ResolveAssetWorkingDirectory(std::filesystem::absolute(argv[0]));

	Wasteland::Log::Init();

	WL_PROFILE_BEGIN_SESSION("Startup", "WastelandProfile-Startup.json");
	auto app = Wasteland::CreateApplication();
	WL_PROFILE_END_SESSION();

	WL_PROFILE_BEGIN_SESSION("Runtime", "WastelandProfile-Runtime.json");
	app->Run();
	WL_PROFILE_END_SESSION();

	WL_PROFILE_BEGIN_SESSION("Shutdown", "WastelandProfile-Shutdown.json");
	delete app;
	WL_PROFILE_END_SESSION();
}

#endif