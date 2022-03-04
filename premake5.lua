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
    location "projects/build/kcp"
    objdir "projects/obj/%{cfg.project.name}/%{cfg.platform}_%{cfg.buildcfg}"
    targetdir "projects/bin/%{cfg.buildcfg}"

    kind "StaticLib"
    language "C"
    includedirs {
        "./",
        "./kcp",
        }
    files {"./kcp/**.h", "./kcp/**.c"}


project "tcp_over_kcp"
    location "projects/build/tcp_over_kcp"
    objdir "projects/obj/%{cfg.project.name}/%{cfg.platform}_%{cfg.buildcfg}"
    targetdir "projects/bin/%{cfg.buildcfg}"

    kind "ConsoleApp"
    language "C++"
    includedirs {
        "./",
        "./src",
        }
    files {"./src/**.h", "./src/**.hpp","./src/**.cpp"}
    defines {
        "ASIO_STANDALONE" ,
        "ASIO_NO_DEPRECATED",
    }
    links{"kcp"}
    filter { "system:windows" }
        cppdialect "C++17"
        defines {"_WIN32_WINNT=0x0601"}
        buildoptions { "/await" }
    filter {"system:linux"}
        buildoptions {"-std=c++20", "-stdlib=libc++"}
        links{"pthread", "c++", "c++abi"}
        linkoptions {"-static-libstdc++ -static-libgcc","-Wl,-rpath=./","-Wl,--as-needed"}
        --linkoptions {"-Wl,-rpath=./","-Wl,--as-needed"}
    filter "configurations:Debug"
        targetsuffix "-d"
    filter{"configurations:*"}
        postbuildcommands{"{COPY} %{cfg.buildtarget.abspath} %{wks.location}"}
