#include "ModalWatch.h"

#include "GameEvents.h"
#include "ModalLogic.h"
#include "Notices.h"

#include <algorithm>
#include <mutex>
#include <set>

namespace dvb::ModalWatch
{
	namespace
	{
		EventBus*             g_events = nullptr;
		std::mutex            g_mutex;
		std::set<std::string> g_autoDismiss;

		ToolRegistry::Annotation Annotate()
		{
			ToolRegistry::Annotation out;
			out.fields = CurrentFields();
			if (const auto it = out.fields.find("blockedBy"); it != out.fields.end()) {
				for (const auto& b : *it)
					out.summary += std::format("blocked by {}: {}; ", b.value("name", ""), b.value("hint", ""));
			}
			if (const auto it = out.fields.find("notices"); it != out.fields.end()) {
				for (const auto& n : *it)
					out.summary += std::format("{} notice from {}: {}; ", n.value("severity", ""), n.value("source", ""), n.value("text", ""));
			}
			if (!out.summary.empty())
				out.summary.resize(out.summary.size() - 2);
			return out;
		}

		json NoticeHandler(const json& a_args, const ToolContext&)
		{
			const std::string action = a_args.value("action", std::string("list"));
			auto&             board = Notices::Global();
			if (action == "list")
				return json{ { "notices", Notices::ToJson(board.Active(Notices::NowMs())) } };

			const std::string key = a_args.value("key", std::string{});
			if (key.empty())
				throw ToolError(400, "action '" + action + "' requires a 'key'");
			if (action == "clear")
				return json{ { "cleared", board.Clear(key) } };
			if (action != "post")
				throw ToolError(400, "unknown action '" + action + "' (list|post|clear)");

			const std::string text = a_args.value("text", std::string{});
			if (text.empty())
				throw ToolError(400, "action 'post' requires 'text'");
			Notices::Severity severity = Notices::Severity::kInfo;
			if (a_args.contains("severity") && !Notices::ParseSeverity(a_args.value("severity", std::string{}), severity))
				throw ToolError(400, "severity must be info, warning or critical");
			std::uint32_t ttlMs = 0;
			if (a_args.contains("ttlMs")) {
				const auto& v = a_args["ttlMs"];
				if (!v.is_number_unsigned() || v.get<std::uint64_t>() > Notices::kMaxTtlMs)
					throw ToolError(400, std::format("ttlMs must be an integer from 0 to {}", Notices::kMaxTtlMs));
				ttlMs = v.get<std::uint32_t>();
			}
			board.Post(key, a_args.value("source", std::string("agent")), severity, text, ttlMs, Notices::NowMs());
			return json{ { "posted", true }, { "key", key } };
		}
	}

	void Init(ToolRegistry& a_registry, EventBus& a_events, std::vector<std::string> a_autoDismissMenus)
	{
		g_events = &a_events;
		SetAutoDismiss(a_autoDismissMenus);
		a_registry.SetAnnotator(Annotate);

		ToolDescriptor notice;
		notice.name = "notice";
		notice.description =
			"Post a message that every tool reply carries (a `notices` array, most severe first) until it is cleared or "
			"expires, so an agent or a headless script sees a critical condition on its very next call. "
			"action='post' takes key (a repeat replaces it), text, severity info|warning|critical, optional source and "
			"ttlMs (0 = until cleared); 'clear' takes key; 'list' returns the active notices. Replies also carry "
			"`blockedBy` while a blocking menu (a tutorial, a message box) is open; pass noNotices=true on any call to "
			"leave both out.";
		notice.inputSchema = json{
			{ "type", "object" },
			{ "properties", json{
								{ "action", { { "type", "string" }, { "enum", { "list", "post", "clear" } } } },
								{ "key", { { "type", "string" } } },
								{ "text", { { "type", "string" } } },
								{ "severity", { { "type", "string" }, { "enum", { "info", "warning", "critical" } } } },
								{ "source", { { "type", "string" } } },
								{ "ttlMs", { { "type", "integer" }, { "minimum", 0 }, { "maximum", Notices::kMaxTtlMs } } },
							} }
		};
		a_registry.Register(std::move(notice), NoticeHandler);
	}

	json CurrentFields()
	{
		json       out = json::object();
		const auto inventory = GetMenuInventory();
		const auto blockers = Modal::Blockers(inventory.openMenus);
		if (!inventory.initialized)
			out["menuInventoryInitialized"] = false;
		if (!blockers.empty()) {
			json list = json::array();
			for (const auto& b : blockers)
				list.push_back(json{ { "name", b.name }, { "hint", b.hint } });
			out["blockedBy"] = std::move(list);
		}
		if (auto notices = Notices::Global().Active(Notices::NowMs()); !notices.empty())
			out["notices"] = Notices::ToJson(notices);
		return out;
	}

	void SetAutoDismiss(const std::vector<std::string>& a_names)
	{
		std::lock_guard lock(g_mutex);
		g_autoDismiss.clear();
		for (const auto& n : a_names)
			if (Modal::CanAutoDismiss(n))
				g_autoDismiss.insert(n);
	}

	std::vector<std::string> AutoDismiss()
	{
		std::lock_guard lock(g_mutex);
		return { g_autoDismiss.begin(), g_autoDismiss.end() };
	}

	void OnMenuEvent(const std::string& a_name, bool a_opening)
	{
		if (!g_events || Modal::IsBenign(a_name))
			return;
		if (!a_opening) {
			const auto inventory = GetMenuInventory();
			if (inventory.initialized && Modal::Blockers(inventory.openMenus).empty())
				g_events->Publish("modal.cleared", json{ { "name", a_name } });
			return;
		}
		bool dismiss = false;
		{
			std::lock_guard lock(g_mutex);
			dismiss = g_autoDismiss.contains(a_name);
		}
		g_events->Publish("modal.blocked", json{ { "name", a_name }, { "hint", Modal::HintFor(a_name) }, { "autoDismiss", dismiss } });
		if (!dismiss)
			return;
		if (auto* task = SKSE::GetTaskInterface())
			task->AddTask([a_name]() {
				if (auto* q = RE::UIMessageQueue::GetSingleton())
					q->AddMessage(a_name.c_str(), RE::UI_MESSAGE_TYPE::kHide, nullptr);
			});
	}
}
