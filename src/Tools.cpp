#include "Tools.h"

#include "ConsoleLogCapture.h"
#include "EventBus.h"
#include "GameEvents.h"
#include "Json.h"
#include "MainThread.h"
#include "ToolRegistry.h"
#include "Version.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>

namespace dvb
{
	namespace
	{
		bool Truthy(const json& a_v)
		{
			if (a_v.is_boolean())
				return a_v.get<bool>();
			if (a_v.is_string()) {
				const auto s = a_v.get<std::string>();
				return s == "true" || s == "1";
			}
			if (a_v.is_number())
				return a_v.get<double>() != 0.0;
			return false;
		}

		// console: run a Skyrim console command; optionally fence + capture its output.
		json ConsoleHandler(const json& a_args, const ToolContext&)
		{
			const std::string action = a_args.value("action", std::string("exec"));

			if (action == "read") {
				// Slice ConsoleLog's buffer between the fence markers, on the main thread
				// (the buffer is written there). markersFound=true → lines are exactly the
				// fenced command's output.
				return MainThread::RunAndWait([]() -> json {
					const auto r = ConsoleLogCapture::ReadFenced(200);
					json arr = json::array();
					for (const auto& l : r.lines)
						arr.push_back(l);
					return json{
						{ "markersFound", r.sawBegin && r.sawEnd },
						{ "sawBegin", r.sawBegin },
						{ "sawEnd", r.sawEnd },
						{ "count", arr.size() },
						{ "lines", std::move(arr) },
					};
				});
			}
			if (action != "exec")
				throw ToolError(400, std::format("unknown action '{}'", action));

			const std::string command = a_args.value("command", std::string{});
			if (command.empty())
				throw ToolError(400, "missing required parameter 'command'");

			auto* task = SKSE::GetTaskInterface();
			if (!task)
				throw ToolError(500, "SKSE TaskInterface unavailable");

			const bool capture = a_args.contains("capture") && Truthy(a_args["capture"]);

			// ExecuteCommand is deferred (GFx console drains queued commands on a later
			// tick). Fence the real command between two invalid marker commands so a later
			// action='read' can slice ConsoleLog's buffer between their echoed tokens.
			// Capture `command` by value so it outlives this lambda.
			task->AddTask([command, capture]() {
				if (capture)
					RE::Console::ExecuteCommand(ConsoleLogCapture::kMarkerBegin);
				RE::Console::ExecuteCommand(command.c_str());
				if (capture)
					RE::Console::ExecuteCommand(ConsoleLogCapture::kMarkerEnd);
			});

			return json{ { "queued", true }, { "command", command }, { "capturing", capture } };
		}

		namespace fs = std::filesystem;

		// Resolve the saves directory: explicit `dir` arg wins (escape hatch for exotic
		// setups); else <My Games game folder> / sLocalSavePath (read from the INI; the
		// 's' prefix guarantees a string), supporting an absolute setting. Running
		// in-process means MO2/USVFS-virtualized saves resolve correctly. Caveat:
		// bUseMyGamesDirectory=0 (saves under the install dir) isn't handled — pass `dir`.
		fs::path ResolveSaveDir(const json& a_args)
		{
			if (const std::string dirArg = a_args.value("dir", std::string{}); !dirArg.empty())
				return dirArg;
			const auto logDir = SKSE::log::log_directory();
			if (!logDir)
				throw ToolError(500, "save directory unavailable (no log dir)");
			fs::path local = "Saves";  // default sLocalSavePath
			if (auto* ini = RE::INISettingCollection::GetSingleton()) {
				if (auto* s = ini->GetSetting("sLocalSavePath:General")) {
					if (const char* sp = s->GetString(); sp && *sp)
						local = sp;
				}
			}
			return local.is_absolute() ? local : (logDir->parent_path() / local);
		}

		// .ess save stems in `a_dir` with mtime, newest first. CommonLib doesn't expose
		// the game's save-entry array, so we enumerate the directory ourselves (the stem
		// is exactly the name `load` takes).
		struct SaveEntry
		{
			std::string name;
			fs::file_time_type mtime;
		};
		std::vector<SaveEntry> EnumerateSaves(const fs::path& a_dir)
		{
			std::vector<SaveEntry> out;
			std::error_code ec;
			for (const auto& e : fs::directory_iterator(a_dir, ec)) {
				if (e.path().extension() == ".ess")
					out.push_back({ e.path().stem().string(), e.last_write_time(ec) });
			}
			std::sort(out.begin(), out.end(), [](const SaveEntry& a, const SaveEntry& b) { return a.mtime > b.mtime; });
			return out;
		}

