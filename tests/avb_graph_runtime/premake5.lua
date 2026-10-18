workspace "AvbGraphRuntimeTests"
	architecture "x64"
	toolset "v145"
	startproject "AvbGraphRuntimeTests"

configurations { "Debug", "Release" }

outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

project "AvbGraphRuntimeTests"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"

	targetdir ("bin/" .. outputdir)
	objdir ("bin-int/" .. outputdir)

	files
	{
		"test_avb_graph_runtime.cpp",
		"../../Wasteland/src/Wasteland/Avb/Graph/Runtime.cpp",
		"../../Wasteland/src/Wasteland/Avb/Graph/Graph.cpp",
		"../../Wasteland/src/Wasteland/Avb/Graph/Compiler.cpp",
		"../../Wasteland/src/Wasteland/Avb/Graph/Validator.cpp",
		"../../Wasteland/src/Wasteland/Avb/Graph/NodeRegistry.cpp",
		"../../Wasteland/src/Wasteland/Avb/AvbDsp.cpp",
		"../../Wasteland/src/Wasteland/Avb/AudioBus.cpp",
		"../../Wasteland/src/Wasteland/Core/Log.cpp",
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

	links
	{
		"ole32.lib",
	}

	filter "system:windows"
		systemversion "latest"
		buildoptions { "/utf-8", "/bigobj" }

	filter "configurations:Debug"
		defines "WL_DEBUG"
		symbols "On"
		runtime "Debug"

	filter "configurations:Release"
		defines "WL_RELEASE"
		optimize "On"
		runtime "Release"
