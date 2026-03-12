set_xmakever("3.0.5")
set_policy("check.auto_ignore_flags", false)
PROJECT_NAME = "Cycle"

set_project(PROJECT_NAME)
set_version("1.3.0")
set_languages("cxx23")
set_toolchains("clang-cl")
set_license("AGPL-3.0")

add_defines("UNICODE", "_UNICODE")

includes("lib/CommonLibSSE-NG")

add_rules("mode.debug", "mode.release")

if is_mode("debug") then
    set_optimize("none")
    add_defines("DEBUG")
elseif is_mode("release") then
    set_optimize("fastest")
    add_defines("NDEBUG")
    set_symbols("debug")
end

add_requires("nlohmann_json", "magic_enum")
add_linkdirs("C:/Program Files (x86)/Windows Kits/10/Lib/10.0.26100.0/um/x64")
add_linkdirs("C:/Program Files (x86)/Windows Kits/10/Lib/10.0.26100.0/ucrt/x64")
add_linkdirs("C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207/lib/x64")
add_includedirs("C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207/atlmfc/include")
add_requires("spdlog", { configs = { header_only = false, wchar = true, std_format = true } })

target(PROJECT_NAME)

    add_deps("commonlibsse-ng")
    add_rules("commonlibsse-ng.plugin", {
        name = PROJECT_NAME,
        author = "Acook1e, 墨良",
        description = "SexLab Inflation Framework NG",
        options = {
            address_library = true,
            signature_scanning = false,
            struct_dependent = false
        }
    })
    add_cxxflags("-Wno-unused-command-line-argument")
    add_packages("nlohmann_json")
    add_packages("magic_enum")
    add_packages("spdlog")

    add_files("src/**.cpp")
    add_includedirs("include/")
    add_headerfiles("include/**.h")
    set_pcxxheader("include/PCH.h")

    after_build(function (target)
        os.vcp(target:targetfile(), "dist/SKSE/Plugins")
        os.vcp(target:symbolfile(), "dist/SKSE/Plugins")
    end)