		// Appended to load responses: a queued load is async AND may be gated by a
		// content-mismatch modal, so a bare {queued:true} would mislead a caller.
		constexpr const char* kLoadNote =
			"async — watch lifecycle 'postLoadGame' / inspect playerLoaded for completion. "
			"A content-mismatch MessageBoxMenu (Yes/No) may gate it; check `menu` action=list.";

		// game: programmatic save / load / list via BGSSaveLoadManager. loadLast gives a
		// settled real-save state for testing WITHOUT coc's heavy new-game init. Mutating
		// actions run on the main thread and are async — see kLoadNote.
		json GameHandler(const json& a_args, const ToolContext&)
		{
			const std::string action = a_args.value("action", std::string{});

			if (action == "list") {
				const fs::path saveDir = ResolveSaveDir(a_args);
				json saves = json::array();
				for (const auto& s : EnumerateSaves(saveDir))
					saves.push_back(s.name);
				return json{ { "dir", saveDir.string() }, { "count", saves.size() }, { "saves", std::move(saves) } };
			}

			auto* task = SKSE::GetTaskInterface();
			if (!task)
				throw ToolError(500, "SKSE TaskInterface unavailable");

			if (action == "loadLast") {
				// BGSSaveLoadManager::LoadMostRecentSaveGame() is a silent no-op from the
				// Main Menu (verified live), so resolve the newest .ess ourselves and load
				// it by name — the named path works from the menu.
				const fs::path saveDir = ResolveSaveDir(a_args);
				const auto saves = EnumerateSaves(saveDir);
				if (saves.empty())
					throw ToolError(404, std::format("no .ess saves in {}", saveDir.string()));
				const std::string name = saves.front().name;
				task->AddTask([name]() {
					if (auto* m = RE::BGSSaveLoadManager::GetSingleton())
						m->Load(name.c_str(), false);
				});
				logs::info("devbench: game loadLast -> '{}'", name);
				return json{ { "queued", true }, { "action", action }, { "name", name }, { "note", kLoadNote } };
			}
			if (action == "save" || action == "load") {
				const std::string name = a_args.value("name", std::string{});
				if (name.empty())
					throw ToolError(400, std::format("action '{}' requires a 'name'", action));
				const bool isSave = (action == "save");
				if (!isSave) {
					// Validate the save exists rather than silently queueing a no-op load
					// (a bare {queued:true} on a bad name is a cold-start trap for agents).
					const fs::path saveDir = ResolveSaveDir(a_args);
					const auto saves = EnumerateSaves(saveDir);
					const bool exists = std::any_of(saves.begin(), saves.end(),
						[&](const SaveEntry& s) { return s.name == name; });
					if (!exists)
						throw ToolError(404, std::format("save '{}' not found in {} — use action='list' for valid names", name, saveDir.string()));
				}
				task->AddTask([name, isSave]() {
					auto* m = RE::BGSSaveLoadManager::GetSingleton();
					if (!m)
						return;
					if (isSave)
						m->Save(name.c_str());
					else
						m->Load(name.c_str(), false);  // checkForMods=false skips the mod-mismatch modal
				});
				logs::info("devbench: game {} '{}'", action, name);
				json out{ { "queued", true }, { "action", action }, { "name", name } };
				if (!isSave)
					out["note"] = kLoadNote;
				return out;
			}
			throw ToolError(400, std::format("unknown action '{}' (list|save|load|loadLast)", action));
		}

