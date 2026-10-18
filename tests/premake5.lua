-- Functional tests for engine subsystems that do not need a GPU or a window.
--
-- ProjectTests links the real Project.cpp, so it exercises the shipping code
-- rather than a copy. It is a ConsoleApp with no engine dependencies beyond
-- logging and yaml-cpp, which is what makes it runnable in CI.

workspace "WastelandTests"
	architecture "x64"
	toolset "v145"
	startproject "ProjectTests"

configurations { "Debug", "Release" }

outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

project "ProjectTests"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"

	targetdir ("bin-int/tests/" .. outputdir)
	objdir ("bin-int/tests/obj/" .. outputdir)

	files
	{
		-- Paths are relative to this script (tests/), not the workspace root.
		"test_project.cpp",
		-- The real implementation, compiled in directly. Depending on the
		-- Wasteland static library would drag in the renderer and every
		-- vendor library for a filesystem-only class.
		"../Wasteland/src/Wasteland/Project/Project.cpp",
		"../Wasteland/src/Wasteland/Core/Log.cpp",
	}

	defines
	{
		-- WL_PLATFORM_WINDOWS is derived from _WIN64 by Wasteland/Core/Core.h;
		-- defining it here too would only produce a redefinition warning.
		"YAML_CPP_STATIC_DEFINE",
		"_CRT_SECURE_NO_WARNINGS",
	}

	includedirs
	{
		-- Relative to this script (tests/), matching the files block above.
		"../Wasteland/src",
		"../Wasteland/vendor/spdlog/include",
		"../Wasteland/vendor/yaml-cpp/include",
	}

	libdirs
	{
		"../Wasteland/vendor/yaml-cpp/bin/%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}/yaml-cpp",
	}

	links
	{
		"yaml-cpp",
		"ole32.lib",
		"shell32.lib",
		"advapi32.lib",
	}

	filter "system:windows"
		systemversion "latest"

	buildoptions
	{
		"/utf-8"
	}

	filter "configurations:Debug"
		defines "WL_DEBUG"
		symbols "On"
		runtime "Debug"

	filter "configurations:Release"
		defines "WL_RELEASE"
		optimize "On"
		runtime "Release"

-- Age module tests. Its own project because it shares no dependency with
-- ProjectTests: AgeBand is pure integer arithmetic, so it needs neither
-- yaml-cpp nor Log.cpp. Linking them would make the compliance-critical
-- threshold logic depend on unrelated subsystems and slow the RED/GREEN
-- loop this suite is built around.
project "AgeTests"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"

	targetdir ("bin-int/tests/" .. outputdir)
	objdir ("bin-int/tests/obj-age/" .. outputdir)

	files
	{
		"test_age_band.cpp",
		"../Wasteland/src/Wasteland/Age/AgeBand.cpp",
	}

	defines
	{
		"_CRT_SECURE_NO_WARNINGS",
	}

	includedirs
	{
		-- AgeBand.cpp follows the engine convention of including wlpch.h
		-- first, which the Wasteland static lib's PCH requires (error C1010
		-- otherwise). wlpch.h pulls in Log.h and therefore spdlog, but
		-- spdlog is header-only in this project (no SPDLOG_COMPILED_LIB), so
		-- that costs an include path and NOT a link dependency -- Log.cpp stays
		-- out of this project, and the compliance-critical threshold logic
		-- keeps no runtime dependency on the logging subsystem.
		"../Wasteland/src",
		"../Wasteland/vendor/spdlog/include",
	}

	links
	{
		"ole32.lib",
	}

	filter "system:windows"
		systemversion "latest"

	buildoptions
	{
		"/utf-8"
	}

	filter "configurations:Debug"
		defines "WL_DEBUG"
		symbols "On"
		runtime "Debug"

	filter "configurations:Release"
		defines "WL_RELEASE"
		optimize "On"
		runtime "Release"

	-- Jurisdiction policy tests. A separate executable rather than a second file
	-- in AgeTests because it has its own main(), and a separate executable rather
	-- than one combined policy binary because the policy layer must be provable
	-- without AgeBand: the assurance ladder and the jurisdiction rules are
	-- independent, and a shared binary would let a regression in one mask a
	-- regression in the other.
	project "PolicyTests"
		kind "ConsoleApp"
		language "C++"
		cppdialect "C++17"

		targetdir ("bin-int/tests/" .. outputdir)
		objdir ("bin-int/tests/obj-policy/" .. outputdir)

		files
		{
			"test_policy.cpp",
			"../Wasteland/src/Wasteland/Age/JurisdictionPolicy.cpp",
		}

		defines
		{
			"_CRT_SECURE_NO_WARNINGS",
		}

		includedirs
		{
			-- JurisdictionPolicy.cpp includes wlpch.h per engine convention, which
			-- pulls in Log.h and therefore spdlog. spdlog is header-only here (no
			-- SPDLOG_COMPILED_LIB), so this is an include path and NOT a link
			-- dependency -- Log.cpp stays out, and the compliance-critical
			-- jurisdiction logic keeps no runtime dependency on logging.
			"../Wasteland/src",
			"../Wasteland/vendor/spdlog/include",
		}

		links
		{
			"ole32.lib",
		}

		filter "system:windows"
			systemversion "latest"

		buildoptions
		{
			"/utf-8"
		}

		filter "configurations:Debug"
			defines "WL_DEBUG"
			symbols "On"
			runtime "Debug"

		filter "configurations:Release"
			defines "WL_RELEASE"
			optimize "On"
			runtime "Release"

	-- SafetyProfile tests. Separate executable again (own main), and separate
