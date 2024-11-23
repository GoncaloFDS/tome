group "third_party"
    include "third_party/build_flecs.lua"
    include "third_party/build_spdlog.lua"
group ""

project "tome_engine"
   kind "StaticLib"
   language "C++"
   cppdialect "C++20"
   targetdir "binaries/%{cfg.buildcfg}"
   staticruntime "off"

   files { 
    "src/**.h", 
    "src/**.cpp" 
   }

   includedirs
   {
      "src",

	  -- Include ThirdParty
	  "third_party/flecs/distr",
	  "third_party/spdlog/include"
   }

   links {
      "flecs",
      "spdlog"
   }

   targetdir ("../binaries/" .. OutputDir .. "/%{prj.name}")
   objdir ("../binaries/intermediates/" .. OutputDir .. "/%{prj.name}")

   filter "system:windows"
       systemversion "latest"
       defines { }
       buildoptions {
           "/utf-8"
       }

   filter "configurations:Debug"
       defines { "DEBUG" }
       runtime "Debug"
       symbols "On"

   filter "configurations:Release"
       defines { "RELEASE" }
       runtime "Release"
       optimize "On"
       symbols "On"

   filter "configurations:Dist"
       defines { "DIST" }
       runtime "Release"
       optimize "On"
       symbols "Off"

