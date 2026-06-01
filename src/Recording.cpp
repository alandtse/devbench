#include "Recording.h"

#include "GameState.h"
#include "MainThread.h"
#include "ToolRegistry.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dvb::Recording
{
	namespace
	{
		using namespace std::chrono;
		namespace fs = std::filesystem;

		constexpr long   kDefaultIntervalMs = 10;
		constexpr long   kMinIntervalMs = 10;
		constexpr double kRadToDeg = 57.295779513082323;  // 180/pi — console setangle is degrees, data.angle is radians

		// Last entry point devbench brokered into the current scene (a save load or a coc),
		// so a recording can stamp a reproducible "how to get here" into its manifest. Guarded
		// because the game/console tools write it from the listener thread and the recorder
		// reads it at start. Empty kind → entry unknown (player walked here).
		struct EntryPoint
		{
			std::string kind;   // "save" | "coc" | ""
			std::string value;  // save name | cell id
		};
		std::mutex g_entryMtx;
		EntryPoint g_entry;
		int        g_loadSettleMs = 3000;                     // set from config via SetLoadSettleMs
		long       g_defaultIntervalMs = kDefaultIntervalMs;  // set from config via SetDefaultIntervalMs

		// True while devbench is replaying a scenario (teleporting the player). The pose sampler
		// skips these ticks: the replay's own setpos/setangle commands — captured via the console
		// hook — already carry the trajectory, so re-sampling would double it. Lets a user record
		// a session that plays back an existing recipe and embed it cleanly (composition).
		std::atomic<bool> g_replaying{ false };

		// Set when a coc/cow console command is captured mid-recording (the player COMMANDED a cell
		// transition). The cell-load that follows consumes it so NoteCellChange doesn't ALSO emit a
		// coc for the same move — the user's own command already reproduces it. A door issues no
		// console command, so the flag stays clear and NoteCellChange captures that transition.
		std::atomic<bool> g_userCocPending{ false };

		EntryPoint CurrentEntry()
		{
			std::lock_guard lock(g_entryMtx);
			return g_entry;
		}

		// Read the live player pose on the main thread. Null if the player isn't loaded
		// (main menu / mid-load) so the sampler skips the tick rather than logging a bogus
		// sample. MUST run on the main thread (called via MainThread::RunAndWait).
		json ReadPose()
		{
			auto* pc = RE::PlayerCharacter::GetSingleton();
			if (!pc || !pc->Get3D())
				return json(nullptr);
			const auto pos = pc->GetPosition();
			json       s{
					  { "x", pos.x },
					  { "y", pos.y },
					  { "z", pos.z },
					  { "angleZ", pc->GetAngleZ() },  // yaw (radians) — captures rotation-in-place
					  { "angleX", pc->GetAngleX() },  // pitch (radians) — look up/down (sky vs ground)
					  { "frame", game::CurrentFrame() },
			};
			if (auto* cam = RE::PlayerCamera::GetSingleton(); cam) {
				// Point of view, normalized to the three states the camera tool can restore.
				// IsInFirstPerson/IsInThirdPerson are runtime-correct (the raw CameraState enum
				// shifts between SE and VR), so store the string, not the id. Other states
				// (VATS/free/furniture) are left unset — replay won't force a POV it can't drive.
				if (cam->IsInFirstPerson())
					s["pov"] = "first";
				else if (cam->IsInThirdPerson())
					s["pov"] = "third";
				else if (cam->currentState && cam->currentState->id == RE::CameraState::kAutoVanity)
					s["pov"] = "vanity";  // kAutoVanity=1 is identical in SE/VR layouts

				// Camera world transform — what's actually rendered. Differs from the player in 3rd
				// person / VR / free cam. The camera-tool replay drives a free camera along this
				// path for an exact viewpoint (1st/3rd/free). Pitch/yaw are the world Euler angles;
				// the free-cam rotation convention is mapped on the drive side.
				if (cam->cameraRoot) {
					const auto& t = cam->cameraRoot->world.translate;
					s["camX"] = t.x;
					s["camY"] = t.y;
					s["camZ"] = t.z;
					if (RE::NiPoint3 euler; cam->cameraRoot->world.rotate.ToEulerAnglesXYZ(euler)) {
						s["camPitch"] = euler.x;  // world Euler X
						s["camYaw"] = euler.z;    // world Euler Z (about up)
					}
				}
			}
			return s;
		}

		// One-time scene manifest captured at start: the location and lighting state a
		// shader benchmark must reproduce to be comparable (worldspace/cell, time of day,
		// weather), plus the anchor pose and runtime. MUST run on the main thread.
		json ReadManifest()
		{
			json m{ { "vr", REL::Module::IsVR() } };
			if (auto* cal = RE::Calendar::GetSingleton())
				m["gameHour"] = cal->GetHour();
			if (auto* sky = RE::Sky::GetSingleton(); sky && sky->currentWeather) {
				auto* w = sky->currentWeather;
				m["weatherFormID"] = w->GetFormID();
				if (const char* eid = w->GetFormEditorID(); eid && *eid)
					m["weather"] = eid;
			}
			if (auto* pc = RE::PlayerCharacter::GetSingleton()) {
				if (auto* ws = pc->GetWorldspace()) {
					m["worldspaceFormID"] = ws->GetFormID();
					if (const char* eid = ws->GetFormEditorID(); eid && *eid)
						m["worldspace"] = eid;
				}
				if (auto* cell = pc->GetParentCell()) {
					m["cellFormID"] = cell->GetFormID();
					if (const char* eid = cell->GetFormEditorID(); eid && *eid)
						m["cell"] = eid;
					m["interior"] = cell->IsInteriorCell();
				}
				const auto pos = pc->GetPosition();
				m["anchor"] = json{ { "x", pos.x }, { "y", pos.y }, { "z", pos.z }, { "angleZ", pc->GetAngleZ() }, { "frame", game::CurrentFrame() } };
			}

			// Reproducible entry point (save/coc devbench brokered), or a loud "unknown" so
			// a replay won't silently pretend it can restore the scene.
			if (const EntryPoint e = CurrentEntry(); !e.kind.empty())
				m["entryPoint"] = json{ { "kind", e.kind }, { "value", e.value } };
			else
				m["entryPoint"] = json{ { "kind", "unknown" }, { "note", "no save/coc brokered by devbench before recording; replay cannot restore the scene — load from a save and re-record, or set entryPoint manually" } };
			return m;
		}

		// Background pose recorder. One instance (function-local static). start() spawns the
		// sampler; stop() joins and serializes. `samples`/`manifest`/`intervalMs` are guarded
		// by `mtx` (sampler appends, status reads); the thread lifecycle is gated by `running`.
		struct Recorder
		{
			std::atomic<bool>        running{ false };
			std::thread              worker;
			std::mutex               mtx;
			std::vector<json>        samples;
			std::vector<json>        commands;  // console commands seen mid-recording: { command, frame }
			json                     manifest;
			long                     intervalMs = kDefaultIntervalMs;
			steady_clock::time_point startTick;

			void Sample()
			{
				while (running.load(std::memory_order_relaxed)) {
					std::this_thread::sleep_for(milliseconds(intervalMs));
					if (!running.load(std::memory_order_relaxed))
						break;
					json pose;
					try {
						pose = MainThread::RunAndWait(&ReadPose, milliseconds(2000));
					} catch (const std::exception&) {
						continue;  // main thread stalled mid-load — skip this tick
					}
					if (pose.is_null())
						continue;  // player not loaded
					if (g_replaying.load(std::memory_order_relaxed))
						continue;  // devbench is teleporting; the replay's setpos commands (captured
								   // via the console hook) are the trajectory — don't re-sample it
					std::lock_guard lock(mtx);
					samples.push_back(std::move(pose));
				}
			}
		};

		Recorder& Get()
		{
			static Recorder r;
			return r;
		}

		// Build a replayable scenario: teleport the player to each sample (per-axis setpos +
		// setangle in degrees) with a wait of intervalMs between, so the captured path doubles
		// as the measure window. Player-teleport replay needs no new engine hooks; smooth
		// interpolation and a free-camera path are later enhancements.
		json BuildScenario(const Recorder& a_rec, long a_recordedMs)
		{
			const auto consoleStep = [](const std::string& a_cmd) {
				return json{ { "tool", "console" }, { "args", json{ { "action", "exec" }, { "command", a_cmd } } } };
			};
			const auto cameraStep = [](const std::string& a_pov) {
				return json{ { "tool", "camera" }, { "args", json{ { "action", "setPov" }, { "pov", a_pov } } } };
			};

			json                       steps = json::array();
			std::string                lastPov;     // emit a camera step only when the POV changes
			std::array<std::string, 5> lastMove{};  // previous setpos/setangle block
			bool                       haveMove = false;
			size_t                     cmdIdx = 0;  // drain console commands captured up to each sample's frame
			for (const auto& s : a_rec.samples) {
				// Replay console commands the user/agent ran during recording at the point in the
				// trajectory they were issued (ordered by frame), so value-setting is reproduced.
				const auto frame = s.value("frame", 0u);
				for (; cmdIdx < a_rec.commands.size() && a_rec.commands[cmdIdx].value("frame", 0u) <= frame; ++cmdIdx)
					steps.push_back(consoleStep(a_rec.commands[cmdIdx].value("command", std::string{})));

				if (const auto pov = s.value("pov", std::string{}); !pov.empty() && pov != lastPov) {
					steps.push_back(cameraStep(pov));
					lastPov = pov;
				}
				// Collapse: skip the per-axis setpos/setangle block when the rounded pose is
				// unchanged from the last one emitted (standing still at a 10ms interval is a long
				// run of identical samples) — just advance time with the wait.
				const std::array<std::string, 5> move{
					std::format("player.setpos x {:.2f}", s.value("x", 0.0)),
					std::format("player.setpos y {:.2f}", s.value("y", 0.0)),
					std::format("player.setpos z {:.2f}", s.value("z", 0.0)),
					std::format("player.setangle z {:.2f}", s.value("angleZ", 0.0) * kRadToDeg),
					std::format("player.setangle x {:.2f}", s.value("angleX", 0.0) * kRadToDeg),  // pitch
				};
				if (!haveMove || move != lastMove) {
					for (const auto& cmd : move)
						steps.push_back(consoleStep(cmd));
					lastMove = move;
					haveMove = true;
				}
				steps.push_back(json{ { "wait", a_rec.intervalMs } });
			}
			// Trailing commands issued after the final pose sample.
			for (; cmdIdx < a_rec.commands.size(); ++cmdIdx)
				steps.push_back(consoleStep(a_rec.commands[cmdIdx].value("command", std::string{})));

			json meta = a_rec.manifest;
			meta["format"] = "devbench-recording-1";
			meta["intervalMs"] = a_rec.intervalMs;
			meta["sampleCount"] = a_rec.samples.size();
			meta["commandCount"] = a_rec.commands.size();
			meta["recordedMs"] = a_recordedMs;
			return json{ { "meta", std::move(meta) }, { "steps", std::move(steps) } };
		}

		// Data/SKSE/Plugins/devbench/recordings/recording_<epoch>.json
		fs::path WriteScenarioFile(const json& a_scenario)
		{
			const fs::path  dir = "Data/SKSE/Plugins/devbench/recordings";
			std::error_code ec;
			fs::create_directories(dir, ec);
			const auto     stamp = static_cast<long long>(std::time(nullptr));
			const fs::path path = dir / std::format("recording_{}.json", stamp);
			if (std::ofstream out(path, std::ios::trunc); out)
				out << a_scenario.dump(2) << '\n';
			return path;
		}
	}

	json Handle(const json& a_args, EventBus& a_events)
	{
		std::string action = a_args.value("action", std::string("status"));
		auto&       rec = Get();
		if (action == "toggle")  // hotkey-friendly: start if idle, stop if recording
			action = rec.running.load() ? "stop" : "start";

		if (action == "start") {
			if (rec.running.load())
				return json{ { "error", "already recording — stop first" } };

			long interval = a_args.value("intervalMs", g_defaultIntervalMs);  // config default; arg overrides
			if (interval < kMinIntervalMs)
				interval = kMinIntervalMs;

			// Capture the manifest synchronously: the player must be loaded to anchor the
			// scene, so fail fast (rather than starting an empty recording) if not.
			json manifest;
			try {
				manifest = MainThread::RunAndWait(&ReadManifest, milliseconds(3000));
			} catch (const std::exception& e) {
				logs::warn("devbench: record start failed — scene read timed out ({})", e.what());
				Notify("devbench: can't record — load a game first");
				return json{ { "error", "could not read scene — is a game loaded?" }, { "detail", e.what() } };
			}
			if (!manifest.contains("anchor")) {
				logs::warn("devbench: record start failed — player not loaded");
				Notify("devbench: can't record — load a game first");
				return json{ { "error", "player not loaded — load a game before recording" } };
			}
			if (manifest.value("entryPoint", json::object()).value("kind", std::string{}) == "unknown")
				logs::info(
					"devbench: recording with UNKNOWN entry point — replay won't restore the "
					"scene (load via the game tool or `coc` so devbench can capture it)");

			{
				std::lock_guard lock(rec.mtx);
				rec.samples.clear();
				rec.commands.clear();
				rec.manifest = std::move(manifest);
				rec.intervalMs = interval;
			}
			g_userCocPending.store(false, std::memory_order_relaxed);  // don't leak across sessions
			rec.startTick = steady_clock::now();
			rec.running.store(true);
			rec.worker = std::thread([&rec] { rec.Sample(); });

			a_events.Publish("record.started", json{ { "intervalMs", interval } });
			Notify("devbench: recording started");
			logs::info("devbench: recording started (interval {}ms)", interval);
			return json{ { "action", "start" }, { "recording", true }, { "intervalMs", interval } };
		}

		if (action == "stop") {
			if (!rec.running.load())
				return json{ { "error", "not recording" } };
			rec.running.store(false);
			if (rec.worker.joinable())
				rec.worker.join();  // sampler done → samples are stable, no lock needed below

			const long recordedMs = static_cast<long>(
				duration_cast<milliseconds>(steady_clock::now() - rec.startTick).count());
			const json     scenario = BuildScenario(rec, recordedMs);
			const fs::path path = WriteScenarioFile(scenario);

			a_events.Publish("record.stopped", json{ { "sampleCount", rec.samples.size() }, { "path", path.string() } });
			Notify(std::format("devbench: recording stopped — {} samples, {:.1f}s", rec.samples.size(), recordedMs / 1000.0));
			logs::info("devbench: recording stopped — {} samples, {}ms -> {}", rec.samples.size(), recordedMs, path.string());
			return json{
				{ "action", "stop" },
				{ "sampleCount", rec.samples.size() },
				{ "recordedMs", recordedMs },
				{ "path", path.string() },
				{ "meta", scenario["meta"] },
			};
		}

		if (action == "status") {
			std::lock_guard lock(rec.mtx);
			return json{
				{ "recording", rec.running.load() },
				{ "sampleCount", rec.samples.size() },
				{ "intervalMs", rec.intervalMs },
			};
		}

		return json{ { "error", "unknown action (start|stop|status)" }, { "action", action } };
	}

	void Notify(const std::string& a_msg)
	{
		// Corner HUD message; marshal to the main thread (touches UI). The hotkey path runs on
		// a detached thread, so this is the on-screen feedback for an otherwise headless bench.
		if (auto* task = SKSE::GetTaskInterface())
			task->AddTask([a_msg]() { RE::SendHUDMessage::ShowHUDMessage(a_msg.c_str()); });
	}

	void NoteLoadEntry(const std::string& a_saveName)
	{
		std::lock_guard lock(g_entryMtx);
		g_entry = EntryPoint{ "save", a_saveName };
		logs::info("devbench: entry point captured — save '{}'", a_saveName);
	}

	void NoteCocEntry(const std::string& a_cellId)
	{
		std::lock_guard lock(g_entryMtx);
		g_entry = EntryPoint{ "coc", a_cellId };
	}

	void NoteConsoleCommand(const std::string& a_command)
	{
		auto& rec = Get();
		if (!rec.running.load(std::memory_order_relaxed))
			return;  // only capture while a recording is active
		// coc/cow are real user commands — capture them. But flag that the player just commanded a
		// transition, so the cell-load that follows (NoteCellChange) won't ALSO emit a coc for the
		// same move; the user's own command already reproduces it.
		if (a_command.size() >= 4 && a_command[3] == ' ' &&
			(a_command[0] | 0x20) == 'c' && (a_command[1] | 0x20) == 'o' &&
			((a_command[2] | 0x20) == 'c' || (a_command[2] | 0x20) == 'w'))
			g_userCocPending.store(true, std::memory_order_relaxed);
		std::lock_guard lock(rec.mtx);
		rec.commands.push_back(json{ { "command", a_command }, { "frame", game::CurrentFrame() } });
	}

	void NoteCellChange(const std::string& a_command)
	{
		auto& rec = Get();
		if (!rec.running.load(std::memory_order_relaxed) || a_command.empty())
			return;  // only capture while recording; caller passes "" when it can't build a command
		// If the player commanded this transition (a coc/cow was just captured), their own command
		// already reproduces it — consume the flag and skip, so we don't double it. A door issues
		// no console command, so the flag is clear and we capture the transition here.
		if (g_userCocPending.exchange(false, std::memory_order_relaxed))
			return;
		// A mid-recording cell transition with no commanding console input (door / fast-travel).
		// The caller built the reproducible command — `coc <interior>` (unique editor id) or
		// `cow <worldspace> <gx> <gy>` for exteriors (whose editor ids are NOT unique across
		// worldspaces). The trajectory's setpos then refines to the exact spot.
		std::lock_guard lock(rec.mtx);
		rec.commands.push_back(json{ { "command", a_command }, { "frame", game::CurrentFrame() } });
		logs::info("devbench: recorded cell transition — {}", a_command);
	}

	void SetReplaying(bool a_replaying)
	{
		g_replaying.store(a_replaying, std::memory_order_relaxed);
	}

	void SetLoadSettleMs(int a_ms)
	{
		g_loadSettleMs = (a_ms < 0) ? 0 : a_ms;
	}

	void SetDefaultIntervalMs(int a_ms)
	{
		g_defaultIntervalMs = (a_ms < kMinIntervalMs) ? kMinIntervalMs : a_ms;
	}

	json BuildReplaySteps(const json& a_args)
	{
		std::string path = a_args.value("path", std::string{});
		if (path.empty()) {
			// No path → replay the most recent recording (convenient for the replay hotkey
			// and quick API calls).
			const fs::path     dir = "Data/SKSE/Plugins/devbench/recordings";
			std::error_code    ec;
			fs::path           newest;
			fs::file_time_type best{};
			for (const auto& e : fs::directory_iterator(dir, ec)) {
				if (e.path().extension() != ".json")
					continue;
				const auto t = e.last_write_time(ec);
				if (newest.empty() || t > best) {
					best = t;
					newest = e.path();
				}
			}
			if (newest.empty())
				throw ToolError(404, "no 'path' given and no recordings found");
			path = newest.string();
		}
		std::ifstream in(path);
		if (!in)
			throw ToolError(404, std::format("recording not found: {}", path));
		json rec;
		try {
			in >> rec;
		} catch (const std::exception& e) {
			throw ToolError(400, std::format("invalid recording JSON: {}", e.what()));
		}
		if (!rec.contains("steps") || !rec["steps"].is_array())
			throw ToolError(400, "recording has no 'steps' array");

		json steps = json::array();

		// restoreScene: re-establish the recorded entry point so the trajectory runs in the
		// scene it was captured in (loading the save also restores its time/weather). Without
		// it, replay just teleports along the path in whatever scene is currently loaded.
		if (a_args.value("restoreScene", false)) {
			const json        meta = rec.value("meta", json::object());
			const json        entry = meta.value("entryPoint", json::object());
			const std::string kind = entry.value("kind", std::string{});
			const std::string value = entry.value("value", std::string{});
			if (kind == "save" && !value.empty()) {
				// game load (by name) skips the content-mismatch modal. Wait on the
				// postLoadGame lifecycle EVENT, not waitUntil playerLoaded: when reloading the
				// save we're already in, playerLoaded reads true before the reload cycles, so it
				// would short-circuit; postLoadGame fires only when the load actually completes.
				steps.push_back(json{ { "tool", "game" }, { "args", json{ { "action", "load" }, { "name", value } } } });
				steps.push_back(json{ { "waitFor", "postLoadGame" }, { "timeoutMs", 60000 } });
			} else if (kind == "coc" && !value.empty()) {
				steps.push_back(json{ { "tool", "console" }, { "args", json{ { "action", "exec" }, { "command", "coc " + value } } } });
				steps.push_back(json{ { "waitUntil", "playerLoaded" }, { "timeoutMs", 60000 } });
			} else {
				logs::warn("devbench record(replay): restoreScene requested but entryPoint is '{}' — running trajectory without scene restore",
					kind.empty() ? "unknown" : kind);
			}
			// Settle after the load before teleporting (local/per-machine; overridable per call).
			// Only when a load was actually prefixed (steps non-empty) — no load, no settle.
			const long settleMs = a_args.value("settleMs", static_cast<long>(g_loadSettleMs));
			if (!steps.empty() && settleMs > 0)
				steps.push_back(json{ { "wait", settleMs } });
		}

		// Copy the trajectory, injecting a load-settle after any captured cell transition (coc/cow):
		// the destination cell must finish loading before the following setpos teleports the player,
		// or the replay teleports onto a not-yet-valid ref mid-load and CTDs. Done here (not baked
		// into the recording) so existing recipes get the fix too.
		const long txnSettleMs = a_args.value("settleMs", static_cast<long>(g_loadSettleMs));
		for (const auto& s : rec["steps"]) {
			steps.push_back(s);
			if (s.value("tool", std::string{}) == "console") {
				const std::string c = s.value("args", json::object()).value("command", std::string{});
				if (c.size() >= 4 && c[3] == ' ' && (c[0] | 0x20) == 'c' && (c[1] | 0x20) == 'o' &&
					((c[2] | 0x20) == 'c' || (c[2] | 0x20) == 'w')) {
					steps.push_back(json{ { "waitUntil", "playerLoaded" }, { "timeoutMs", 60000 } });
					if (txnSettleMs > 0)
						steps.push_back(json{ { "wait", txnSettleMs } });
				}
			}
		}
		return steps;
	}
}