-- from PolicyTests because the two layers have different dependencies: the
-- policy layer reasons about jurisdiction and assurance strength, the profile
-- layer reasons about age bands alone. Keeping them apart means a change to
-- one cannot mask a regression in the other, and SafetyProfile can be proved
-- to fail closed without any jurisdiction input at all.
project "SafetyProfileTests"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"

	targetdir ("bin-int/tests/" .. outputdir)
	objdir ("bin-int/tests/obj-safety/" .. outputdir)

	files
	{
		"test_safety_profile.cpp",
		"../Wasteland/src/Wasteland/Age/AgeBand.cpp",
		"../Wasteland/src/Wasteland/Age/SafetyProfile.cpp",
	}

	defines
	{
		"_CRT_SECURE_NO_WARNINGS",
	}

	includedirs
	{
		-- Same rationale as AgeTests: SafetyProfile.cpp includes wlpch.h per
		-- engine convention, and wlpch.h pulls in Log.h/spdlog. spdlog is
		-- header-only here, so that is an include path and not a link
		-- dependency.
		"../Wasteland/src",
		"../Wasteland/vendor/spdlog/include",
	}

	links
	{
		"ole32.lib",
	}

	filter "system:windows"
		systemversion "latest"

	buildoptions
	{
		"/utf-8"
	}

	filter "configurations:Debug"
		defines "WL_DEBUG"
		symbols "On"
		runtime "Debug"

	filter "configurations:Release"
		defines "WL_RELEASE"
		optimize "On"
		runtime "Release"

-- Attestation and provider-interface tests. Separate executable (own main)
-- and separate from the earlier two because this is the only suite that
-- exercises a polymorphic interface: a studio subclasses
-- IAgeAssuranceProvider and the engine consumes it, so the tests have to do
-- the same or they are not testing the seam.
project "AttestationTests"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"

	targetdir ("bin-int/tests/" .. outputdir)
	objdir ("bin-int/tests/obj-attest/" .. outputdir)

	files
	{
		"test_attestation.cpp",
		"../Wasteland/src/Wasteland/Age/AgeBand.cpp",
		"../Wasteland/src/Wasteland/Age/Attestation.cpp",
	}

	defines
	{
		"_CRT_SECURE_NO_WARNINGS",
	}

	includedirs
	{
		"../Wasteland/src",
		"../Wasteland/vendor/spdlog/include",
	}

	links
	{
		"ole32.lib",
	}

	filter "system:windows"
		systemversion "latest"

	buildoptions
	{
		"/utf-8"
	}

	filter "configurations:Debug"
		defines "WL_DEBUG"
		symbols "On"
		runtime "Debug"

	filter "configurations:Release"
		defines "WL_RELEASE"
		optimize "On"
		runtime "Release"

-- Apple Declared Age Range mapping tests. Separate executable (own main)
-- because the platform wrapper is inert off iOS: the engine's Core.h rejects
-- an iOS build with #error, so the provider can only return Absent() here.
-- The mapping -- the part that decides what Apple's numbers mean -- is pure
-- and lives in the same translation unit, so it is fully testable on desktop.
project "AppleProviderTests"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"

	targetdir ("bin-int/tests/" .. outputdir)
	objdir ("bin-int/tests/obj-apple/" .. outputdir)

	files
	{
		"test_apple_provider.cpp",
		"../Wasteland/src/Wasteland/Age/AgeBand.cpp",
		"../Wasteland/src/Wasteland/Age/Attestation.cpp",
		"../Wasteland/src/Wasteland/Age/JurisdictionPolicy.cpp",
		"../Wasteland/src/Wasteland/Age/AppleAgeSignalProvider.cpp",
	}

	defines
	{
		"_CRT_SECURE_NO_WARNINGS",
	}

	includedirs
	{
		"../Wasteland/src",
		"../Wasteland/vendor/spdlog/include",
	}

	links
	{
		"ole32.lib",
	}

	filter "system:windows"
		systemversion "latest"

	buildoptions
	{
		"/utf-8"
	}

	filter "configurations:Debug"
		defines "WL_DEBUG"
		symbols "On"
		runtime "Debug"

	filter "configurations:Release"
		defines "WL_RELEASE"
		optimize "On"
		runtime "Release"

