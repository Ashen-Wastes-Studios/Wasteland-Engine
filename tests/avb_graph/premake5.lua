-- Standalone headless AVB.4 graph model/serializer/compiler tests.
-- The parent Wasteland project enumerates sources explicitly; this target
-- compiles the isolated AVB graph files without editing its project settings.
workspace "AvbGraphTests"
	architecture "x64"
	toolset "v145"
	configurations { "Debug", "Release" }

project "AvbGraphTests"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"
	location "."
	targetdir "bin/%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"
	objdir "bin-int/%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

	files
	{
		"test_avb_graph.cpp",
		"../../Wasteland/src/Wasteland/Avb/Graph/Graph.cpp",
		"../../Wasteland/src/Wasteland/Avb/Graph/GraphSerializer.cpp",
		"../../Wasteland/src/Wasteland/Avb/Graph/NodeRegistry.cpp",
		"../../Wasteland/src/Wasteland/Avb/Graph/Validator.cpp",
		"../../Wasteland/src/Wasteland/Avb/Graph/Compiler.cpp",
	}

	defines { "YAML_CPP_STATIC_DEFINE", "_CRT_SECURE_NO_WARNINGS" }
	includedirs
	{
		"../../Wasteland/src",
		"../../Wasteland/vendor/spdlog/include",
		"../../Wasteland/vendor/yaml-cpp/include",
	}

	filter "system:windows"
		systemversion "latest"
		buildoptions { "/utf-8" }
		libdirs { "../../Wasteland/vendor/yaml-cpp/bin/%{cfg.buildcfg}-windows-x86_64/yaml-cpp" }
		links { "yaml-cpp" }

	filter "configurations:Debug"
		symbols "On"
		runtime "Debug"

	filter "configurations:Release"
		optimize "On"
		runtime "Release"
