-- Standalone AVB.5 bus tests. Generate and build this project directly only
-- when the engine's active MSBuild jobs have finished.
workspace "AvbBusTests"
	architecture "x64"
	toolset "v145"
	configurations { "Debug", "Release" }

project "AvbBusTests"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"

	location "."
	targetdir "bin/%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"
	objdir "bin-int/%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

	files
	{
		"test_avb_buses.cpp",
		"../../Wasteland/src/Wasteland/Avb/AudioBus.cpp",
	}

	defines
	{
		"_CRT_SECURE_NO_WARNINGS",
	}

	includedirs
	{
		"../../Wasteland/src",
		"../../Wasteland/vendor/spdlog/include",
	}

	filter "system:windows"
		systemversion "latest"
		buildoptions { "/utf-8" }
		links { "ole32.lib" }

	filter "configurations:Debug"
		defines { "WL_DEBUG" }
		symbols "On"
		runtime "Debug"

	filter "configurations:Release"
		defines { "WL_RELEASE" }
		optimize "On"
		runtime "Release"
