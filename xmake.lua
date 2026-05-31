-- devbench — SKSE MCP/REST host (general mod-dev test bench)

-- minimum xmake version
set_xmakever("2.8.2")

-- commonlibsse-ng options
set_config("rex_ini", true)

-- includes
includes("lib/commonlibsse-ng")
includes("xmake/cpp-mcp.lua")

-- project
set_project("devbench")
set_license("GPL-3.0")

local version = "1.0.0"
local ver = version:split("%.")
set_version(version)

-- defaults
set_languages("c++23")
set_warnings("allextra")

-- policies
set_policy("package.requires_lock", true)

-- build modes
add_rules("mode.debug", "mode.releasedbg")
set_defaultmode("releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- packages
add_requires("nlohmann_json")

-- target
target("devbench")
add_deps("commonlibsse-ng", "cpp-mcp")
add_packages("nlohmann_json")

-- DLL output name
set_basename("devbench")

-- ensure winsock2 (pulled in by cpp-mcp/httplib) wins over the legacy winsock
-- that <Windows.h> would otherwise include via CommonLib.
add_defines("_WINSOCKAPI_")

-- generate PDB (releasedbg handles /Zi; /DEBUG tells the linker to emit it)
add_shflags("/DEBUG", { force = true })

-- version config vars
set_configvar("VERSION_MAJOR", tonumber(ver[1]))
set_configvar("VERSION_MINOR", tonumber(ver[2]))
set_configvar("VERSION_PATCH", tonumber(ver[3]))
set_configvar("VERSION_STRING", version)

-- commonlibsse-ng plugin (auto-generates the SKSE plugin declaration)
add_rules("commonlibsse-ng.plugin", {
    name = "devbench",
    author = "alandtse",
    description = "MCP + REST test bench host for Skyrim mod development",
})

-- sources
add_files("src/**.cpp")
add_headerfiles("src/**.h")
add_includedirs("src")
-- Public C-ABI consumer header (DevBenchAPI.h). The companion DevBenchAPI.cpp is
-- consumer-only and intentionally NOT globbed into this target.
add_includedirs("include")
add_headerfiles("include/*.h")
set_pcxxheader("src/pch.h")
add_configfiles("src/Version.h.in")
-- Version.h is generated into the build config-files dir; put it on the include
-- path so sources (main.cpp, Server.cpp) can #include "Version.h".
set_configdir("$(builddir)/.gens/devbench/$(plat)/$(arch)/$(mode)")
add_includedirs("$(builddir)/.gens/devbench/$(plat)/$(arch)/$(mode)")

-- auto deploy: set SkyrimPluginTargets to one or more game Data paths separated by ';'
after_build(function(target)
    local deploy_dirs = os.getenv("SkyrimPluginTargets")
    if not deploy_dirs then
        return
    end
    local dll = target:targetfile()
    local pdb = target:symbolfile()
    for _, dir in ipairs(deploy_dirs:split(";")) do
        dir = dir:trim()
        if dir ~= "" then
            local dest = path.join(dir, "SKSE", "Plugins")
            os.mkdir(dest)
            os.cp(dll, dest)
            if os.isfile(pdb) then
                os.cp(pdb, dest)
            end
        end
    end
end)