-- Google Play Age Signals mapping tests. Separate executable (own main),
-- matching the Apple one. The platform wrapper is inert off Android for the
-- same reason -- Core.h rejects an Android build with #error -- so again only
-- the pure mapping is exercised here.
project "GoogleProviderTests"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"

	targetdir ("bin-int/tests/" .. outputdir)
	objdir ("bin-int/tests/obj-google/" .. outputdir)

	files
	{
		"test_google_provider.cpp",
		"../Wasteland/src/Wasteland/Age/AgeBand.cpp",
		"../Wasteland/src/Wasteland/Age/Attestation.cpp",
		"../Wasteland/src/Wasteland/Age/JurisdictionPolicy.cpp",
		"../Wasteland/src/Wasteland/Age/GooglePlayAgeSignalProvider.cpp",
	}

	defines
	{
		"_CRT_SECURE_NO_WARNINGS",
	}

	includedirs
	{
		"../Wasteland/src",
		"../Wasteland/vendor/spdlog/include",
	}

	links
	{
		"ole32.lib",
	}

	filter "system:windows"
		systemversion "latest"

	buildoptions
	{
		"/utf-8"
	}

	filter "configurations:Debug"
		defines "WL_DEBUG"
		symbols "On"
		runtime "Debug"

	filter "configurations:Release"
		defines "WL_RELEASE"
		optimize "On"
		runtime "Release"

-- Guardian state-machine tests. Separate executable (own main). Pure value
-- semantics with no ImGui, no window and no GPU: the 5b GuardianToolsLayer
-- draws this state, and it must be provable without a display. Anything that
-- ends up inside the layer's OnImGuiRender should live here instead.
project "GuardianPolicyTests"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"

	targetdir ("bin-int/tests/" .. outputdir)
	objdir ("bin-int/tests/obj-guardian/" .. outputdir)

	files
	{
		"test_guardian_policy.cpp",
		"../Wasteland/src/Wasteland/Age/AgeBand.cpp",
		"../Wasteland/src/Wasteland/Age/Attestation.cpp",
		"../Wasteland/src/Wasteland/Age/JurisdictionPolicy.cpp",
		"../Wasteland/src/Wasteland/Age/GuardianPolicy.cpp",
	}

	defines
	{
		"_CRT_SECURE_NO_WARNINGS",
	}

	includedirs
	{
		"../Wasteland/src",
		"../Wasteland/vendor/spdlog/include",
	}

	links
	{
		"ole32.lib",
	}

	filter "system:windows"
		systemversion "latest"

	buildoptions
	{
		"/utf-8"
	}

	filter "configurations:Debug"
		defines "WL_DEBUG"
		symbols "On"
		runtime "Debug"

	filter "configurations:Release"
		defines "WL_RELEASE"
		optimize "On"
		runtime "Release"

-- AgeService tests. Separate executable (own main). AgeService is the only
-- place the layers meet, so it needs a provider stub and the full Age stack
-- compiled in; it deliberately does NOT use the engine-wide Instance() -- each
-- test constructs its own service so the cases stay order-independent.
project "AgeServiceTests"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"

	targetdir ("bin-int/tests/" .. outputdir)
	objdir ("bin-int/tests/obj-service/" .. outputdir)

	files
	{
		"test_age_service.cpp",
		"../Wasteland/src/Wasteland/Age/AgeBand.cpp",
		"../Wasteland/src/Wasteland/Age/Attestation.cpp",
		"../Wasteland/src/Wasteland/Age/JurisdictionPolicy.cpp",
		"../Wasteland/src/Wasteland/Age/SafetyProfile.cpp",
		"../Wasteland/src/Wasteland/Age/GuardianPolicy.cpp",
		"../Wasteland/src/Wasteland/Age/AgeService.cpp",
	}

	defines
	{
		"_CRT_SECURE_NO_WARNINGS",
	}

	includedirs
	{
		"../Wasteland/src",
		"../Wasteland/vendor/spdlog/include",
	}

	links
	{
		"ole32.lib",
	}

	filter "system:windows"
		systemversion "latest"

	buildoptions
	{
		"/utf-8"
	}

	filter "configurations:Debug"
		defines "WL_DEBUG"
		symbols "On"
		runtime "Debug"

	filter "configurations:Release"
		defines "WL_RELEASE"
		optimize "On"
		runtime "Release"