		// menu: detect/answer menus. 'list' = open menus + messageBoxOpen (tracked live from
		// MenuOpenCloseEvent); 'describe' = the active MessageBoxMenu's body + buttons; 'accept'
		// = select a button by index (answers + dismisses); 'close' = hide a menu by name
		// (kHide). describe/accept use CommonLib's RE'd MessageBoxMenu accessors
		// (GetCurrentMessageBoxData / SelectOption) on the main thread — no detour.
		json MenuHandler(const json& a_args, const ToolContext&)
		{
			const std::string action = a_args.value("action", std::string("list"));

			if (action == "list") {
				json open = json::array();
				bool messageBox = false;
				for (const auto& m : GetOpenMenus()) {
					if (m == RE::MessageBoxMenu::MENU_NAME)
						messageBox = true;
					open.push_back(m);
				}
				return json{ { "openMenus", std::move(open) }, { "messageBoxOpen", messageBox } };
			}

			if (action == "close") {
				// Dismiss a menu by name via the UI message queue (kHide). For a modal
				// MessageBoxMenu this cancels/closes it — unblocking automated flows (e.g.
				// new-game popups). Marshalled to the main thread.
				const std::string name = a_args.value("name", std::string{});
				if (name.empty())
					throw ToolError(400, "action 'close' requires a 'name' (menu to hide)");
				auto* task = SKSE::GetTaskInterface();
				if (!task)
					throw ToolError(500, "SKSE TaskInterface unavailable");
				task->AddTask([name]() {
					if (auto* q = RE::UIMessageQueue::GetSingleton())
						q->AddMessage(name.c_str(), RE::UI_MESSAGE_TYPE::kHide, nullptr);
				});
				return json{ { "queued", true }, { "action", "close" }, { "name", name } };
			}

			if (action == "describe") {
				// Read the active MessageBoxMenu (body + buttons) via CommonLib's RE'd
				// GetCurrentMessageBoxData() — on the main thread (touches the live UI queue).
				return MainThread::RunAndWait([]() -> json {
					auto* data = RE::MessageBoxMenu::GetCurrentMessageBoxData();
					if (!data)
						return json{ { "messageBoxOpen", false } };
					json buttons = json::array();
					for (const auto& b : data->buttonText)
						buttons.push_back(b.c_str() ? std::string(b.c_str()) : std::string{});
					return json{
						{ "messageBoxOpen", true },
						{ "bodyText", data->bodyText.c_str() ? std::string(data->bodyText.c_str()) : std::string{} },
						{ "buttons", std::move(buttons) },
						{ "cancelIndex", data->cancelOptionIndex },
					};
				});
			}

			if (action == "accept") {
				// Answer the active MessageBoxMenu by button index (default 0) via CommonLib's
				// SelectOption() — runs the modal's callback and dismisses it; no detour.
				const int index = a_args.value("index", 0);
				auto* task = SKSE::GetTaskInterface();
				if (!task)
					throw ToolError(500, "SKSE TaskInterface unavailable");
				task->AddTask([index]() { RE::MessageBoxMenu::SelectOption(index); });
				return json{ { "queued", true }, { "action", "accept" }, { "index", index } };
			}

			throw ToolError(400, std::format("unknown action '{}' (list|close|describe|accept)", action));
		}

		// inspect: read live game state. Demonstrates the value-returning primitive —
		// the read runs on the main thread and its result is returned synchronously.
		json InspectHandler(const json& a_args, const ToolContext&)
		{
			const std::string kind = a_args.value("kind", std::string("state"));
			if (kind != "state")
				throw ToolError(400, std::format("unknown kind '{}'", kind));

			return MainThread::RunAndWait([]() -> json {
				auto* pc = RE::PlayerCharacter::GetSingleton();
				const bool loaded = pc && pc->Get3D() != nullptr;
				return json{
					{ "plugin", "devbench" },
					{ "version", DEVBENCH_VERSION_STRING },
					{ "vr", REL::Module::IsVR() },
					{ "playerLoaded", loaded },
				};
			});
		}

		// ---- scenario: server-side timed replay of a step list -------------------
		// Runs on the listener thread, so it may sleep/poll directly and marshal each
		// action to the main thread. Steps are one of: { "tool", "args" } (dispatch a
		// registered tool — so any tool, incl. consumer-registered ones, is replayable),
		// { "wait": ms } (fixed pacing), { "waitFor": ... } (block on a Skyrim EVENT),
		// or { "waitUntil": cond } (poll live state). Prefer waitFor over wait: it keys
		// off the real signal (e.g. a load is done when lifecycle:postLoadGame fires).

		using namespace std::chrono;

		bool IsLifecycleName(const std::string& a_s)
		{
			return a_s == "dataLoaded" || a_s == "newGame" || a_s == "preLoadGame" ||
			       a_s == "postLoadGame" || a_s == "saveGame" || a_s == "deleteGame";
		}

		// Every key/value in `a_match` present and equal in `a_payload` (subset match).
		bool PayloadMatches(const json& a_payload, const json& a_match)
		{
			if (!a_match.is_object())
				return true;
			for (const auto& [k, v] : a_match.items()) {
				if (!a_payload.contains(k) || a_payload[k] != v)
					return false;
			}
			return true;
		}

