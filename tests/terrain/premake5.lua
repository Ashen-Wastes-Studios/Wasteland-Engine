-- Standalone headless native tests for the CPU terrain core and sidecar store.
-- Run Premake from this directory and build TerrainTests.vcxproj directly.
workspace "TerrainTests"
	architecture "x64"
	toolset "v145"
	configurations { "Debug", "Release" }

project "TerrainTests"
	uuid "7A4D2A01-ED6C-45E6-91F0-9CEB121D8B7A"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"
	location "."
	flags { "NoPCH" }
	targetdir "bin/%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"
	objdir "bin-int/%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

	files
	{
		"test_terrain.cpp",
		"../../Wasteland/src/Wasteland/Terrain/Terrain.cpp",
		"../../Wasteland/src/Wasteland/Terrain/TerrainCollision.cpp",
		"../../Wasteland/src/Wasteland/Terrain/TerrainRuntime.cpp",
		"../../Wasteland/src/Wasteland/Terrain/TerrainScenePersistence.cpp",
	}

	defines
	{
		"_CRT_SECURE_NO_WARNINGS",
		"YAML_CPP_STATIC_DEFINE",
	}

	includedirs
	{
		"../../Wasteland/src",
		"../../Wasteland/vendor/spdlog/include",
		"../../Wasteland/vendor/Box3D/include",
		"../../Wasteland/vendor/yaml-cpp/include",
	}

	filter "system:windows"
		systemversion "latest"
		buildoptions { "/utf-8" }
		libdirs
		{
			"../../Wasteland/vendor/Box3D/bin/%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}/Box3D",
			"../../Wasteland/vendor/yaml-cpp/bin/%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}/yaml-cpp",
		}
		links { "Box3D", "yaml-cpp", "ole32.lib" }

	filter "configurations:Debug"
		defines { "WL_DEBUG" }
		symbols "On"
		runtime "Debug"

	filter "configurations:Release"
		defines { "WL_RELEASE" }
		optimize "On"
		runtime "Release"
