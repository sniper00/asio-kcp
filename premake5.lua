workspace "tcp-over-kcp"
    configurations { "Debug", "Release" }
    flags{"NoPCH","RelativeLinks"}
    location "./"
    architecture "x64"

    libdirs("./")

    filter "configurations:Debug"
        defines { "DEBUG" }
        symbols "On"
        libdirs("./libs/Debug")

    filter "configurations:Release"
        defines { "NDEBUG" }
        optimize "On"
        symbols "On"
        libdirs("./libs/Release")

    filter {"system:windows"}
        characterset "MBCS"
        systemversion "latest"
        warnings "Extra"

    filter { "system:linux" }
        warnings "High"

    filter { "system:macosx" }
        warnings "High"

project "kcp"
    location "build/projects/kcp"
    objdir "build/obj/%{cfg.project.name}/%{cfg.platform}_%{cfg.buildcfg}"
    targetdir "build/bin/%{cfg.buildcfg}"

    kind "StaticLib"
    language "C"
    includedirs {
        "./3rd/kcp",
        }
    files {"./3rd/**.h", "./3rd/**.c"}


project "tcp_over_kcp"
    location "build/projects/tcp_over_kcp"
    objdir "build/obj/%{cfg.project.name}/%{cfg.platform}_%{cfg.buildcfg}"
    targetdir "build/bin/%{cfg.buildcfg}"

    kind "ConsoleApp"
    language "C++"
    includedirs {
        "./3rd",
        "./include",
        "./example",
        }
    files {"./example/**.h", "./example/**.hpp","./example/**.cpp"}
    defines {
        "ASIO_STANDALONE" ,
        "ASIO_NO_DEPRECATED",
        --"ASIO_ENABLE_HANDLER_TRACKING"
    }
    links{"kcp"}
    filter { "system:windows" }
        cppdialect "C++17"
        defines {"_WIN32_WINNT=0x0601"}
        buildoptions { "/await" }
    filter {"system:linux"}
        buildoptions {"-std=c++20"}
        links{"pthread"}
        linkoptions {"-static-libstdc++ -static-libgcc","-Wl,-rpath=./","-Wl,--as-needed"}
    filter "configurations:Debug"
        targetsuffix "-d"
    filter{"configurations:*"}
        postbuildcommands{"{COPY} %{cfg.buildtarget.abspath} %{wks.location}"}