		struct WaitForSpec
		{
			std::string topic;
			json match;
		};

		// Normalize a step's "waitFor" into {topic, match}. String shorthands: a
		// lifecycle event name → {lifecycle,{event}}; "menuOpened"/"menuClosed" (with the
		// step's "name") → {menu,{name,opening}}. An object is {topic, match} verbatim.
		WaitForSpec ParseWaitFor(const json& a_step)
		{
			const json& wf = a_step["waitFor"];
			WaitForSpec spec;
			if (wf.is_string()) {
				const std::string s = wf.get<std::string>();
				if (IsLifecycleName(s)) {
					spec.topic = "lifecycle";
					spec.match = json{ { "event", s } };
				} else if (s == "menuOpened" || s == "menuClosed") {
					spec.topic = "menu";
					spec.match = json{ { "name", a_step.value("name", std::string{}) }, { "opening", s == "menuOpened" } };
				} else {
					throw ToolError(400, std::format("unknown waitFor shorthand '{}'", s));
				}
			} else if (wf.is_object()) {
				spec.topic = wf.value("topic", std::string{});
				spec.match = wf.value("match", json::object());
				if (spec.topic.empty())
					throw ToolError(400, "waitFor object requires a 'topic'");
			} else {
				throw ToolError(400, "waitFor must be a string shorthand or {topic, match}");
			}
			return spec;
		}

		// Live-state conditions for waitUntil. playerLoaded marshals to the main thread;
		// a mid-load stall (RunAndWait 504) just means "not yet" → keep polling. Menu
		// conditions read the thread-safe tracked set (no marshal).
		bool CheckState(const std::string& a_cond)
		{
			if (a_cond == "playerLoaded") {
				try {
					const json r = MainThread::RunAndWait([]() -> json {
						auto* pc = RE::PlayerCharacter::GetSingleton();
						return pc && pc->Get3D() != nullptr;
					},
						milliseconds(2000));
					return r.get<bool>();
				} catch (const ToolError&) {
					return false;  // main thread stalled mid-load — condition not met yet
				}
			}
			if (a_cond == "noModal") {
				for (const auto& m : GetOpenMenus())
					if (m == RE::MessageBoxMenu::MENU_NAME)
						return false;
				return true;
			}
			if (a_cond == "noMenu")
				return GetOpenMenus().empty();
			throw ToolError(400, std::format("unknown waitUntil condition '{}' (playerLoaded|noModal|noMenu)", a_cond));
		}

