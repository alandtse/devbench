#include "ToolRegistry.h"

#include "Json.h"

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

	json CoerceArgs(const json& a_schema, const json& a_args)
	{
		if (!a_args.is_object() || !a_schema.is_object() || !a_schema.contains("properties") || !a_schema["properties"].is_object())
			return nullptr;
		json out;
		for (const auto& [key, value] : a_args.items()) {
			if (!value.is_string() || !a_schema["properties"].contains(key))
				continue;
			const json& prop = a_schema["properties"][key];
			if (!prop.is_object() || !prop.contains("type") || !prop["type"].is_string())
				continue;
			const std::string& type = prop["type"].get_ref<const std::string&>();
			const std::string& text = value.get_ref<const std::string&>();
			json               parsed;
			if (type == "boolean") {
				if (text == "true" || text == "false")
					parsed = (text == "true");
			} else if (type == "integer" || type == "number" || type == "array" || type == "object") {
				parsed = json::parse(text, nullptr, false);
				const bool fits = !parsed.is_discarded() &&
				                  ((type == "integer" && parsed.is_number_integer()) || (type == "number" && parsed.is_number()) ||
									  (type == "array" && parsed.is_array()) || (type == "object" && parsed.is_object()));
				if (!fits)
					parsed = nullptr;
			}
			if (parsed.is_null())
				continue;
			if (out.is_null())
				out = a_args;
			out[key] = std::move(parsed);
		}
		return out;
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
			auto              schema = std::make_shared<const json>(a_desc.inputSchema);
			m_tools[name] = Entry{ std::move(a_desc), std::move(a_handler), std::move(keys), std::move(schema) };
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

	void ToolRegistry::SetAnnotator(Annotator a_annotator)
	{
		std::lock_guard lock(m_annotatorMutex);
		m_annotator = std::move(a_annotator);
	}

	ToolResult ToolRegistry::Invoke(std::string_view a_name, const json& a_args, const ToolContext& a_ctx) const
	{
		const bool hasOptOut = a_args.is_object() && a_args.contains("noNotices");
		if (hasOptOut && !a_args["noNotices"].is_boolean())
			return ToolResult::Failure(400, "'noNotices' must be a boolean");
		const bool quiet = a_ctx.internal || (hasOptOut && a_args["noNotices"].get<bool>());
		ToolResult result;
		if (hasOptOut) {
			json args = a_args;
			args.erase("noNotices");
			result = InvokeCore(a_name, args, a_ctx);
		} else {
			result = InvokeCore(a_name, a_args, a_ctx);
		}
		if (quiet)
			return result;

		Annotator annotator;
		{
			std::lock_guard lock(m_annotatorMutex);
			annotator = m_annotator;
		}
		if (!annotator)
			return result;
		try {
			const Annotation note = annotator();
			if (result.ok && result.value.is_object()) {
				for (const auto& [key, value] : note.fields.items())
					if (!result.value.contains(key))
						result.value[key] = value;
			} else if (!result.ok && !note.summary.empty()) {
				result.errorMessage += std::format(" [{}]", note.summary);
			}
		} catch (const std::exception& e) {
			logs::warn("tool '{}': annotator failed: {}", a_name, e.what());
		} catch (...) {
			logs::warn("tool '{}': annotator failed", a_name);
		}
		return result;
	}

	ToolResult ToolRegistry::InvokeCore(std::string_view a_name, const json& a_rawArgs, const ToolContext& a_ctx) const
	{
		// Copy the handler out under the lock; never hold the lock across a handler
		// (handlers can block on the main thread, re-enter the registry, etc.).
		ToolHandler                 handler;
		KeySet                      knownKeys;
		std::shared_ptr<const json> schema;
		{
			std::lock_guard lock(m_mutex);
			const auto      it = m_tools.find(std::string(a_name));
			if (it == m_tools.end())
				return ToolResult::Failure(404, std::format("unknown tool '{}' (not registered in this game; see inspect kind=registrants)", a_name));
			handler = it->second.handler;
			knownKeys = it->second.knownKeys;
			schema = it->second.schema;
		}
		const json  coerced = schema ? CoerceArgs(*schema, a_rawArgs) : json(nullptr);
		const json& callArgs = coerced.is_null() ? a_rawArgs : coerced;
		ToolContext ctx = a_ctx;
		ctx.rawArgs = &a_rawArgs;

		std::string unknownKeys;
		if (knownKeys && callArgs.is_object()) {
			std::vector<std::string> unknown;
			for (const auto& [key, _] : callArgs.items())
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
			const std::string args = a_rawArgs.is_null() ? std::string{} : Dump(a_rawArgs);
			logs::debug("tool '{}' invoked {}", a_name, args.size() > 160 ? Utf8Prefix(args, 160) + "..." : args);
		}
		try {
			auto result = ToolResult::Success(handler(callArgs, ctx));
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