-- The guardian view model: pure decision logic, no ImGui. Separate
	-- executable (own main) because it needs AgeBand + GuardianPolicy
	-- compiled in, and deliberately links NO ImGui -- that separation is the
	-- whole point of splitting the view model out of GuardianToolsLayer.
	project "GuardianViewTests"
		kind "ConsoleApp"
		language "C++"
		cppdialect "C++17"

		targetdir ("bin-int/tests/" .. outputdir)
		objdir ("bin-int/tests/obj-view/" .. outputdir)

		files
		{
			"test_guardian_view.cpp",
			"../Wasteland/src/Wasteland/Age/AgeBand.cpp",
			"../Wasteland/src/Wasteland/Age/GuardianPolicy.cpp",
			"../Wasteland/src/Wasteland/Age/GuardianToolsView.cpp",
		}

		defines
		{
			"_CRT_SECURE_NO_WARNINGS",
		}

		includedirs
		{
			"../Wasteland/src",
			"../Wasteland/vendor/spdlog/include",
		}

		links
		{
			"ole32.lib",
		}

		filter "system:windows"
			systemversion "latest"

		buildoptions
		{
			"/utf-8"
		}

		filter "configurations:Debug"
			defines "WL_DEBUG"
			symbols "On"
			runtime "Debug"

		filter "configurations:Release"
			defines "WL_RELEASE"
			optimize "On"
			runtime "Release"

-- The audit log and the feature-authorisation gate. Separate
	-- executable (own main); needs AgeService and the policy stack, and links
	-- no ImGui and no graphics.
	project "AuditLogTests"
		kind "ConsoleApp"
		language "C++"
		cppdialect "C++17"

		targetdir ("bin-int/tests/" .. outputdir)
		objdir ("bin-int/tests/obj-audit/" .. outputdir)

		files
		{
			"test_age_audit_log.cpp",
			"../Wasteland/src/Wasteland/Age/AgeBand.cpp",
			"../Wasteland/src/Wasteland/Age/Attestation.cpp",
			"../Wasteland/src/Wasteland/Age/JurisdictionPolicy.cpp",
			"../Wasteland/src/Wasteland/Age/SafetyProfile.cpp",
			"../Wasteland/src/Wasteland/Age/GuardianPolicy.cpp",
			"../Wasteland/src/Wasteland/Age/AgeService.cpp",
			"../Wasteland/src/Wasteland/Age/AgeAuditLog.cpp",
		}

		defines
		{
			"_CRT_SECURE_NO_WARNINGS",
		}

		includedirs
		{
			"../Wasteland/src",
			"../Wasteland/vendor/spdlog/include",
		}

		links
		{
			"ole32.lib",
		}

		filter "system:windows"
			systemversion "latest"

		buildoptions
		{
			"/utf-8"
		}

		filter "configurations:Debug"
			defines "WL_DEBUG"
			symbols "On"
			runtime "Debug"

		filter "configurations:Release"
			defines "WL_RELEASE"
			optimize "On"
			runtime "Release"

-- A second executable rather than another file in ProjectTests: it has its
-- own main(). Creates a real project against the installed engine assets
-- and checks the renderers can find every shader they load by name.
project "SeedingSmokeTest"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"

	targetdir ("bin-int/tests/" .. outputdir)
	objdir ("bin-int/tests/obj-seed/" .. outputdir)

	files
	{
		"test_project_seeding.cpp",
		"../Wasteland/src/Wasteland/Project/Project.cpp",
		"../Wasteland/src/Wasteland/Core/Log.cpp",
	}

	defines
	{
		"YAML_CPP_STATIC_DEFINE",
		"_CRT_SECURE_NO_WARNINGS",
	}

	includedirs
	{
		"../Wasteland/src",
		"../Wasteland/vendor/spdlog/include",
		"../Wasteland/vendor/yaml-cpp/include",
	}

	libdirs
	{
		"../Wasteland/vendor/yaml-cpp/bin/%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}/yaml-cpp",
	}

	links
	{
		"yaml-cpp",
		"ole32.lib",
		"shell32.lib",
		"advapi32.lib",
	}

	filter "system:windows"
		systemversion "latest"

	buildoptions
	{
		"/utf-8"
	}

	filter "configurations:Debug"
		defines "WL_DEBUG"
		symbols "On"
		runtime "Debug"

	filter "configurations:Release"
		defines "WL_RELEASE"
		optimize "On"
		runtime "Release"
