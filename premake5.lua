-- cod4mirror — proxy d3d9.dll for vanilla CoD4 1.7 MP (iw3mp.exe).
-- Build with: premake5 vs2022   (or vs2019/vs2017/vs2015)
-- Then open build\cod4mirror.sln in Visual Studio, set Win32 + Release, Build.
-- Output: build\bin\Win32\Release\d3d9.dll  (rename / drop next to iw3mp.exe).

workspace "cod4mirror"
	location      "./build"
	architecture  "x86"           -- iw3mp.exe is 32-bit
	configurations { "Debug", "Release" }
	platforms     { "Win32" }

	filter "platforms:Win32"
		architecture "x86"
	filter {}

project "cod4mirror"
	kind        "SharedLib"
	language    "C++"
	cppdialect  "C++17"
	targetname  "d3d9"            -- *** must export as d3d9.dll ***
	targetdir   "%{wks.location}/bin/%{cfg.platform}/%{cfg.buildcfg}"
	objdir      "%{wks.location}/obj/%{cfg.platform}/%{cfg.buildcfg}"

	files {
		"src/**.h",
		"src/**.cpp",
		"src/**.def",
	}

	includedirs { "src" }

	-- d3d9 import lib (only for type defs / __uuidof; we don't link to it for
	-- exports because we resolve via GetProcAddress at runtime).
	links { "d3d9" }

	-- force .def-file exports so __stdcall names stay undecorated and match
	-- the real d3d9.dll's export table. Path is relative to the generated
	-- vcxproj which lives in build/ so we step up once into src/.
	linkoptions { "/DEF:\"..\\src\\d3d9.def\"" }

	-- modern premake5 (5.0.0-beta7+) replaces flags{...} with dedicated APIs
	multiprocessorcompile "On"
	incrementallink "Off"

	defines { "WIN32", "_WINDOWS", "_USRDLL", "NOMINMAX", "WIN32_LEAN_AND_MEAN" }

	filter "configurations:Debug"
		defines  { "_DEBUG" }
		runtime  "Debug"
		symbols  "On"
		optimize "Off"
	filter {}

	filter "configurations:Release"
		defines  { "NDEBUG" }
		runtime  "Release"
		symbols  "On"          -- keep PDB for crash debugging
		optimize "Speed"
		linktimeoptimization "On"
	filter {}

	filter "system:windows"
		systemversion "latest"
		staticruntime "On"     -- ship without VC++ runtime dep on user PC
	filter {}
