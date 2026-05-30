#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Json.h"

namespace dvb
{
	/// Per-invocation context handed to every handler. Transport-agnostic: the MCP
	/// adapter fills clientId with the session id; the REST adapter leaves it empty
	/// (stateless). Extend with cancellation / logging as needed.
	struct ToolContext
	{
		std::string clientId;  ///< opaque connection/session id ("" for stateless callers)
	};

	/// Throw from a handler to signal a non-success outcome. `code` is an HTTP-style
	/// status the REST adapter returns verbatim and the MCP adapter maps to an error.
	struct ToolError : std::runtime_error
	{
		int code;  ///< 400 bad args, 404 not found, 409 conflict, 422 unprocessable, 500 internal
		ToolError(int a_code, const std::string& a_msg) :
			std::runtime_error(a_msg), code(a_code) {}
	};

	/// A handler: JSON arguments in, JSON result out. Invoked on the server's listener
	/// thread — handlers that touch game/render state marshal to the main thread
	/// themselves (see MainThread::RunAndWait), which returns the value synchronously.
	using ToolHandler = std::function<json(const json& a_args, const ToolContext& a_ctx)>;

	/// Static description of a tool. `inputSchema` is a JSON Schema object; it doubles
	/// as the MCP tool inputSchema and the REST `GET /api/tools` documentation.
	struct ToolDescriptor
	{
		std::string name;
		std::string description;
		json inputSchema = json::object();
		bool readOnly = false;  ///< hint: side-effect-free (REST may also expose via GET)
	};

	/// Outcome of Invoke — a result payload or an error. No exception crosses the
	/// adapter boundary; ToolError / std::exception are folded into this.
	struct ToolResult
	{
		bool ok = true;
		json value;  ///< result payload when ok
		int errorCode = 0;
		std::string errorMessage;

		static ToolResult Success(json a_value) { return { true, std::move(a_value), 0, {} }; }
		static ToolResult Failure(int a_code, std::string a_msg)
		{
			return { false, json::object(), a_code, std::move(a_msg) };
		}
	};

	/// Transport-agnostic registry. Adapters (MCP, REST) reflect this and never
	/// reference individual tool names, so registering a tool exposes it on every
	/// transport at once. Registration is expected at startup; all methods are
	/// thread-safe.
	class ToolRegistry
	{
	public:
		/// Register (or replace) a tool. Returns false if it replaced an existing
		/// entry of the same name — callers may treat that as a warning.
		bool Register(ToolDescriptor a_desc, ToolHandler a_handler);

		bool Has(std::string_view a_name) const;

		/// Copy of the descriptor for `a_name`, or nullopt if unknown.
		std::optional<ToolDescriptor> Describe(std::string_view a_name) const;

		/// Snapshot of all descriptors (MCP tools/list, REST GET /api/tools).
		std::vector<ToolDescriptor> List() const;

		/// Dispatch: look up the handler, invoke it, fold any ToolError / std::exception
		/// into a ToolResult. Returns a 404 ToolResult for an unknown tool. Never throws.
		ToolResult Invoke(std::string_view a_name, const json& a_args, const ToolContext& a_ctx) const;

	private:
		struct Entry
		{
			ToolDescriptor desc;
			ToolHandler handler;
		};

		mutable std::mutex m_mutex;
		std::unordered_map<std::string, Entry> m_tools;
	};
}
