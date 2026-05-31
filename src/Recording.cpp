#include "Recording.h"

#include "MainThread.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace dvb::Recording
{
	namespace
	{
		using namespace std::chrono;
		namespace fs = std::filesystem;

		constexpr long   kDefaultIntervalMs = 1000;
		constexpr long   kMinIntervalMs = 100;
		constexpr double kRadToDeg = 57.295779513082323;  // 180/pi — console setangle is degrees, data.angle is radians

		// Read the live player pose on the main thread. Null if the player isn't loaded
		// (main menu / mid-load) so the sampler skips the tick rather than logging a bogus
		// sample. MUST run on the main thread (called via MainThread::RunAndWait).
		json ReadPose()
		{
			auto* pc = RE::PlayerCharacter::GetSingleton();
			if (!pc || !pc->Get3D())
				return json(nullptr);
			const auto pos = pc->GetPosition();
			return json{
				{ "x", pos.x },
				{ "y", pos.y },
				{ "z", pos.z },
				{ "angleZ", pc->GetAngleZ() },  // radians
			};
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
				m["anchor"] = json{ { "x", pos.x }, { "y", pos.y }, { "z", pos.z }, { "angleZ", pc->GetAngleZ() } };
			}
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

			json steps = json::array();
			for (const auto& s : a_rec.samples) {
				steps.push_back(consoleStep(std::format("player.setpos x {:.2f}", s.value("x", 0.0))));
				steps.push_back(consoleStep(std::format("player.setpos y {:.2f}", s.value("y", 0.0))));
				steps.push_back(consoleStep(std::format("player.setpos z {:.2f}", s.value("z", 0.0))));
				steps.push_back(consoleStep(std::format("player.setangle z {:.2f}", s.value("angleZ", 0.0) * kRadToDeg)));
				steps.push_back(json{ { "wait", a_rec.intervalMs } });
			}

			json meta = a_rec.manifest;
			meta["format"] = "devbench-recording-1";
			meta["intervalMs"] = a_rec.intervalMs;
			meta["sampleCount"] = a_rec.samples.size();
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
		const std::string action = a_args.value("action", std::string("status"));
		auto&             rec = Get();

		if (action == "start") {
			if (rec.running.load())
				return json{ { "error", "already recording — stop first" } };

			long interval = a_args.value("intervalMs", kDefaultIntervalMs);
			if (interval < kMinIntervalMs)
				interval = kMinIntervalMs;

			// Capture the manifest synchronously: the player must be loaded to anchor the
			// scene, so fail fast (rather than starting an empty recording) if not.
			json manifest;
			try {
				manifest = MainThread::RunAndWait(&ReadManifest, milliseconds(3000));
			} catch (const std::exception& e) {
				return json{ { "error", "could not read scene — is a game loaded?" }, { "detail", e.what() } };
			}
			if (!manifest.contains("anchor"))
				return json{ { "error", "player not loaded — load a game before recording" } };

			{
				std::lock_guard lock(rec.mtx);
				rec.samples.clear();
				rec.manifest = std::move(manifest);
				rec.intervalMs = interval;
			}
			rec.startTick = steady_clock::now();
			rec.running.store(true);
			rec.worker = std::thread([&rec] { rec.Sample(); });

			a_events.Publish("record.started", json{ { "intervalMs", interval } });
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
}
