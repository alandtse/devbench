#include "ToolRegistry.h"

namespace dvb
{
	bool ToolRegistry::Register(ToolDescriptor a_desc, ToolHandler a_handler)
	{
		const ToolDescriptor descCopy = a_desc;  // for the listener (notified post-insert)
		RegistrationListener listener;
		bool replaced;
		{
			std::lock_guard lock(m_mutex);
			replaced = m_tools.contains(a_desc.name);
			const std::string name = a_desc.name;
			m_tools[name] = Entry{ std::move(a_desc), std::move(a_handler) };
			listener = m_onRegister;
		}
		if (listener)
			listener(descCopy);  // outside the lock — listener may re-enter / block
		return !replaced;
	}

	void ToolRegistry::SetRegistrationListener(RegistrationListener a_listener)
	{
		std::lock_guard lock(m_mutex);
		m_onRegister = std::move(a_listener);
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

		// Per-invocation trace (debug) + failures (warn) — set logLevel=debug in
		// config.json to see the full client/agent call stream for diagnosis.
		logs::debug("tool '{}' invoked", a_name);
		try {
			auto result = ToolResult::Success(handler(a_args, a_ctx));
			return result;
		} catch (const ToolError& e) {
			logs::warn("tool '{}' failed [{}]: {}", a_name, e.code, e.what());
			return ToolResult::Failure(e.code, e.what());
		} catch (const std::exception& e) {
			logs::warn("tool '{}' threw: {}", a_name, e.what());
			return ToolResult::Failure(500, e.what());
		} catch (...) {
			logs::warn("tool '{}' threw unknown", a_name);
			return ToolResult::Failure(500, "unknown error in tool handler");
		}
	}
}
