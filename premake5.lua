workspace "Wasteland"
	architecture "x64"
	startproject "DemonCore-Editor"

	configurations
	{
		"Debug",
		"Release",
		"Dist"
	}

outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

-- Include directories releative to root folder (solution directory)
IncludeDir = {}
IncludeDir["GLFW"] = "Wasteland/vendor/GLFW/include"
IncludeDir["Glad"] = "Wasteland/vendor/Glad/include"
IncludeDir["ImGui"] = "Wasteland/vendor/imgui"
IncludeDir["glm"] = "Wasteland/vendor/glm"
IncludeDir["stb_image"] = "Wasteland/vendor/stb_image"
IncludeDir["entt"] = "Wasteland/vendor/entt/single_include"
IncludeDir["yaml_cpp"] = "Wasteland/vendor/yaml-cpp/include"
IncludeDir["ImGuizmo"] = "Wasteland/vendor/ImGuizmo"
IncludeDir["Box2D"] = "Wasteland/vendor/Box2D/include"
IncludeDir["pybind11"] = "Wasteland/vendor/pybind11/include"

pythonpath = "C:\\Users\\rtoue\\AppData\\Local\\Python\\pythoncore-3.14-64"

group "Dependencies"
	include "Wasteland/vendor/GLFW"
	include "Wasteland/vendor/Glad"
	include "Wasteland/vendor/imgui"
	include "Wasteland/vendor/yaml-cpp"
	include "Wasteland/vendor/Box2D"
group ""

project "Wasteland"
	location "Wasteland"
	kind "StaticLib"
	language "C++"
	cppdialect "C++17"

	debugdir ("%{wks.location}/DemonCore-Editor")

	targetdir ("bin/" .. outputdir .. "/%{prj.name}")
	objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

	pchheader "wlpch.h"
	pchsource "Wasteland/src/wlpch.cpp"

	files
	{
		"%{prj.name}/src/**.h",
		"%{prj.name}/src/**.cpp",
		"%{prj.name}/vendor/stb_image/**.h",
		"%{prj.name}/vendor/stb_image/**.cpp",
		"%{prj.name}/vendor/glm/glm/**.hpp",
		"%{prj.name}/vendor/glm/glm/**.inl",

		"%{prj.name}/vendor/ImGuizmo/ImGuizmo.h",
		"%{prj.name}/vendor/ImGuizmo/ImGuizmo.cpp"
	}

	defines
	{
		"_CRT_SECURE_NO_WARNINGS",
		"YAML_CPP_STATIC_DEFINE"
	}

	includedirs
	{
		"%{prj.name}/src",
		"%{prj.name}/vendor/spdlog/include",
		"%{IncludeDir.GLFW}",
		"%{IncludeDir.Glad}",
		"%{IncludeDir.ImGui}",
		"%{IncludeDir.glm}",
		"%{IncludeDir.stb_image}",
		"%{IncludeDir.entt}",
		"%{IncludeDir.yaml_cpp}",
		"%{IncludeDir.ImGuizmo}",
		"%{IncludeDir.Box2D}",
		"%{IncludeDir.pybind11}",
		pythonpath .. "/include"
	}

	libdirs
	{
		pythonpath .. "/libs"
	}

	links 
	{
		"GLFW",
		"Glad",
		"ImGui",
		"yaml-cpp",
		"Box2D",
		"python314"
	}

	filter "files:vendor/ImGuizmo/**.cpp"
		flags 
		{
			"NoPCH"
		}

	filter "system:windows"
		systemversion "latest"

		buildoptions
		{
			"/utf-8"
		}

		defines
		{
			"WL_PLATFORM_WINDOWS",
			"WL_BUILD_DLL",
			"GLFW_INCLUDE_NONE"
		}

		links
		{
			"opengl32.lib"
		}

	filter "system:linux"
		buildoptions
		{
			"-fPIC"
		}

		defines
		{
			"WL_PLATFORM_LINUX",
			"WL_BUILD_DLL",
			"GLFW_INCLUDE_NONE"
		}

		links
		{
			"GL",
			"pthread"
		}

	filter "system:macosx"
		buildoptions
		{
			"-fPIC"
		}

		defines
		{
			"WL_PLATFORM_MACOS",
			"WL_BUILD_DLL",
			"GLFW_INCLUDE_NONE"
		}

		links
		{
			"Cocoa.framework",
			"IOKit.framework",
			"CoreFoundation.framework"
		}

	filter "configurations:Debug"
		defines "WL_DEBUG"
		symbols "On"
		runtime "Debug"

	filter "configurations:Release"
		defines "WL_RELEASE"
		optimize "On"
		runtime "Release"

	filter "configurations:Dist"
		defines "WL_DIST"
		optimize "On"
		runtime "Release"

