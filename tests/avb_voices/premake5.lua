-- Standalone headless native tests for the AVB.3 procedural voices.
-- Run premake from this directory and build AvbVoiceTests.vcxproj directly;
-- this target compiles the real AVB DSP sources and does not edit the aggregate project.
workspace "AvbVoiceTests"
	architecture "x64"
	toolset "v145"
	configurations { "Debug", "Release" }

project "AvbVoiceTests"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"

	location "."
	targetdir "bin/%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"
	objdir "bin-int/%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

	files
	{
		"test_avb_voices.cpp",
		"../../Wasteland/src/Wasteland/Avb/AvbDsp.cpp",
		"../../Wasteland/src/Wasteland/Avb/AvbVoices.cpp",
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