		json ScenarioHandler(const json& a_args, const ToolContext& a_ctx,
			const ToolRegistry& a_registry, const EventBus& a_events)
		{
			if (!a_args.contains("steps") || !a_args["steps"].is_array())
				throw ToolError(400, "scenario requires a 'steps' array");
			const json& steps = a_args["steps"];

			int repeat = a_args.value("repeat", 1);
			if (repeat < 1)
				repeat = 1;
			if (repeat > 1000)
				throw ToolError(400, "repeat capped at 1000");
			const bool continueOnError = a_args.value("continueOnError", false);

			json results = json::array();
			const auto t0 = steady_clock::now();
			bool anyFailure = false;
			bool aborted = false;

			for (int rep = 0; rep < repeat && !aborted; ++rep) {
				for (size_t i = 0; i < steps.size() && !aborted; ++i) {
					const json& step = steps[i];
					json r{ { "index", i } };
					if (repeat > 1)
						r["repeat"] = rep;
					const auto stepStart = steady_clock::now();
					bool stepFailed = false;

					try {
						if (step.contains("wait")) {
							const long ms = step["wait"].get<long>();
							r["kind"] = "wait";
							r["ms"] = ms;
							std::this_thread::sleep_for(milliseconds(ms));
						} else if (step.contains("waitFor")) {
							const WaitForSpec spec = ParseWaitFor(step);
							const long timeoutMs = step.value("timeoutMs", static_cast<long>(60000));
							const long pollMs = step.value("pollMs", static_cast<long>(100));
							r["kind"] = "waitFor";
							r["topic"] = spec.topic;
							r["match"] = spec.match;
							// Only events published after this step begins count.
							uint64_t since = a_events.HeadSeq();
							bool satisfied = false;
							const auto deadline = steady_clock::now() + milliseconds(timeoutMs);
							while (steady_clock::now() < deadline) {
								for (const auto& ev : a_events.Since(since)) {
									since = ev.seq;
									if (ev.topic == spec.topic && PayloadMatches(ev.payload, spec.match)) {
										satisfied = true;
										break;
									}
								}
								if (satisfied)
									break;
								std::this_thread::sleep_for(milliseconds(pollMs));
							}
							r["satisfied"] = satisfied;
							if (!satisfied) {
								r["timedOut"] = true;
								stepFailed = true;
							}
						} else if (step.contains("waitUntil")) {
							const std::string cond = step["waitUntil"].get<std::string>();
							const long timeoutMs = step.value("timeoutMs", static_cast<long>(30000));
							const long pollMs = step.value("pollMs", static_cast<long>(250));
							r["kind"] = "waitUntil";
							r["cond"] = cond;
							bool satisfied = false;
							const auto deadline = steady_clock::now() + milliseconds(timeoutMs);
							do {
								if (CheckState(cond)) {
									satisfied = true;
									break;
								}
								std::this_thread::sleep_for(milliseconds(pollMs));
							} while (steady_clock::now() < deadline);
							r["satisfied"] = satisfied;
							if (!satisfied) {
								r["timedOut"] = true;
								stepFailed = true;
							}
						} else if (step.contains("tool")) {
							const std::string tool = step["tool"].get<std::string>();
							const json args = step.value("args", json::object());
							r["kind"] = "tool";
							r["tool"] = tool;
							if (step.contains("label"))
								r["label"] = step["label"];
							const ToolResult tr = a_registry.Invoke(tool, args, a_ctx);
							r["ok"] = tr.ok;
							if (tr.ok) {
								r["result"] = tr.value;
							} else {
								r["errorCode"] = tr.errorCode;
								r["error"] = tr.errorMessage;
								stepFailed = true;
							}
						} else {
							throw ToolError(400, std::format("step {} has none of wait/waitFor/waitUntil/tool", i));
						}
					} catch (const ToolError& e) {
						if (!r.contains("kind"))
							r["kind"] = "error";
						r["ok"] = false;
						r["errorCode"] = e.code;
						r["error"] = e.what();
						stepFailed = true;
					}

					r["elapsedMs"] = duration_cast<milliseconds>(steady_clock::now() - stepStart).count();
					results.push_back(std::move(r));

					if (stepFailed) {
						anyFailure = true;
						if (!continueOnError)
							aborted = true;
					}
				}
			}

			return json{
				{ "ok", !anyFailure },
				{ "aborted", aborted },
				{ "stepsRun", results.size() },
				{ "elapsedMs", duration_cast<milliseconds>(steady_clock::now() - t0).count() },
				{ "results", std::move(results) },
			};
		}
	}

