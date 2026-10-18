-- Standalone native AVB.11 recording and stem-export tests.
-- Capture is fed through the public deterministic block-ingest seam here, so
-- this test project never requires a physical audio input device.
workspace "AvbRecordingTests"
	architecture "x64"
	toolset "v145"
	configurations { "Debug", "Release" }

project "AvbRecordingTests"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"

	location "."
	targetdir "bin/%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"
	objdir "bin-int/%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

	files
	{
		"test_avb_recording.cpp",
		"../../Wasteland/src/Wasteland/Avb/Recorder.cpp",
		"../../Wasteland/src/Wasteland/Avb/AudioBus.cpp",
		"../../Wasteland/src/Wasteland/Audio/AudioClip.cpp",
		"../../Wasteland/src/Wasteland/Core/UUID.cpp",
	}

	defines
	{
		"_CRT_SECURE_NO_WARNINGS",
		"AVB_RECORDER_NO_MINIAUDIO_CAPTURE",
		"AVB_RECORDER_TEST_HOOKS",
	}

	includedirs
	{
		"../../Wasteland/src",
		"../../Wasteland/vendor/spdlog/include",
		"../../Wasteland/vendor/miniaudio",
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
