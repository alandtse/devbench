#include "ToolRegistry.h"

namespace dvb
{
	bool ToolRegistry::Register(ToolDescriptor a_desc, ToolHandler a_handler)
	{
		std::lock_guard lock(m_mutex);
		const bool replaced = m_tools.contains(a_desc.name);
		const std::string name = a_desc.name;
		m_tools[name] = Entry{ std::move(a_desc), std::move(a_handler) };
		return !replaced;
	}

	bool ToolRegistry::Has(std::string_view a_name) const
	{
		std::lock_guard lock(m_mutex);
		return m_tools.contains(std::string(a_name));
	}

	std::optional<ToolDescriptor> ToolRegistry::Describe(std::string_view a_name) const
	{
		std::lock_guard lock(m_mutex);
		const auto it = m_tools.find(std::string(a_name));
		if (it == m_tools.end())
			return std::nullopt;
		return it->second.desc;
	}

	std::vector<ToolDescriptor> ToolRegistry::List() const
	{
		std::lock_guard lock(m_mutex);
		std::vector<ToolDescriptor> out;
		out.reserve(m_tools.size());
		for (const auto& [name, entry] : m_tools)
			out.push_back(entry.desc);
		return out;
	}

	ToolResult ToolRegistry::Invoke(std::string_view a_name, const json& a_args, const ToolContext& a_ctx) const
	{
		// Copy the handler out under the lock; never hold the lock across a handler
		// (handlers can block on the main thread, re-enter the registry, etc.).
		ToolHandler handler;
		{
			std::lock_guard lock(m_mutex);
			const auto it = m_tools.find(std::string(a_name));
			if (it == m_tools.end())
				return ToolResult::Failure(404, std::format("unknown tool '{}'", a_name));
			handler = it->second.handler;
		}

		try {
			return ToolResult::Success(handler(a_args, a_ctx));
		} catch (const ToolError& e) {
			return ToolResult::Failure(e.code, e.what());
		} catch (const std::exception& e) {
			return ToolResult::Failure(500, e.what());
		} catch (...) {
			return ToolResult::Failure(500, "unknown error in tool handler");
		}
	}
}