	void RegisterCoreTools(ToolRegistry& a_registry, EventBus& a_events)
	{
		ToolDescriptor console;
		console.name = "console";
		console.description =
			"Run a Skyrim console command. action='exec' (default) queues `command` onto the main "
			"thread (runs next tick). With capture=true it is fenced between marker commands; a "
			"later action='read' slices ConsoleLog's buffer between the markers and returns the "
			"command's output as { markersFound, lines:[…] }. Useful for printing commands "
			"(getav, getgs, getpos, help). Read promptly after exec — heavy ConsoleLog spam can "
			"scroll the markers out of the buffer (then markersFound=false, no wrong data).";
		console.inputSchema = json{
			{ "type", "object" },
			{ "properties", json{
									 { "action", json{ { "type", "string" }, { "enum", json::array({ "exec", "read" }) }, { "description", "'exec' (default) runs `command`; 'read' returns the fenced output and closes the window" } } },
									 { "command", json{ { "type", "string" }, { "description", "the console command, exactly as typed after ~ (required for exec)" } } },
									 { "capture", json{ { "type", "boolean" }, { "description", "exec: fence and capture this command's output for the next read" } } },
								 } },
		};
		a_registry.Register(std::move(console), &ConsoleHandler);

		ToolDescriptor game;
		game.name = "game";
		game.description =
			"Save/load and list saves. action='list' enumerates the saves directory and "
			"returns save names (use one as 'name' for load); 'loadLast' loads the most recent "
			"save (a settled real-game state — avoids coc's heavy new-game init); 'load'/'save' "
			"take a 'name' ('load' skips the mod-mismatch confirmation modal). All but 'list' "
			"are fire-and-forget; watch lifecycle events / inspect playerLoaded for completion.";
		game.inputSchema = json{
			{ "type", "object" },
			{ "properties", json{
									 { "action", json{ { "type", "string" }, { "enum", json::array({ "list", "save", "load", "loadLast" }) }, { "description", "list | save | load | loadLast" } } },
									 { "name", json{ { "type", "string" }, { "description", "save file name (required for save/load; from action='list')" } } },
									 { "dir", json{ { "type", "string" }, { "description", "list only: override the saves directory (default resolves from sLocalSavePath)" } } },
								 } },
		};
		a_registry.Register(std::move(game), &GameHandler);

		ToolDescriptor inspect;
		inspect.name = "inspect";
		inspect.description =
			"Read live game/plugin state. Runs on the main thread and returns the value "
			"synchronously (times out if the game is mid-load / not pumping tasks).";
		inspect.readOnly = true;
		inspect.inputSchema = json{
			{ "type", "object" },
			{ "properties", json{
									 { "kind", json{ { "type", "string" }, { "enum", json::array({ "state" }) }, { "description", "'state' → { plugin, version, vr, playerLoaded }" } } },
								 } },
		};
		a_registry.Register(std::move(inspect), &InspectHandler);

		ToolDescriptor menu;
		menu.name = "menu";
		menu.description =
			"Inspect, answer, or dismiss menus. action='list' returns { openMenus, messageBoxOpen } "
			"tracked live from menu open/close events. 'describe' returns the active MessageBoxMenu "
			"as { messageBoxOpen, bodyText, buttons:[…], cancelIndex } (read the buttons, then pick "
			"one). 'accept' answers a MessageBoxMenu by button 'index' (default 0) — runs its "
			"callback and dismisses it (this is how you clear a Yes/No modal, e.g. the "
			"content-mismatch dialog gating a load; kHide does NOT). 'close' hides a menu by 'name' "
			"via the UI queue (kHide).";
		menu.inputSchema = json{
			{ "type", "object" },
			{ "properties", json{
									 { "action", json{ { "type", "string" }, { "enum", json::array({ "list", "describe", "accept", "close" }) }, { "description", "list | describe | accept | close" } } },
									 { "name", json{ { "type", "string" }, { "description", "menu name to hide (required for close), e.g. MessageBoxMenu" } } },
									 { "index", json{ { "type", "integer" }, { "description", "accept: 0-based button index to select (default 0). See describe's buttons/cancelIndex." } } },
								 } },
		};
		a_registry.Register(std::move(menu), &MenuHandler);

		ToolDescriptor scenario;
		scenario.name = "scenario";
		scenario.description =
			"Run a timed sequence of steps server-side (reproducible tests/benchmarks) and "
			"return a per-step transcript. Each step is one of: "
			"{\"tool\":\"<name>\",\"args\":{…}} dispatch any registered tool (e.g. console, game); "
			"{\"wait\":<ms>} fixed pacing; "
			"{\"waitFor\":<event>,…} block on a Skyrim EVENT — string shorthand "
			"(\"postLoadGame\"/\"saveGame\"/\"newGame\"/\"preLoadGame\"/\"dataLoaded\"/\"deleteGame\", or "
			"\"menuOpened\"/\"menuClosed\" with a \"name\"), or {\"topic\":\"…\",\"match\":{…}}; "
			"{\"waitUntil\":\"playerLoaded\"|\"noModal\"|\"noMenu\"} poll live state. "
			"PREFER waitFor over a fixed wait — e.g. wait for postLoadGame to know a load truly "
			"finished. Optional top-level: repeat (≤1000), continueOnError. waitFor/waitUntil take "
			"timeoutMs + pollMs. Blocks the request for the run's duration (seconds); keep "
			"per-step timeouts sane.";
		scenario.inputSchema = json{
			{ "type", "object" },
			{ "required", json::array({ "steps" }) },
			{ "properties", json{
									 { "steps", json{ { "type", "array" }, { "description", "ordered steps; each is one of tool/wait/waitFor/waitUntil (see description)" }, { "items", json{ { "type", "object" } } } } },
									 { "repeat", json{ { "type", "integer" }, { "description", "run the whole step list N times (default 1, max 1000)" } } },
									 { "continueOnError", json{ { "type", "boolean" }, { "description", "keep going after a failed/timed-out step instead of aborting (default false)" } } },
								 } },
		};
		a_registry.Register(std::move(scenario),
			[&a_registry, &a_events](const json& a_args, const ToolContext& a_ctx) {
				return ScenarioHandler(a_args, a_ctx, a_registry, a_events);
			});
	}
}