project "WastelandPython"
    kind "SharedLib"
    language "C++"
    cppdialect "C++17"
    
    targetextension ".pyd"
    targetname "Wasteland"
    
    targetdir ("bin/" .. outputdir .. "/%{wks.startproject}")
    
    files 
    { 
        "Wasteland/src/Wasteland/Scripting/Bindings.cpp"
    }

    includedirs
    {
        "Wasteland/src",
		"Wasteland/vendor/spdlog/include",
		"%{IncludeDir.entt}",
		"%{IncludeDir.glm}",
        "%{IncludeDir.pybind11}",
        pythonpath .. "/include"
    }

    libdirs { pythonpath .. "/libs" }

    links 
    { 
        "Wasteland",
        "python314" 
    }

	filter "system:windows"
		systemversion "latest"

		buildoptions
		{
			"/utf-8"
		}

		defines
		{
			"WL_PLATFORM_WINDOWS",
			"WL_BUILD_DLL",
			"GLFW_INCLUDE_NONE"
		}

		links
		{
			"opengl32.lib"
		}

	filter "system:linux"
		buildoptions
		{
			"-fPIC"
		}

		defines
		{
			"WL_PLATFORM_LINUX",
			"WL_BUILD_DLL",
			"GLFW_INCLUDE_NONE"
		}

		links
		{
			"GL",
			"pthread"
		}

	filter "system:macosx"
		buildoptions
		{
			"-fPIC"
		}

		defines
		{
			"WL_PLATFORM_MACOS",
			"WL_BUILD_DLL",
			"GLFW_INCLUDE_NONE"
		}

		links
		{
			"Cocoa.framework",
			"IOKit.framework",
			"CoreFoundation.framework"
		}

	filter "configurations:Debug"
		defines "WL_DEBUG"
		symbols "On"
		runtime "Debug"

	filter "configurations:Release"
		defines "WL_RELEASE"
		optimize "On"
		runtime "Release"

	filter "configurations:Dist"
		defines "WL_DIST"
		optimize "On"
		runtime "Release"

project "Sandbox"
	location "Sandbox"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"

	debugdir ("%{wks.location}/Sandbox")

	targetdir ("bin/" .. outputdir .. "/%{prj.name}")
	objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

	files
	{
		"%{prj.name}/src/**.h",
		"%{prj.name}/src/**.cpp"
	}

	includedirs
	{
		"Wasteland/vendor/spdlog/include",
		"Wasteland/vendor",
		"Wasteland/src",
		"%{IncludeDir.glm}",
		"%{IncludeDir.Glad}",
		"%{IncludeDir.entt}",
		"%{IncludeDir.pybind11}",
		pythonpath .. "/include"
	}

	libdirs
	{
		pythonpath .. "/libs"
	}

	links
	{
		"Wasteland",
		"ImGui",
		"python314"
	}

	filter "system:windows"
		systemversion "latest"

		buildoptions
		{
			"/utf-8"
		}

		defines
		{
			"WL_PLATFORM_WINDOWS"
		}

	filter "system:linux"
		buildoptions
		{
			"-fPIC"
		}

		defines
		{
			"WL_PLATFORM_LINUX"
		}

	filter "system:macosx"
		buildoptions
		{
			"-fPIC"
		}

		defines
		{
			"WL_PLATFORM_MACOS"
		}

	filter "configurations:Debug"
		defines "WL_DEBUG"
		symbols "On"
		runtime "Debug"

	filter "configurations:Release"
		defines "WL_RELEASE"
		optimize "On"
		runtime "Release"

	filter "configurations:Dist"
		defines "WL_DIST"
		optimize "On"
		runtime "Release"

project "DemonCore-Editor"
	location "DemonCore-Editor"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"

	debugdir ("%{wks.location}/DemonCore-Editor")

	targetdir ("bin/" .. outputdir .. "/%{prj.name}")
	objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

	files
	{
		"%{prj.name}/src/**.h",
		"%{prj.name}/src/**.cpp"
	}

	defines
	{
		"YAML_CPP_STATIC_DEFINE"
	}

	includedirs
	{
		"Wasteland/vendor/spdlog/include",
		"Wasteland/vendor",
		"Wasteland/src",
		"%{IncludeDir.glm}",
		"%{IncludeDir.Glad}",
		"%{IncludeDir.entt}",
		"%{IncludeDir.yaml_cpp}",
		"%{IncludeDir.ImGuizmo}",
		"%{IncludeDir.pybind11}",
		pythonpath .. "/include"
	}

	libdirs
	{
		pythonpath .. "/libs"
	}

	links
	{
		"Wasteland",
		"ImGui",
		"yaml-cpp",
		"python314"
	}

	postbuildcommands {
        'copy /Y "C:\\Users\\rtoue\\AppData\\Local\\Python\\pythoncore-3.14-64\\python314.dll" "$(TargetDir)python314.dll"',
		'copy /Y "C:\\Users\\rtoue\\AppData\\Local\\Python\\pythoncore-3.14-64\\python314._pth" "$(TargetDir)"',
        'xcopy /Y /E /I "C:\\Users\\rtoue\\AppData\\Local\\Python\\pythoncore-3.14-64\\Lib" "$(TargetDir)Lib"',
        'xcopy /Y /E /I "C:\\Users\\rtoue\\AppData\\Local\\Python\\pythoncore-3.14-64\\DLLs" "$(TargetDir)DLLs"'
    }

	filter "system:windows"
		systemversion "latest"

		buildoptions
		{
			"/utf-8"
		}

		defines
		{
			"WL_PLATFORM_WINDOWS"
		}

	filter "system:linux"
		buildoptions
		{
			"-fPIC"
		}

		defines
		{
			"WL_PLATFORM_LINUX"
		}

	filter "system:macosx"
		buildoptions
		{
			"-fPIC"
		}

		defines
		{
			"WL_PLATFORM_MACOS"
		}

	filter "configurations:Debug"
		defines "WL_DEBUG"
		symbols "On"
		runtime "Debug"

	filter "configurations:Release"
		defines "WL_RELEASE"
		optimize "On"
		runtime "Release"

	filter "configurations:Dist"
		defines "WL_DIST"
		optimize "On"
		runtime "Release"