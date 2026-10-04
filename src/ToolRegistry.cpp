#include "ToolRegistry.h"

namespace dvb
{
	namespace
	{
		// The tool's declared top-level keys, or null when the schema doesn't enumerate them
		// (schemaless, additionalProperties:true, or a composed schema): nothing to check against.
		ToolRegistry::KeySet DeclaredKeys(const json& a_schema)
		{
			if (!a_schema.is_object() || !a_schema.contains("properties") || !a_schema["properties"].is_object())
				return nullptr;
			if (a_schema.value("additionalProperties", false) == true)
				return nullptr;
			for (const char* composed : { "oneOf", "anyOf", "allOf" })
				if (a_schema.contains(composed))
					return nullptr;
			std::set<std::string, std::less<>> keys;
			for (const auto& [key, _] : a_schema["properties"].items())
				keys.insert(key);
			return std::make_shared<const std::set<std::string, std::less<>>>(std::move(keys));
		}

		std::string Join(const std::vector<std::string>& a_items)
		{
			std::string out;
			for (const auto& item : a_items)
				out += (out.empty() ? "" : ", ") + item;
			return out;
		}
	}

	bool ToolRegistry::Register(ToolDescriptor a_desc, ToolHandler a_handler)
	{
		const ToolDescriptor descCopy = a_desc;  // for the listener (notified post-insert)
		RegistrationListener listener;
		bool                 replaced;
		{
			std::lock_guard lock(m_mutex);
			replaced = m_tools.contains(a_desc.name);
			const std::string name = a_desc.name;
			KeySet            keys = DeclaredKeys(a_desc.inputSchema);
			m_tools[name] = Entry{ std::move(a_desc), std::move(a_handler), std::move(keys) };
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
		const auto      it = m_tools.find(std::string(a_name));
		if (it == m_tools.end())
			return std::nullopt;
		return it->second.desc;
	}

	std::vector<ToolDescriptor> ToolRegistry::List() const
	{
		std::lock_guard             lock(m_mutex);
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
		KeySet      knownKeys;
		{
			std::lock_guard lock(m_mutex);
			const auto      it = m_tools.find(std::string(a_name));
			if (it == m_tools.end())
				return ToolResult::Failure(404, std::format("unknown tool '{}'", a_name));
			handler = it->second.handler;
			knownKeys = it->second.knownKeys;
		}

		std::string unknownKeys;
		if (knownKeys && a_args.is_object()) {
			std::vector<std::string> unknown;
			for (const auto& [key, _] : a_args.items())
				if (!knownKeys->contains(key))
					unknown.push_back(key);
			if (!unknown.empty()) {
				const std::vector<std::string> accepted(knownKeys->begin(), knownKeys->end());
				unknownKeys = std::format("unknown key(s) {}; accepted: {}", Join(unknown), Join(accepted));
				if (!a_ctx.internal) {
					logs::warn("tool '{}': {}", a_name, unknownKeys);
					if (m_strictArgs.load(std::memory_order_relaxed))
						return ToolResult::Failure(400, unknownKeys);
				}
			}
		}

		// Per-invocation debug + failures (warn) — set logLevel=debug in config.json to see the
		// client/agent call stream. Includes a compact args summary so a one-off call is
		// diagnosable. Skipped for scenario/replay-driven (internal) calls: a replay issues
		// hundreds of identical console steps and already logs its own start/finish summary, so
		// per-step lines were pure noise.
		if (!a_ctx.internal) {
			const std::string args = a_args.is_null() ? std::string{} : a_args.dump();
			logs::debug("tool '{}' invoked {}", a_name, args.size() > 160 ? args.substr(0, 160) + "..." : args);
		}
		try {
			auto result = ToolResult::Success(handler(a_args, a_ctx));
			if (!unknownKeys.empty() && result.value.is_object()) {
				auto& warnings = result.value["warnings"];
				if (!warnings.is_array())
					warnings = json::array();
				warnings.push_back(unknownKeys);
			}
			return result;
		} catch (const ToolError& e) {
			logs::warn("tool '{}' failed [{}]: {}", a_name, e.code, e.what());
			return ToolResult::Failure(e.code, e.what());
		} catch (const json::type_error& e) {
			// .value()/.get<T>() throwing type_error specifically means the caller sent an
			// argument of the wrong JSON type — a 400, not a 500. Deliberately NOT the broader
			// json::exception (parse_error/out_of_range/other_error can mean something server-
			// side actually broke, which the generic 500 path below should still catch).
			logs::warn("tool '{}' bad args: {}", a_name, e.what());
			return ToolResult::Failure(400, std::format("invalid arguments: {}", e.what()));
		} catch (const std::exception& e) {
			logs::warn("tool '{}' threw: {}", a_name, e.what());
			return ToolResult::Failure(500, e.what());
		} catch (...) {
			logs::warn("tool '{}' threw unknown", a_name);
			return ToolResult::Failure(500, "unknown error in tool handler");
		}
	}
}
