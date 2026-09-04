-- devbench — SKSE MCP/REST host (general mod-dev test bench)

-- minimum xmake version
set_xmakever("2.8.2")

-- commonlibsse-ng options
set_config("rex_ini", true)
set_config("skse_xbyak", true)

-- includes
includes("lib/commonlibsse-ng")
includes("xmake/cpp-mcp.lua")

-- project
set_project("devbench")
set_license("GPL-3.0")

local version = "1.16.0"
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
-- Decode-only image loading for the `capture` tool's native SSIM comparison (two already-
-- captured PNGs -> pixel buffers). Same single-header library Open Shaders already vendors
-- for its own screenshot encode/decode -- no new dependency ecosystem, just the one everyone
-- adjacent to this already uses.
add_requires("stb")

-- Local package repo: pins the optional SMF3 client API header (single header, pulled at build
-- time, runtime GetProcAddress — inert when SMF is not installed). See xmake-pkgs/.
add_repositories("devbench-pkgs xmake-pkgs")
add_requires("skse-menu-framework-api 3.7.0")
-- FUCK: an alternative in-game menu + keybind framework. Header-only client API (runtime
-- GetProcAddress on FUCK.dll — inert when absent). Its header pulls in the real imgui.h for
-- types and references CSimpleIniA, so its UI target needs imgui + simpleini.
add_requires("fuck-api 1.0.0")
add_requires("imgui")
add_requires("simpleini")

-- The SMF-hosted in-game menu lives in its own PCH-free static lib: the SMF client header's
-- cimgui-style typedefs cannot coexist with the real imgui.h the main target's PCH pulls in.
target("devbench-UI")
set_kind("static")
set_warnings("all")
add_deps("commonlibsse-ng")
add_packages("skse-menu-framework-api", "nlohmann_json")
add_defines("_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING") -- SMF header uses std::wstring_convert
add_defines("UNICODE", "_UNICODE", "_WINSOCKAPI_")
add_files("src/RecordingsMenu.cpp")
add_includedirs("src")
target_end()

-- The FUCK-hosted in-game menu also lives in its own PCH-free static lib: FUCK_API.h pulls in the
-- real imgui.h, which cannot coexist with SMF's cimgui ImGuiMCP (devbench-UI) or the main PCH.
target("devbench-UI-fuck")
set_kind("static")
set_warnings("all")
add_deps("commonlibsse-ng")
add_packages("fuck-api", "imgui", "simpleini", "nlohmann_json")
add_defines("UNICODE", "_UNICODE", "_WINSOCKAPI_")
add_files("src/RecordingsMenuFuck.cpp")
add_includedirs("src")
target_end()

-- target
target("devbench")
add_deps("commonlibsse-ng", "cpp-mcp", "devbench-UI", "devbench-UI-fuck")
add_packages("nlohmann_json", "stb")

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
-- RecordingsMenu.cpp is SMF/cimgui and PCH-free — it belongs only to devbench-UI. The glob above
-- would otherwise also compile it here with the real-imgui PCH, which conflicts.
remove_files("src/RecordingsMenu.cpp")
-- RecordingsMenuFuck.cpp is FUCK/real-imgui and PCH-free — it belongs only to devbench-UI-fuck.
remove_files("src/RecordingsMenuFuck.cpp")
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

-- Host-independent unit tests. Compiles only the pure-logic production TUs
-- (ToolRegistry) and header-only seams (McpContent) against a no-op `logs` stub
-- (tests/pch.h), so it builds and runs on CI WITHOUT CommonLibSSE-NG/SKSE or a
-- running game. Not built by default (`xmake` / `xmake build devbench` skip it);
-- build with `xmake build devbench-tests`, run with `xmake run devbench-tests`.
target("devbench-tests")
set_kind("binary")
set_default(false)
set_languages("c++23")
add_packages("nlohmann_json", "stb")
add_includedirs("src")
add_files("tests/*.cpp")
add_files("src/ToolRegistry.cpp") -- exercised directly; pure logic, no game deps
add_files("src/Ssim.cpp") -- exercised directly; pure logic, no game deps
add_headerfiles("tests/*.h")
set_pcxxheader("tests/pch.h")
add_defines("_WINSOCKAPI_")
-- /EHsc: the suite asserts on thrown exceptions (CHECK_THROWS). /utf-8 matches
-- the main target so the shared nlohmann_json headers parse identically.
add_cxflags("/utf-8", "/EHsc", { force = true })
target_end()
