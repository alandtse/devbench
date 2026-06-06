#include "Tools.h"

#include "ConsoleLogCapture.h"
#include "EventBus.h"
#include "GameEvents.h"
#include "GameState.h"
#include "Json.h"
#include "MainThread.h"
#include "MenuExtensions.h"
#include "Papyrus.h"
#include "Recording.h"
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
					json       arr = json::array();
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

			// A `coc <cell>` is a reproducible entry point for a later recording — note the
			// cell so the recording manifest can capture "how to get here". (|0x20 lowercases
			// ASCII letters for the prefix test without a <cctype> dependency.)
			if (command.size() > 4 && (command[0] | 0x20) == 'c' && (command[1] | 0x20) == 'o' &&
				(command[2] | 0x20) == 'c' && command[3] == ' ') {
				std::string cell = command.substr(4);
				if (const auto nb = cell.find_first_not_of(' '); nb != std::string::npos) {
					cell = cell.substr(nb);
					if (const auto sp = cell.find(' '); sp != std::string::npos)
						cell = cell.substr(0, sp);
					if (!cell.empty())
						Recording::NoteCocEntry(cell);
				}
			}

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
			std::string        name;
			fs::file_time_type mtime;
		};
		std::vector<SaveEntry> EnumerateSaves(const fs::path& a_dir)
		{
			std::vector<SaveEntry> out;
			std::error_code        ec;
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
				json           saves = json::array();
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
				const auto     saves = EnumerateSaves(saveDir);
				if (saves.empty())
					throw ToolError(404, std::format("no .ess saves in {}", saveDir.string()));
				const std::string name = saves.front().name;
				Recording::NoteLoadEntry(name);  // reproducible entry point for a later recording
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
					const auto     saves = EnumerateSaves(saveDir);
					const bool     exists = std::any_of(saves.begin(), saves.end(),
						[&](const SaveEntry& s) { return s.name == name; });
					if (!exists)
						throw ToolError(404, std::format("save '{}' not found in {} — use action='list' for valid names", name, saveDir.string()));
					Recording::NoteLoadEntry(name);  // reproducible entry point for a later recording
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
		// = select a button by index (answers + dismisses); 'open' = show a menu by name (kShow);
		// 'close' = hide a menu by name (kHide). open/close are symmetric UI-queue ops; describe/
		// accept use CommonLib's RE'd MessageBoxMenu accessors (GetCurrentMessageBoxData /
		// SelectOption) on the main thread — no detour.
		json MenuHandler(const json& a_args, const ToolContext& a_ctx)
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
				// `registered` = menus a consumer mod exposed via the C-ABI RegisterMenuHandler,
				// invocable with action='invoke' (kept under this one tool, not separate tools).
				return json{
					{ "openMenus", std::move(open) },
					{ "messageBoxOpen", messageBox },
					{ "registered", MenuExtensions::Names() },
				};
			}

			if (action == "invoke") {
				// Dispatch to a consumer-registered menu handler (see C-ABI RegisterMenuHandler).
				// The handler receives the full args object and runs under the same contract as a
				// tool handler; we just route by `name` so mod menus share the one `menu` tool.
				const std::string name = a_args.value("name", std::string{});
				if (name.empty())
					throw ToolError(400, "action 'invoke' requires a 'name' (a registered menu — see menu list .registered)");
				auto entry = MenuExtensions::Find(name);
				if (!entry)
					throw ToolError(404, std::format("no handler registered for menu '{}' (see menu list .registered)", name));
				return entry->handler(a_args, a_ctx);
			}

			if (action == "open") {
				// Show a menu by name via the UI message queue (kShow) — the mirror of 'close'.
				// kShow instantiates the registered menu via its factory if no instance exists
				// yet, so this opens hub menus (TweenMenu, "Journal Menu", MagicMenu, MapMenu,
				// StatsMenu, InventoryMenu, FavoritesMenu) from a plain name. Context menus
				// (ContainerMenu/BarterMenu/BookMenu) need a target ref and won't open this way.
				// Marshalled to the main thread.
				const std::string name = a_args.value("name", std::string{});
				if (name.empty())
					throw ToolError(400, "action 'open' requires a 'name' (menu to show)");
				auto* task = SKSE::GetTaskInterface();
				if (!task)
					throw ToolError(500, "SKSE TaskInterface unavailable");
				task->AddTask([name]() {
					if (auto* q = RE::UIMessageQueue::GetSingleton())
						q->AddMessage(name.c_str(), RE::UI_MESSAGE_TYPE::kShow, nullptr);
				});
				return json{ { "queued", true }, { "action", "open" }, { "name", name } };
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
				// With a `name`, report a consumer-registered menu's descriptor (what `invoke`
				// accepts). Without one, fall back to the active MessageBoxMenu (the original use).
				if (const std::string name = a_args.value("name", std::string{}); !name.empty()) {
					auto entry = MenuExtensions::Find(name);
					if (!entry)
						throw ToolError(404, std::format("no handler registered for menu '{}' (see menu list .registered)", name));
					return json{ { "registered", true }, { "name", name }, { "descriptor", entry->descriptor } };
				}
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
				auto*     task = SKSE::GetTaskInterface();
				if (!task)
					throw ToolError(500, "SKSE TaskInterface unavailable");
				task->AddTask([index]() { RE::MessageBoxMenu::SelectOption(index); });
				return json{ { "queued", true }, { "action", "accept" }, { "index", index } };
			}

			throw ToolError(400, std::format("unknown action '{}' (list|open|close|describe|accept|invoke)", action));
		}

		// Identify any form as { formId, formType, name, editorId } — CommonLib's RE'd accessors.
		json IdentifyForm(const RE::TESForm* a_form)
		{
			if (!a_form)
				return nullptr;
			json j{
				{ "formId", std::format("0x{:08X}", a_form->GetFormID()) },
				{ "formType", std::string(RE::FormTypeToString(a_form->GetFormType())) },
			};
			if (const char* n = a_form->GetName(); n && *n)
				j["name"] = n;
			if (const char* e = a_form->GetFormEditorID(); e && *e)
				j["editorId"] = e;
			return j;
		}

		// Identify a placed reference — the form's identity plus its base object and position.
		// Actors get a live combat snapshot (health, level, hostility) so 'refs formType=Actor'
		// is an actual check on the NPCs in the scene, not just their names.
		json IdentifyRef(RE::TESObjectREFR* a_ref)
		{
			if (!a_ref)
				return nullptr;
			json j = IdentifyForm(a_ref);
			if (auto* base = a_ref->GetBaseObject())
				j["base"] = IdentifyForm(base);
			const auto p = a_ref->GetPosition();
			j["position"] = json::array({ p.x, p.y, p.z });

			if (auto* actor = a_ref->As<RE::Actor>()) {
				json a{ { "level", actor->GetLevel() } };
				if (auto* avo = actor->AsActorValueOwner()) {
					a["health"] = avo->GetActorValue(RE::ActorValue::kHealth);
					a["healthMax"] = avo->GetPermanentActorValue(RE::ActorValue::kHealth);
				}
				if (auto* pc = RE::PlayerCharacter::GetSingleton(); pc && actor != pc)
					a["hostileToPlayer"] = actor->IsHostileToActor(pc);
				a["playerTeammate"] = actor->IsPlayerTeammate();
				j["actor"] = std::move(a);
			}
			return j;
		}

		// inspect: read live game state. The value-returning primitive — each read runs on the
		// main thread and its result is returned synchronously. kinds: state | vm | scene | refs.
		json InspectHandler(const json& a_args, const ToolContext&)
		{
			const std::string kind = a_args.value("kind", std::string("state"));

			if (kind == "state") {
				return MainThread::RunAndWait([]() -> json {
					auto*      pc = RE::PlayerCharacter::GetSingleton();
					const bool loaded = pc && pc->Get3D() != nullptr;
					return json{
						{ "plugin", "devbench" },
						{ "version", DEVBENCH_VERSION_STRING },
						{ "vr", REL::Module::IsVR() },
						{ "playerLoaded", loaded },
						{ "frame", game::CurrentFrame() },
					};
				});
			}

			// vm: Papyrus VM health — how loaded the script engine is (spot script lag).
			if (kind == "vm") {
				return MainThread::RunAndWait([]() -> json {
					auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
					if (!vm)
						return json{ { "available", false } };
					std::size_t types = 0;
					{
						RE::BSSpinLockGuard l(vm->typeInfoLock);
						types = vm->objectTypeMap.size();
					}
					std::size_t running = 0;
					{
						RE::BSSpinLockGuard l(vm->runningStacksLock);
						running = vm->allRunningStacks.size();
					}
					return json{
						{ "available", true },
						{ "loadedTypes", types },
						{ "attachedScripts", vm->scriptCount },
						{ "arrays", vm->arrayCount },
						{ "runningStacks", running },
						{ "frozenStacks", vm->frozenStacksCount },
						{ "overstressed", vm->overstressed },
					};
				});
			}

			// scene: the player's live context — cell/worldspace/location, position, time, weather.
			if (kind == "scene") {
				return MainThread::RunAndWait([]() -> json {
					auto* pc = RE::PlayerCharacter::GetSingleton();
					if (!pc || !pc->Get3D())
						return json{ { "playerLoaded", false } };
					json j{ { "playerLoaded", true } };
					if (auto* cell = pc->GetParentCell())
						j["cell"] = IdentifyForm(cell);
					if (auto* ws = pc->GetWorldspace())
						j["worldspace"] = IdentifyForm(ws);
					if (auto* loc = pc->GetCurrentLocation())
						j["location"] = IdentifyForm(loc);
					const auto p = pc->GetPosition();
					j["position"] = json::array({ p.x, p.y, p.z });
					if (auto* cal = RE::Calendar::GetSingleton()) {
						j["gameHour"] = cal->GetHour();
						j["daysPassed"] = cal->GetDaysPassed();
					}
					if (auto* sky = RE::Sky::GetSingleton(); sky && sky->currentWeather)
						j["weather"] = IdentifyForm(sky->currentWeather);
					return j;
				});
			}

			// refs: consolidated form identification. One of three sources, one identify shape:
			//   'formId'      → that one form (a placed ref gets base + position)
			//   'selected'    → the console-selected / crosshair ref (set via prid/click)
			//   else enumerate the loaded references in the grid (on-screen or not), with optional
			//                   'formType' filter, 'radius' (from player), and 'limit' (default 100).
			if (kind == "refs") {
				const std::string formId = a_args.value("formId", std::string{});
				const bool        selected = a_args.value("selected", false);
				const std::string typeFilter = a_args.value("formType", std::string{});
				const double      radius = a_args.value("radius", 0.0);
				const int         limit = a_args.value("limit", 100);
				if (radius < 0.0)
					throw ToolError(400, "inspect refs: 'radius' must be >= 0 (0 scans the whole loaded grid)");
				if (limit < 0)
					throw ToolError(400, "inspect refs: 'limit' must be >= 0");
				return MainThread::RunAndWait([=]() -> json {
					auto lower = [](std::string s) {
						std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
						return s;
					};

					if (!formId.empty()) {
						// Explicit 0x.. → FormID; otherwise EditorID first (so an all-hex EditorID
						// isn't misread as a FormID), then a bare hex FormID fallback.
						// Whole-string + 32-bit-range hex, so "14G" / overflow don't truncate to a
						// valid FormID and resolve the wrong form.
						auto byHex = [](const std::string& s) -> RE::TESForm* {
							std::size_t        consumed = 0;
							unsigned long long id = 0;
							try {
								id = std::stoull(s, &consumed, 16);
							} catch (...) {
								return nullptr;
							}
							if (consumed != s.size() || id > 0xFFFFFFFFull)
								return nullptr;
							return RE::TESForm::LookupByID(static_cast<RE::FormID>(id));
						};
						RE::TESForm* f = nullptr;
						if (formId.size() > 2 && formId[0] == '0' && (formId[1] == 'x' || formId[1] == 'X'))
							f = byHex(formId.substr(2));
						else if (f = RE::TESForm::LookupByEditorID(formId); !f)
							f = byHex(formId);
						json one = (f && f->As<RE::TESObjectREFR>()) ? IdentifyRef(f->As<RE::TESObjectREFR>()) : IdentifyForm(f);
						return json{ { "count", f ? 1 : 0 }, { "refs", f ? json::array({ one }) : json::array() } };
					}

					if (selected) {
						auto sel = RE::Console::GetSelectedRef();
						return json{
							{ "source", "selected" },
							{ "count", sel ? 1 : 0 },
							{ "refs", sel ? json::array({ IdentifyRef(sel.get()) }) : json::array() },
						};
					}

					auto* tes = RE::TES::GetSingleton();
					if (!tes)
						throw ToolError(503, "TES unavailable (no loaded world?)");
					// The engine's type strings are 4-char codes (ACHR, NPC_, CONT). Map common
					// friendly names onto them so 'Actor'/'container' work, not just 'ACHR'; the
					// match is also a substring, so a raw code or a prefix ('ACH') works too.
					std::string needle = lower(typeFilter);
					{
						static const std::unordered_map<std::string, std::string> kAlias{
							{ "actor", "achr" }, { "npc", "npc_" }, { "container", "cont" },
							{ "door", "door" }, { "weapon", "weap" }, { "armor", "armo" },
							{ "book", "book" }, { "ingredient", "ingr" }, { "potion", "alch" },
							{ "misc", "misc" }, { "light", "ligh" }, { "furniture", "furn" },
							{ "activator", "acti" }, { "flora", "flor" }, { "tree", "tree" },
							{ "static", "stat" }, { "key", "keym" }, { "scroll", "scrl" },
							{ "ammo", "ammo" }, { "soulgem", "slgm" }
						};
						if (auto it = kAlias.find(needle); it != kAlias.end())
							needle = it->second;
					}
					json refs = json::array();
					int  total = 0;
					auto cb = [&](RE::TESObjectREFR* r) {
						if (r && r->GetFormID() != 0) {
							if (!needle.empty()) {
								const std::string t = lower(std::string(RE::FormTypeToString(r->GetFormType())));
								const std::string bt = r->GetBaseObject() ? lower(std::string(RE::FormTypeToString(r->GetBaseObject()->GetFormType()))) : std::string{};
								if (t.find(needle) == std::string::npos && bt.find(needle) == std::string::npos)
									return RE::BSContainer::ForEachResult::kContinue;
							}
							++total;
							if (static_cast<int>(refs.size()) < limit)
								refs.push_back(IdentifyRef(r));
						}
						return RE::BSContainer::ForEachResult::kContinue;
					};
					auto* pc = RE::PlayerCharacter::GetSingleton();
					if (radius > 0.0 && pc)
						tes->ForEachReferenceInRange(pc, static_cast<float>(radius), cb);
					else
						tes->ForEachReference(cb);
					return json{
						{ "count", total },
						{ "returned", static_cast<int>(refs.size()) },
						{ "truncated", total > static_cast<int>(refs.size()) },
						{ "refs", std::move(refs) },
					};
				});
			}

			throw ToolError(400, std::format("unknown kind '{}' (state|vm|scene|refs)", kind));
		}

		// camera: read or set the player camera point of view, so a recording can capture the
		// POV (first/third/vanity) and a replay restore it — what's rendered differs by POV.
		// action='get' returns the live POV synchronously; 'setPov' queues the switch onto the
		// main thread (PlayerCamera state changes must run there). Uses CommonLib's runtime-
		// correct helpers, not the raw CameraState enum (which shifts between SE and VR).
		json CameraHandler(const json& a_args, const ToolContext&)
		{
			const std::string action = a_args.value("action", std::string("get"));

			if (action == "get") {
				return MainThread::RunAndWait([]() -> json {
					auto* cam = RE::PlayerCamera::GetSingleton();
					if (!cam)
						return json{ { "pov", nullptr } };
					std::string pov = "other";
					if (cam->IsInFirstPerson())
						pov = "first";
					else if (cam->IsInThirdPerson())
						pov = "third";
					else if (cam->currentState && cam->currentState->id == RE::CameraState::kAutoVanity)
						pov = "vanity";  // kAutoVanity=1 is identical in SE/VR layouts
					json out{ { "pov", pov }, { "freeCam", cam->IsInFreeCameraMode() } };
					if (cam->cameraRoot) {
						const auto& t = cam->cameraRoot->world.translate;
						out["camX"] = t.x;
						out["camY"] = t.y;
						out["camZ"] = t.z;
						if (RE::NiPoint3 e; cam->cameraRoot->world.rotate.ToEulerAnglesXYZ(e)) {
							out["camPitch"] = e.x;
							out["camYaw"] = e.z;
						}
					}
					return out;
				});
			}
			auto* task = SKSE::GetTaskInterface();
			if (!task)
				throw ToolError(500, "SKSE TaskInterface unavailable");

			// freecam: toggle the free camera. Enter before 'drive' (the state change is deferred a
			// tick, so issue freecam {on:true} + a short wait before driving).
			if (action == "freecam") {
				const bool on = a_args.value("on", true);
				task->AddTask([on]() {
					if (auto* cam = RE::PlayerCamera::GetSingleton(); cam && cam->IsInFreeCameraMode() != on)
						cam->ToggleFreeCameraMode(false);  // false: don't freeze time
				});
				return json{ { "queued", true }, { "action", "freecam" }, { "on", on } };
			}

			// drive: set the free camera's world transform — the exact-viewpoint replay primitive.
			// Must already be in free cam (see freecam). rotation pitch/yaw use the free-cam
			// convention; the recording captures world Euler angles which the recipe maps here.
			if (action == "drive") {
				const float x = a_args.value("x", 0.0f), y = a_args.value("y", 0.0f), z = a_args.value("z", 0.0f);
				const float pitch = a_args.value("pitch", 0.0f), yaw = a_args.value("yaw", 0.0f);
				task->AddTask([x, y, z, pitch, yaw]() {
					auto* cam = RE::PlayerCamera::GetSingleton();
					if (!cam || !cam->currentState || cam->currentState->id != RE::CameraState::kFree)
						return;  // not in free cam — issue camera freecam {on:true} first
					auto* fc = static_cast<RE::FreeCameraState*>(cam->currentState.get());
					fc->translation = RE::NiPoint3{ x, y, z };
					fc->rotation.x = pitch;  // BEST-EFFORT free-cam rotation convention — tune in-game
					fc->rotation.y = yaw;
				});
				return json{ { "queued", true }, { "action", "drive" } };
			}

			if (action != "setPov")
				throw ToolError(400, std::format("unknown action '{}' (get|setPov|freecam|drive)", action));

			const std::string pov = a_args.value("pov", std::string{});
			if (pov != "first" && pov != "third" && pov != "vanity")
				throw ToolError(400, std::format("invalid pov '{}' (first|third|vanity)", pov));
			task->AddTask([pov]() {
				auto* cam = RE::PlayerCamera::GetSingleton();
				if (!cam)
					return;
				if (pov == "first")
					cam->ForceFirstPerson();
				else if (pov == "third")
					cam->ForceThirdPerson();
				else  // vanity has no Force* helper; push the state by its (runtime-mapped) enum
					cam->PushCameraState(RE::CameraState::kAutoVanity);
			});
			return json{ { "queued", true }, { "action", "setPov" }, { "pov", pov } };
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
			json        match;
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

			json       results = json::array();
			const auto t0 = steady_clock::now();
			bool       anyFailure = false;
			bool       aborted = false;

			// Mark replaying for the whole run: if a recording is active (composition — recording
			// a session that plays back a recipe), the pose sampler must not re-capture the
			// teleported path; the issued setpos commands (seen by the console hook) are the
			// trajectory. RAII so the flag clears on any return/throw.
			Recording::SetReplaying(true);
			struct ReplayGuard
			{
				~ReplayGuard() { Recording::SetReplaying(false); }
			} replayGuard;

			for (int rep = 0; rep < repeat && !aborted; ++rep) {
				for (size_t i = 0; i < steps.size() && !aborted; ++i) {
					const json& step = steps[i];
					json        r{ { "index", i } };
					if (repeat > 1)
						r["repeat"] = rep;
					const auto stepStart = steady_clock::now();
					bool       stepFailed = false;

					try {
						if (step.contains("wait")) {
							const long ms = step["wait"].get<long>();
							r["kind"] = "wait";
							r["ms"] = ms;
							std::this_thread::sleep_for(milliseconds(ms));
						} else if (step.contains("waitFor")) {
							const WaitForSpec spec = ParseWaitFor(step);
							const long        timeoutMs = step.value("timeoutMs", static_cast<long>(60000));
							const long        pollMs = step.value("pollMs", static_cast<long>(100));
							r["kind"] = "waitFor";
							r["topic"] = spec.topic;
							r["match"] = spec.match;
							// Only events published after this step begins count.
							uint64_t   since = a_events.HeadSeq();
							bool       satisfied = false;
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
							const long        timeoutMs = step.value("timeoutMs", static_cast<long>(30000));
							const long        pollMs = step.value("pollMs", static_cast<long>(250));
							r["kind"] = "waitUntil";
							r["cond"] = cond;
							bool       satisfied = false;
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
							const json        args = step.value("args", json::object());
							r["kind"] = "tool";
							r["tool"] = tool;
							if (step.contains("label"))
								r["label"] = step["label"];
							ToolContext stepCtx = a_ctx;
							stepCtx.internal = true;  // scenario-driven — don't log each step (replay logs a summary)
							const ToolResult tr = a_registry.Invoke(tool, args, stepCtx);
							r["ok"] = tr.ok;
							if (tr.ok) {
								r["result"] = tr.value;
							} else {
								r["errorCode"] = tr.errorCode;
								r["error"] = tr.errorMessage;
								stepFailed = true;
							}
						} else if (step.contains("assert")) {
							const std::string what = step["assert"].get<std::string>();
							r["kind"] = "assert";
							r["assert"] = what;
							if (what != "scene")
								throw ToolError(400, std::format("unknown assert '{}' (scene)", what));
							const bool          interior = step.value("interior", false);
							const std::uint32_t wsWant = step.value("worldspaceFormID", 0u);
							const std::uint32_t cellWant = step.value("cellFormID", 0u);
							const long          timeoutMs = step.value("timeoutMs", static_cast<long>(10000));
							// The scene may still be loading right after a restore — poll until the
							// player is loaded, read the current worldspace/cell, compare the coarse id.
							json       check;
							bool       ready = false;
							const auto deadline = steady_clock::now() + milliseconds(timeoutMs);
							do {
								try {
									check = MainThread::RunAndWait([interior, wsWant, cellWant]() -> json {
										auto* pc = RE::PlayerCharacter::GetSingleton();
										if (!pc || pc->Get3D() == nullptr)
											return json{ { "ready", false } };
										std::uint32_t curWs = 0, curCell = 0;
										if (auto* ws = pc->GetWorldspace())
											curWs = ws->GetFormID();
										if (auto* cell = pc->GetParentCell())
											curCell = cell->GetFormID();
										const bool ok = interior ? (curCell == cellWant) : (curWs == wsWant);
										return json{ { "ready", true }, { "ok", ok }, { "worldspaceFormID", curWs }, { "cellFormID", curCell } };
									},
										milliseconds(2000));
								} catch (const ToolError&) {
									check = json{ { "ready", false } };  // main thread stalled mid-load — keep polling
								}
								if (check.value("ready", false)) {
									ready = true;
									break;
								}
								std::this_thread::sleep_for(milliseconds(250));
							} while (steady_clock::now() < deadline);

							if (!ready) {
								r["errorCode"] = 504;
								r["error"] = "scene assert: player never finished loading";
								stepFailed = true;
							} else if (!check.value("ok", false)) {
								const std::string wantEid = interior ? step.value("cell", std::string{}) : step.value("worldspace", std::string{});
								r["errorCode"] = 409;
								r["error"] = std::format("scene mismatch: recorded {} '{}' (0x{:X}), currently in 0x{:X} — aborting replay",
									interior ? "cell" : "worldspace", wantEid,
									interior ? cellWant : wsWant,
									interior ? check.value("cellFormID", 0u) : check.value("worldspaceFormID", 0u));
								stepFailed = true;
							} else {
								r["ok"] = true;
								r["worldspaceFormID"] = check.value("worldspaceFormID", 0u);
								r["cellFormID"] = check.value("cellFormID", 0u);
							}
						} else {
							throw ToolError(400, std::format("step {} has none of wait/waitFor/waitUntil/tool/assert", i));
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

		ToolDescriptor camera;
		camera.name = "camera";
		camera.description =
			"Read or set the player camera point of view. action='get' returns { pov } where pov "
			"is first | third | vanity | other, read live on the main thread. action='setPov' "
			"queues a switch (param 'pov': first | third | vanity) onto the main thread "
			"(fire-and-forget). Recordings capture the POV per sample and replay restores it via "
			"this tool, since what is rendered (and benchmarked) differs by POV.";
		camera.inputSchema = json{
			{ "type", "object" },
			{ "properties", json{
								{ "action", json{ { "type", "string" }, { "enum", json::array({ "get", "setPov" }) }, { "description", "get (default) | setPov" } } },
								{ "pov", json{ { "type", "string" }, { "enum", json::array({ "first", "third", "vanity" }) }, { "description", "setPov: target point of view" } } },
							} },
		};
		a_registry.Register(std::move(camera), &CameraHandler);

		ToolDescriptor inspect;
		inspect.name = "inspect";
		inspect.description =
			"Read live game/plugin state. Runs on the main thread and returns the value "
			"synchronously (times out if the game is mid-load / not pumping tasks). kinds: "
			"'state' → { plugin, version, vr, playerLoaded, frame }; 'vm' → Papyrus VM health "
			"{ loadedTypes, attachedScripts, arrays, runningStacks, frozenStacks, overstressed }; "
			"'scene' → player context { cell, worldspace, location, position, gameHour, daysPassed, "
			"weather }; 'refs' → identify reference(s) sharing one shape { formId, formType, name, "
			"editorId, base, position } — pass 'formId' for one form, 'selected'=true for the "
			"console/crosshair ref (set via prid), or neither to enumerate loaded refs in the grid "
			"(optional 'formType' filter, 'radius' from player, 'limit' default 100).";
		inspect.readOnly = true;
		inspect.inputSchema = json{
			{ "type", "object" },
			{ "properties", json{
								{ "kind", json{ { "type", "string" }, { "enum", json::array({ "state", "vm", "scene", "refs" }) }, { "description", "state | vm | scene | refs" } } },
								{ "formId", json{ { "type", "string" }, { "description", "refs: identify this form (hex formId, e.g. 0x14, or EditorID)" } } },
								{ "selected", json{ { "type", "boolean" }, { "description", "refs: identify the console-selected / crosshair ref instead" } } },
								{ "formType", json{ { "type", "string" }, { "description", "refs enumerate: keep only refs whose type or base type matches (e.g. Actor, Container)" } } },
								{ "radius", json{ { "type", "number" }, { "description", "refs enumerate: only refs within this distance of the player (0 = whole loaded grid)" } } },
								{ "limit", json{ { "type", "integer" }, { "description", "refs enumerate: max refs to return (default 100)" } } },
							} },
		};
		a_registry.Register(std::move(inspect), &InspectHandler);

		ToolDescriptor menu;
		menu.name = "menu";
		menu.description =
			"Inspect, open, answer, or dismiss menus. action='list' returns { openMenus, "
			"messageBoxOpen } tracked live from menu open/close events. 'describe' returns the "
			"active MessageBoxMenu as { messageBoxOpen, bodyText, buttons:[…], cancelIndex } (read "
			"the buttons, then pick one). 'accept' answers a MessageBoxMenu by button 'index' "
			"(default 0) — runs its callback and dismisses it (this is how you clear a Yes/No modal, "
			"e.g. the content-mismatch dialog gating a load; kHide does NOT). 'open' shows a menu by "
			"'name' via the UI queue (kShow) — opens hub menus from a plain name (TweenMenu, "
			"'Journal Menu', MagicMenu, MapMenu, StatsMenu, InventoryMenu, FavoritesMenu); context "
			"menus that need a target ref (ContainerMenu/BarterMenu/BookMenu) won't open this way. "
			"'close' hides a menu by 'name' via the UI queue (kHide). 'invoke' dispatches to a "
			"consumer-registered menu handler by 'name' (a mod exposes its menu's interaction via the "
			"C-ABI RegisterMenuHandler instead of adding its own tool); 'list' returns those under "
			"'registered', and 'describe' with a 'name' returns that handler's descriptor.";
		menu.inputSchema = json{
			{ "type", "object" },
			{ "properties", json{
								{ "action", json{ { "type", "string" }, { "enum", json::array({ "list", "describe", "accept", "open", "close", "invoke" }) }, { "description", "list | describe | accept | open | close | invoke" } } },
								{ "name", json{ { "type", "string" }, { "description", "open/close: menu to show/hide (e.g. TweenMenu). invoke: a registered menu (see list .registered). describe: a registered menu to return its descriptor." } } },
								{ "index", json{ { "type", "integer" }, { "description", "accept: 0-based button index to select (default 0). See describe's buttons/cancelIndex." } } },
							} },
		};
		a_registry.Register(std::move(menu), &MenuHandler);

		ToolDescriptor papyrus;
		papyrus.name = "papyrus";
		papyrus.description =
			"Inspect the live Papyrus surface and invoke global functions, returning the value. "
			"action='list' returns loaded script class names { total, returned, truncated, scripts } "
			"(optional 'filter' substring, 'limit' default 200). 'describe' takes a 'script' (class "
			"name) and returns its { globalFunctions, memberFunctions, properties }, each function "
			"with params + returnType — use it to discover what 'call' can invoke. 'call' runs a "
			"function via the VM: 'script' + 'function' (+ optional 'args' array, 'timeoutMs' "
			"default 3000) and returns { called, returned, returnedType }. Unlike console 'cgf', "
			"this hands the return value back (e.g. Utility.GetCurrentGameTime → a Float). Pass "
			"'self' to call a MEMBER function on a target: { \"form\": \"0x14 | EditorID\" } targets "
			"any form, or \"selected\" uses the console/crosshair ref (set via prid); without 'self' "
			"only global/native functions are callable. args and returns support bool/number/string, "
			"{ \"form\": … } (a form return resolves to { formId, formType, editorId, name }), and "
			"arrays of scalars.";
		papyrus.inputSchema = json{
			{ "type", "object" },
			{ "properties", json{
								{ "action", json{ { "type", "string" }, { "enum", json::array({ "list", "describe", "call" }) }, { "description", "list | describe | call" } } },
								{ "script", json{ { "type", "string" }, { "description", "describe/call: the Papyrus script class name, e.g. Utility, Game, Actor" } } },
								{ "function", json{ { "type", "string" }, { "description", "call: the function name, e.g. GetCurrentGameTime or GetActorValue" } } },
								{ "self", json{ { "description", "call: target a member function — { \"form\": \"0x14 | EditorID\" } or \"selected\" (console/crosshair ref). Omit for global/native functions." } } },
								{ "args", json{ { "type", "array" }, { "description", "call: arguments; each a bool/number/string, { \"form\": \"0x14 | EditorID\" }, or an array of scalars" } } },
								{ "filter", json{ { "type", "string" }, { "description", "list: case-insensitive substring to match class names" } } },
								{ "limit", json{ { "type", "integer" }, { "description", "list: max class names to return (default 200)" } } },
								{ "timeoutMs", json{ { "type", "integer" }, { "description", "call: ms to wait for the result before 504 (default 3000)" } } },
							} },
		};
		a_registry.Register(std::move(papyrus), &Papyrus::Handle);

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

		ToolDescriptor record;
		record.name = "record";
		record.description =
			"Capture a manual play-through as a replayable scenario. action='start' begins "
			"sampling the player pose (x/y/z/angleZ/angleX + camera pos + POV + game frame) every "
			"intervalMs (default from config recordIntervalMs, min 10) on a background thread "
			"and captures a one-time scene manifest "
			"(worldspace/cell, time of day, weather, anchor pose, and the entryPoint — the save "
			"loaded or coc'd to reach the scene, or 'unknown'); a game must be loaded. 'stop' "
			"writes the trajectory to Data/SKSE/Plugins/devbench/recordings/recording_<stamp>.json "
			"and returns its path + meta. 'status' reports recording/sampleCount/intervalMs. "
			"'replay' runs a recording file ('path'): with restoreScene=true it re-establishes "
			"the entryPoint (loads the save / coc's the cell) and waits for the player before the "
			"trajectory, so the run reproduces the recorded scene; otherwise it teleports along "
			"the path in the current scene. Emits record.started / record.stopped markers.";
		record.inputSchema = json{
			{ "type", "object" },
			{ "properties", json{
								{ "action", json{ { "type", "string" }, { "enum", json::array({ "start", "stop", "status", "replay" }) }, { "description", "start | stop | status | replay" } } },
								{ "intervalMs", json{ { "type", "integer" }, { "description", "start: pose sample period in ms (default = config recordIntervalMs, min 10)" } } },
								{ "path", json{ { "type", "string" }, { "description", "replay: recording file to play back (from stop's 'path')" } } },
								{ "restoreScene", json{ { "type", "boolean" }, { "description", "replay: re-establish the recorded entryPoint + wait for load before the trajectory (default false)" } } },
							} },
		};
		a_registry.Register(std::move(record),
			[&a_registry, &a_events](const json& a_args, const ToolContext& a_ctx) {
				// replay assembles a step list (optionally prefixed with scene restore) and runs
				// it through the scenario engine, which needs the registry — hence handled here
				// rather than in Recording::Handle.
				if (a_args.value("action", std::string{}) == "replay") {
					const json steps = Recording::BuildReplaySteps(a_args);
					long       estMs = 0;  // sum of wait steps ≈ replay duration
					for (const auto& s : steps)
						if (s.contains("wait"))
							estMs += s["wait"].get<long>();
					Recording::Notify(std::format("devbench: replaying {} steps (~{:.1f}s)", steps.size(), estMs / 1000.0));
					logs::info("devbench: replay starting — {} steps, ~{}ms", steps.size(), estMs);
					const json result = ScenarioHandler(json{ { "steps", steps } }, a_ctx, a_registry, a_events);
					logs::info("devbench: replay finished — {} steps, ok={}",
						result.value("stepsRun", 0), result.value("ok", false));
					return result;
				}
				return Recording::Handle(a_args, a_events);
			});
	}
}
