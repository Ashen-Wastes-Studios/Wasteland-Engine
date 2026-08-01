project "Box3D"
    kind "StaticLib"
    language "C"
    cdialect "C11"
    staticruntime "Off"

    -- Adjust target/object output directories as needed
    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

    files
    {
        "include/**.h",
        "src/**.h",
        "src/**.c"
    }

    includedirs
    {
        "include",
        "src"
    }

    filter "system:windows"
        systemversion "latest"
        defines 
        { 
            "_CRT_SECURE_NO_WARNINGS" 
        }

    filter "configurations:Debug"
        runtime "Debug"
        symbols "On"

    filter "configurations:Release"
        runtime "Release"
        optimize "On"

    filter "configurations:Dist"
        runtime "Release"
        optimize "On"