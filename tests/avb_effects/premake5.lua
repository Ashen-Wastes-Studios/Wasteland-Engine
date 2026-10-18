-- Standalone AVB.6 bus-effect tests; build independently from tests/premake5.lua.
workspace "AvbEffectTests"
	architecture "x64"
	toolset "v145"
	configurations { "Debug", "Release" }

project "AvbEffectTests"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"
	location "."
	targetdir "bin/%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"
	objdir "bin-int/%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

	files
	{
		"test_avb_effects.cpp",
		"../../Wasteland/src/Wasteland/Avb/AudioBus.cpp",
		"../../Wasteland/src/Wasteland/Avb/AvbDsp.cpp",
		"../../Wasteland/src/Wasteland/Avb/AvbBusEffects.cpp",
		"../../Wasteland/src/Wasteland/Audio/AudioClip.cpp",
		"../../Wasteland/src/Wasteland/Core/UUID.cpp",
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
