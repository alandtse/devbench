package("cpp-mcp")
do
    set_homepage("https://github.com/hkr04/cpp-mcp")
    set_description("C++ Model Context Protocol server/client. devbench builds only the server TUs.")
    set_license("MIT")

    add_urls("https://github.com/hkr04/cpp-mcp.git")
    -- Pin to the v2025.03.26 tree. The commit/patch hashes below are placeholders to
    -- finalize on the first `xmake` run: xmake reports the expected sha256 for each;
    -- paste them in (or pin to the exact commit Community Shaders' submodule uses).
    add_versions("2025.03.26", "0000000000000000000000000000000000000000")

    -- Local patch: expose httplib::Server so the REST facade shares the MCP port.
    add_patches("2025.03.26",
        path.join(os.scriptdir(), "patches", "cpp-mcp-expose-http.patch"),
        "0000000000000000000000000000000000000000000000000000000000000000")

    add_deps("nlohmann_json")

    on_install("windows", function(package)
        -- Single JSON ABI: point cpp-mcp at the same nlohmann_json devbench uses,
        -- instead of its vendored common/json.hpp (mirrors Community Shaders' fix —
        -- otherwise the ABI-tagged inline namespaces differ and the link fails).
        io.replace("include/mcp_message.h",
            "#include \"json.hpp\"", "#include <nlohmann/json.hpp>", { plain = true })

        -- Upstream ships no install rules (PR #12 open), so build the server TUs into
        -- a static lib ourselves. Client TUs (stdio/SSE) are intentionally omitted.
        io.writefile("xmake.lua", [[
            add_rules("mode.releasedbg")
            add_requires("nlohmann_json")
            target("cpp-mcp")
                set_kind("static")
                set_languages("c++17")
                add_files("src/mcp_message.cpp", "src/mcp_resource.cpp",
                          "src/mcp_server.cpp", "src/mcp_tool.cpp")
                add_includedirs("include", "common", { public = true })
                add_headerfiles("include/(*.h)", "common/(*.h)", "common/(*.hpp)")
                add_defines("MCP_MAX_SESSIONS=10", "MCP_SESSION_TIMEOUT=30",
                            "_WINSOCKAPI_", "_CRT_SECURE_NO_WARNINGS", { public = true })
                add_cxflags("/utf-8", "/bigobj", "/W0", { tools = "cl" })
                add_packages("nlohmann_json")
                add_syslinks("ws2_32", "crypt32")
        ]])
        import("package.tools.xmake").install(package, {})
    end)

    on_test(function(package)
        assert(package:has_cxxincludes("mcp_server.h"))
    end)
end
package_end()
