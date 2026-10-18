-- Headless integration against the already-built Wasteland static library.
-- This target intentionally does not compile SceneSerializer or Terrain sources
-- into the test executable: calls must traverse the production library.
workspace "SceneTerrainPersistenceTests"
	architecture "x64"
	toolset "v145"
	configurations { "Debug", "Release" }
	startproject "SceneTerrainPersistenceTests"

outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

project "SceneTerrainPersistenceTests"
	uuid "A0B91D6E-8E07-4D4A-9768-6B3B3B367A72"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"
	location "."
	targetdir ("bin/" .. outputdir)
	objdir ("bin-int/" .. outputdir)

	files { "test_scene_terrain_persistence.cpp" }

	defines
	{
		"YAML_CPP_STATIC_DEFINE",
		"_CRT_SECURE_NO_WARNINGS",
	}

	includedirs
	{
		"../../Wasteland/src",
		"../../Wasteland/vendor/spdlog/include",
		"../../Wasteland/vendor/GLFW/include",
		"../../Wasteland/vendor/Glad/include",
		"../../Wasteland/vendor/imgui",
		"../../Wasteland/vendor/glm",
		"../../Wasteland/vendor/miniaudio",
		"../../Wasteland/vendor/stb_image",
		"../../Wasteland/vendor/entt/single_include",
		"../../Wasteland/vendor/yaml-cpp/include",
		"../../Wasteland/vendor/ImGuizmo",
		"../../Wasteland/vendor/Box2D/include",
		"../../Wasteland/vendor/Box3D/include",
		"../../Wasteland/vendor/pybind11/include",
		"../../Wasteland/vendor/nvrhi/include",
		"../../Wasteland/vendor/nvrhi/build/_deps/directx_headers-src/include",
		"../../Wasteland/vendor/nvrhi/build/_deps/vulkan_headers-src/include",
		"C:/VulkanSDK/1.3.296.0/Include",
	}

	libdirs
	{
		"../../bin/" .. outputdir .. "/Wasteland",
		"../../Wasteland/vendor/GLFW/bin/" .. outputdir .. "/GLFW",
		"../../Wasteland/vendor/Glad/bin/" .. outputdir .. "/Glad",
		"../../Wasteland/vendor/imgui/bin/" .. outputdir .. "/ImGui",
		"../../Wasteland/vendor/yaml-cpp/bin/" .. outputdir .. "/yaml-cpp",
		"../../Wasteland/vendor/Box2D/bin/" .. outputdir .. "/Box2D",
		"../../Wasteland/vendor/Box3D/bin/" .. outputdir .. "/Box3D",
		"../../Wasteland/vendor/nvrhi/build/%{cfg.buildcfg}",
		"C:/Users/rtoue/AppData/Local/Python/pythoncore-3.14-64/libs",
		"C:/VulkanSDK/1.3.296.0/Lib",
	}

	links
	{
		"Wasteland",
		"GLFW",
		"Glad",
		"ImGui",
		"yaml-cpp",
		"Box2D",
		"Box3D",
		"python314",
		"nvrhi",
		"nvrhi_d3d11",
		"nvrhi_d3d12",
		"nvrhi_vk",
		"opengl32.lib",
		"d3d11.lib",
		"d3d12.lib",
		"dxgi.lib",
		"d3dcompiler.lib",
		"dxguid.lib",
		"vulkan-1.lib",
		"ole32.lib",
		"winmm.lib",
		"avrt.lib",
		"shell32.lib",
		"advapi32.lib",
	}

	filter "system:windows"
		systemversion "latest"
		buildoptions { "/utf-8", "/bigobj" }

	filter "configurations:Debug"
		defines { "WL_DEBUG" }
		symbols "On"
		runtime "Debug"

	filter "configurations:Release"
		defines { "WL_RELEASE" }
		optimize "On"
		runtime "Release"
