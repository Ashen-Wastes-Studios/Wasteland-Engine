-- Standalone headless native tests for the AVB.2 DSP implementation.
-- Run premake from this directory and build AvbDspTests.vcxproj directly; this
-- intentionally does not edit or share the aggregate tests/premake5.lua project.
workspace "AvbDspTests"
	architecture "x64"
	toolset "v145"
	configurations { "Debug", "Release" }

project "AvbDspTests"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"

	location "."
	targetdir "bin/%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"
	objdir "bin-int/%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

	files
	{
		"test_avb_dsp.cpp",
		"../../Wasteland/src/Wasteland/Avb/AvbDsp.cpp",
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
