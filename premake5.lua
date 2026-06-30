-- premake5.lua — generates VS solution for core.dll (injected) and injector.exe.
-- Targets x64 to match the Garry's Mod x86-64 branch.
--
--   premake5 vs2022      (generates build/gmod-mcp.sln)
--   then build the Release|x64 configuration.

workspace "gmod-mcp"
    configurations { "Release", "Debug" }
    platforms { "x64" }
    location "build"
    architecture "x86_64"
    systemversion "latest"
    cppdialect "C++17"
    staticruntime "On"
    flags { "MultiProcessorCompile" }
    defines { "WIN32_LEAN_AND_MEAN", "NOMINMAX", "_CRT_SECURE_NO_WARNINGS" }

    filter "configurations:Release"
        optimize "Speed"
        defines { "NDEBUG" }
    filter "configurations:Debug"
        symbols "On"
        defines { "_DEBUG" }
    filter {}

-- The injected DLL.
project "core"
    kind "SharedLib"
    language "C++"
    targetname "core"
    targetdir "%{wks.location}/bin/%{cfg.buildcfg}"
    objdir "%{wks.location}/obj/%{prj.name}/%{cfg.buildcfg}"
    characterset "MBCS"  -- the Source SDK is an ANSI codebase (tchar == char); we
                         -- use explicit *A Win32 APIs, so MBCS is safe throughout.

    files {
        "core/src/**.cpp",
        "core/src/**.h",
        "core/third_party/minhook/src/**.c",
        "core/third_party/minhook/include/**.h",
    }

    includedirs {
        "core/third_party/garrysmod_common/include", -- GarrysMod/Lua/*.h
        "core/third_party/minhook/include",
        "core/third_party",                          -- json.hpp, stb_image_write.h
        -- Source SDK headers (only engine_console.cpp includes them; the dirs are
        -- harmless to other files, but the SDK *defines* are scoped per-file below).
        "core/third_party/garrysmod_common/sourcesdk-minimal/public",
        "core/third_party/garrysmod_common/sourcesdk-minimal/public/tier0",
        "core/third_party/garrysmod_common/sourcesdk-minimal/public/tier1",
        "core/third_party/garrysmod_common/sourcesdk-minimal/public/mathlib",
        "core/third_party/garrysmod_common/sourcesdk-minimal/common",
    }

    -- Ship bootstrap.lua alongside core.dll (the bridge loads it at runtime).
    postbuildcommands {
        '{COPYFILE} "%{wks.location}/../core/lua/bootstrap.lua" "%{cfg.targetdir}/bootstrap.lua"',
    }

    -- engine_console.cpp is the only file that includes the Source SDK headers
    -- (for IVEngineClient). Scope the SDK *defines* to just that file so its
    -- macros don't leak into the rest of the DLL / json.hpp.
    filter "files:**engine_console.cpp"
        defines { "COMPILER_MSVC", "COMPILER_MSVC64", "PLATFORM_64BITS",
                  "_ALLOW_KEYWORD_MACROS", "GMOD_ALLOW_DEPRECATED", "NO_MALLOC_OVERRIDE" }
    filter {}

-- The launcher/injector the MCP server shells out to.
project "injector"
    kind "ConsoleApp"
    language "C++"
    targetname "injector"
    targetdir "%{wks.location}/bin/%{cfg.buildcfg}"
    objdir "%{wks.location}/obj/%{prj.name}/%{cfg.buildcfg}"
    characterset "MBCS"  -- generic Toolhelp structs/funcs map to ANSI (szExeFile is char[])

    files { "injector/src/**.cpp" }
