-- In-tree build of cpp-mcp (https://github.com/hkr04/cpp-mcp), vendored as the
-- lib/cpp-mcp submodule and pinned to commit a0eb22c (the same tree Community
-- Shaders vendors). Mirrors CS's cmake/cpp-mcp.cmake recipe.
--
-- Only the server-side TUs are compiled (mcp_message/resource/server/tool); the
-- stdio/SSE *client* implementations are intentionally omitted — devbench is a
-- server only. Upstream ships no install rules (PR #12 open), so we drive the
-- build ourselves rather than consume it as a package.
--
-- Two source edits are applied into a build-tree header mirror so the submodule
-- stays clean (see gen_patched_headers below):
--   1. mcp_message.h: `#include "json.hpp"` -> `#include <nlohmann/json.hpp>`,
--      so cpp-mcp and devbench share one nlohmann_json ABI (the vendored 3.11.3
--      and our 3.12.0 wrap their API in different ABI-tagged inline namespaces;
--      mixing them is an LNK2001 at link time).
--   2. mcp_server.h: insert a public `http()` getter returning the underlying
--      httplib::Server*, so the REST facade can mount routes on the MCP port
--      (the cpp-mcp-expose-http.patch, applied here as a string edit).

local cpp_mcp_root = path.join(os.projectdir(), "lib", "cpp-mcp")
local patched_inc = path.join(os.projectdir(), "build", "cpp-mcp-patched", "include")

target("cpp-mcp")
    set_kind("static")
    set_languages("c++17")
    set_group("extern")

    add_files(
        path.join(cpp_mcp_root, "src", "mcp_message.cpp"),
        path.join(cpp_mcp_root, "src", "mcp_resource.cpp"),
        path.join(cpp_mcp_root, "src", "mcp_server.cpp"),
        path.join(cpp_mcp_root, "src", "mcp_tool.cpp"))

    -- Patched mirror first so its mcp_message.h / mcp_server.h win over the
    -- submodule's; common/ supplies httplib.h (no shared-ABI concern there).
    add_includedirs(patched_inc, path.join(cpp_mcp_root, "common"), { public = true })

    add_defines("MCP_MAX_SESSIONS=10", "MCP_SESSION_TIMEOUT=30",
        "_WINSOCKAPI_", "_CRT_SECURE_NO_WARNINGS", { public = true })

    add_cxflags("/utf-8", "/bigobj", "/W0", "/std:c++17", { force = true })
    add_packages("nlohmann_json")
    add_syslinks("ws2_32", "crypt32")

    -- Generate the patched header mirror before compiling. on_load runs with the
    -- full os/io/raise API available (unlike the description sandbox).
    on_load(function (target)
        local root = path.join(os.projectdir(), "lib", "cpp-mcp")
        local out = path.join(os.projectdir(), "build", "cpp-mcp-patched", "include")
        if not os.isfile(path.join(root, "src", "mcp_server.cpp")) then
            raise("cpp-mcp submodule missing. Run: git submodule update --init --recursive lib/cpp-mcp")
        end
        os.mkdir(out)
        for _, hdr in ipairs(os.files(path.join(root, "include", "*.h"))) do
            local name = path.filename(hdr)
            local content = io.readfile(hdr)
            if name == "mcp_message.h" then
                if not content:find('#include "json.hpp"', 1, true) then
                    raise("cpp-mcp: expected `#include \"json.hpp\"` in mcp_message.h; upstream changed — review xmake/cpp-mcp.lua")
                end
                content = content:gsub('#include "json%.hpp"', "#include <nlohmann/json.hpp>")
            elseif name == "mcp_server.h" then
                local anchor = "\nprivate:\n    std::string host_;"
                if not content:find(anchor, 1, true) then
                    raise("cpp-mcp: expected private section anchor in mcp_server.h; upstream changed — review xmake/cpp-mcp.lua")
                end
                local getter = "\n" ..
                    "    /**\n" ..
                    "     * @brief Access the underlying httplib server to register custom routes.\n" ..
                    "     * Lets a consumer mount additional endpoints (e.g. a REST facade) on the\n" ..
                    "     * same host/port as the MCP transport. Register routes before start().\n" ..
                    "     */\n" ..
                    "    httplib::Server* http() { return http_server_.get(); }\n" ..
                    anchor
                content = content:gsub("\nprivate:\n    std::string host_;", getter, 1)
            end
            io.writefile(path.join(out, name), content)
        end
    end)
target_end()
