#include "Capture.h"

#include "Config.h"
#include "EventBus.h"
#include "GameState.h"
#include "MainThread.h"
#include "ToolExtensions.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <thread>

namespace dvb::Capture
{
	namespace
	{
		namespace fs = std::filesystem;
		using namespace std::chrono;

		EventBus*                g_events = nullptr;
		std::string              g_captureDir = "Data/SKSE/Plugins/devbench/captures";
		long                     g_captureTimeoutMs = 8000;
		long                     g_captureSettleMs = 500;  // not consumed here yet — read by the Part 2 checkpoint macro
		std::vector<std::string> g_captureScanDirs = { "", "Screenshots" };

		std::atomic<std::uint64_t> g_requestSeq{ 0 };

		std::string GenericPath(const fs::path& a_p)
		{
			return a_p.generic_string();  // forward slashes, per the provider contract
		}

		// ---- game root / directory scanning (shared by the native fallback and inspect kind=screenshots) ----

		struct ScannedFile
		{
			fs::path           path;
			std::uintmax_t     size = 0;
			fs::file_time_type mtime{};
		};

		std::vector<ScannedFile> ScanDirs(const std::vector<fs::path>& a_dirs)
		{
			std::vector<ScannedFile>          out;
			static constexpr std::string_view kExts[] = { ".bmp", ".png", ".jpg", ".jpeg", ".dds" };
			for (const auto& dir : a_dirs) {
				std::error_code ec;
				for (const auto& e : fs::directory_iterator(dir, ec)) {
					if (!e.is_regular_file(ec))
						continue;
					const std::string ext = e.path().extension().string();
					std::string       extLower = ext;
					std::transform(extLower.begin(), extLower.end(), extLower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
					if (std::find(std::begin(kExts), std::end(kExts), extLower) == std::end(kExts))
						continue;
					std::error_code sizeEc, timeEc;
					out.push_back({ e.path(), e.file_size(sizeEc), e.last_write_time(timeEc) });
				}
			}
			return out;
		}

		// True only when no other handle is open on the file — the deterministic "is the writer
		// still holding it" probe (a size-stability check alone can false-positive under OS write
		// buffering; a magic-byte check can't prove completeness since the header is written first).
		bool IsWriterDone(const fs::path& a_p)
		{
			HANDLE h = CreateFileW(a_p.c_str(), GENERIC_READ, 0 /*no sharing*/, nullptr,
				OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h == INVALID_HANDLE_VALUE)
				return false;  // ERROR_SHARING_VIOLATION (or similar) → the writer still holds it
			CloseHandle(h);
			return true;
		}

		bool CopyWithRetry(const fs::path& a_from, const fs::path& a_to, std::string& a_err)
		{
			for (int i = 0; i < 5; ++i) {
				std::error_code ec;
				fs::copy_file(a_from, a_to, fs::copy_options::overwrite_existing, ec);
				if (!ec)
					return true;
				a_err = ec.message();
				std::this_thread::sleep_for(milliseconds(50 * (1 << i)));  // 50…800ms
			}
			return false;
		}

		// ---- scene stamp (best-effort correlation metadata; never fails the capture) ----

		json ReadSceneStamp()
		{
			return MainThread::RunAndWait([]() -> json {
				json  j;
				auto* pc = RE::PlayerCharacter::GetSingleton();
				if (pc) {
					if (auto* cell = pc->GetParentCell())
						j["cellFormID"] = cell->GetFormID();
					if (auto* ws = pc->GetWorldspace())
						j["worldspaceFormID"] = ws->GetFormID();
				}
				if (auto* cal = RE::Calendar::GetSingleton())
					j["gameHour"] = cal->GetHour();
				if (auto* sky = RE::Sky::GetSingleton(); sky && sky->currentWeather)
					j["weatherFormID"] = sky->currentWeather->GetFormID();
				return j;
			},
				milliseconds(1000));
		}

		// ---- sidecar + events ----

		void WriteSidecar(const fs::path& a_imagePath, const json& a_result)
		{
			fs::path sidecar = a_imagePath;
			sidecar.replace_extension(".json");
			std::error_code ec;
			fs::create_directories(sidecar.parent_path(), ec);
			std::ofstream out(sidecar, std::ios::trunc);
			if (out)
				out << a_result.dump(2) << '\n';
			else
				logs::warn("devbench capture: could not write sidecar {}", sidecar.string());
		}

		void PublishSaved(const json& a_result)
		{
			if (g_events)
				g_events->Publish("capture.saved", a_result);
		}

		void PublishAbandoned(const std::string& a_requestId, const std::string& a_path,
			const json& a_args, const std::string& a_reason)
		{
			if (!g_events)
				return;
			g_events->Publish("capture.abandoned", json{
													   { "requestId", a_requestId },
													   { "path", a_path },
													   { "checkpointId", a_args.value("checkpointId", std::string{}) },
													   { "runId", a_args.value("runId", json(nullptr)) },
													   { "reason", a_reason },
												   });
		}

		// ---- provider readiness: capture.ready event, or a file-based fallback poll ----

		struct Ready
		{
			bool                ok = false;
			std::string         readyBy;  // "event" | "poll"
			std::string         error;
			std::uintmax_t      bytes = 0;
			std::optional<bool> uiExcluded;
			std::optional<int>  width, height;
		};

		Ready AwaitArtifact(std::uint64_t a_sinceSeq, const std::string& a_requestId,
			const fs::path& a_path, long a_timeoutMs, long a_pollMs)
		{
			Ready      ready;
			uint64_t   since = a_sinceSeq;
			const auto deadline = steady_clock::now() + milliseconds(a_timeoutMs);
			while (steady_clock::now() < deadline) {
				if (g_events) {
					for (const auto& ev : g_events->Since(since)) {
						since = ev.seq;
						if (ev.topic != "capture.ready")
							continue;
						if (ev.payload.value("requestId", std::string{}) != a_requestId)
							continue;
						if (!ev.payload.value("ok", false)) {
							ready.ok = false;
							ready.readyBy = "event";
							ready.error = ev.payload.value("error", std::string("provider reported failure"));
							return ready;
						}
						std::error_code ec;
						ready.ok = true;
						ready.readyBy = "event";
						ready.bytes = fs::file_size(a_path, ec);
						if (ev.payload.contains("uiExcluded"))
							ready.uiExcluded = ev.payload.value("uiExcluded", false);
						if (ev.payload.contains("width"))
							ready.width = ev.payload.value("width", 0);
						if (ev.payload.contains("height"))
							ready.height = ev.payload.value("height", 0);
						return ready;
					}
				}
				// Fallback: the provider never emitted (or the event was evicted from the ring) —
				// resolve from the file itself so a working-but-degraded provider is still visible
				// rather than blocking the full timeout for nothing.
				std::error_code ec;
				if (fs::exists(a_path, ec) && IsWriterDone(a_path)) {
					const auto size1 = fs::file_size(a_path, ec);
					std::this_thread::sleep_for(milliseconds(a_pollMs));
					const auto size2 = fs::file_size(a_path, ec);
					if (size1 > 0 && size1 == size2) {
						ready.ok = true;
						ready.readyBy = "poll";
						ready.bytes = size2;
						return ready;
					}
				}
				std::this_thread::sleep_for(milliseconds(a_pollMs));
			}
			ready.ok = false;
			ready.readyBy = "poll";
			ready.error = "timed out waiting for the capture provider";
			return ready;
		}

		// ---- native fallback ----

		json Native(const json& a_args)
		{
			const auto        start = steady_clock::now();
			const std::string checkpointId = a_args.value("checkpointId", std::string{});
			if (checkpointId.empty())
				throw ToolError(400, "capture requires 'checkpointId'");

			// Preflight: bAllowScreenShot can disable vanilla capture outright.
			const bool allowed = MainThread::RunAndWait([]() -> json {
				if (auto* ini = RE::INISettingCollection::GetSingleton()) {
					if (auto* s = ini->GetSetting("bAllowScreenShot:Display"))
						return json(s->GetBool());
				}
				return json(true);  // absent → assume allowed rather than block on a guess
			},
				milliseconds(1000))
			                         .get<bool>();
			if (!allowed)
				throw ToolError(409, "vanilla screenshots disabled (bAllowScreenShot:Display=0)");

			std::vector<fs::path> dirs;
			for (const auto& rel : g_captureScanDirs)
				dirs.push_back(rel.empty() ? GameRoot() : GameRoot() / rel);

			// Snapshot BEFORE queueing, so the post-queue scan can tell "new" from "pre-existing".
			auto                                         before = ScanDirs(dirs);
			std::unordered_map<std::string, ScannedFile> beforeByPath;
			for (auto& f : before)
				beforeByPath[f.path.string()] = f;

			const bool queued = MainThread::RunAndWait([]() -> json {
				auto* mc = RE::MenuControls::GetSingleton();
				return json(mc && mc->QueueScreenshot());
			},
				milliseconds(1000))
			                        .get<bool>();
			if (!queued)
				throw ToolError(409, "no ScreenshotHandler, or a shot is already queued");

			const long timeoutMs = a_args.value("timeoutMs", g_captureTimeoutMs);
			const long pollMs = a_args.value("pollMs", static_cast<long>(100));
			const auto deadline = steady_clock::now() + milliseconds(timeoutMs);

			fs::path found;
			while (steady_clock::now() < deadline && found.empty()) {
				for (const auto& f : ScanDirs(dirs)) {
					const std::string key = f.path.string();
					const auto        it = beforeByPath.find(key);
					// Three-way candidate rule: new path, OR changed size, OR newer mtime — mtime
					// compared per-file against ITS OWN prior snapshot, never against a wall-clock
					// t0 (the engine writes a fresh auto-incremented filename every time, so the
					// set-difference branch is primary and granularity-independent; the other two
					// are defence in depth).
					const bool isCandidate = it == beforeByPath.end() ||
					                         it->second.size != f.size ||
					                         f.mtime > it->second.mtime;
					if (!isCandidate)
						continue;
					if (f.size == 0 || !IsWriterDone(f.path))
						continue;
					found = f.path;
					break;
				}
				if (found.empty())
					std::this_thread::sleep_for(milliseconds(pollMs));
			}
			if (found.empty()) {
				std::string dirList;
				for (const auto& d : dirs)
					dirList += (dirList.empty() ? "" : ", ") + d.string();
				PublishAbandoned("", "", a_args, "timeout");
				throw ToolError(504, std::format("no new screenshot appeared within {}ms (scanned: {})", timeoutMs, dirList));
			}

			const std::string recording = a_args.value("recording", std::string("adhoc"));
			const std::string variant = a_args.value("variant", std::string("default"));
			std::string       stem = checkpointId;
			if (a_args.contains("repeat"))
				stem += "__r" + std::to_string(a_args.value("repeat", 0));
			const fs::path  capDir = fs::path(g_captureDir) / recording / variant;
			std::error_code ec;
			fs::create_directories(capDir, ec);
			const fs::path dest = capDir / (stem + found.extension().string());

			std::string copyErr;
			bool        cleanupFailed = false;
			if (!CopyWithRetry(found, dest, copyErr))
				throw ToolError(500, std::format("failed to copy captured screenshot: {}", copyErr));
			if (a_args.value("cleanup", false)) {
				std::string     rmErr;
				std::error_code rmEc;
				fs::remove(found, rmEc);
				if (rmEc)
					cleanupFailed = true;
			}

			const auto sceneStamp = ReadSceneStamp();
			json       degraded = json::array({ "nativeFallback", "formatFromIni", "hudNotSuppressed" });
			if (cleanupFailed)
				degraded.push_back("cleanupFailed");
			if (a_args.value("sceneMismatch", false))
				degraded.push_back("sceneMismatch");

			json result{
				{ "ok", true },
				{ "provider", "native" },
				{ "kind", "screenshot" },
				{ "path", GenericPath(dest) },
				{ "file", dest.filename().string() },
				{ "bytes", static_cast<std::uintmax_t>(fs::file_size(dest, ec)) },
				{ "checkpointId", checkpointId },
				{ "recording", recording },
				{ "variant", variant },
				{ "frame", game::CurrentFrame() },
				{ "epoch", static_cast<long long>(std::time(nullptr)) },
				{ "sceneMismatch", a_args.value("sceneMismatch", false) },
				{ "uiExcluded", false },
				{ "readyBy", "poll" },
				{ "elapsedMs", duration_cast<milliseconds>(steady_clock::now() - start).count() },
				{ "degraded", std::move(degraded) },
				{ "sourcePath", GenericPath(found) },
			};
			if (a_args.contains("runId"))
				result["runId"] = a_args["runId"];
			if (a_args.contains("repeat"))
				result["repeat"] = a_args["repeat"];
			for (const char* k : { "atMs", "resolvedAtMs", "resolvedIndex" })
				if (a_args.contains(k))
					result[k] = a_args[k];
			result.update(sceneStamp);

			WriteSidecar(dest, result);
			PublishSaved(result);
			return result;
		}

		// ---- provider dispatch ----

		json DispatchToProvider(const std::string& a_providerKey, const json& a_args, const ToolContext& a_ctx)
		{
			const auto start = steady_clock::now();
			auto       entry = ToolExtensions::Find("capture", a_providerKey);
			if (!entry)
				throw ToolError(404, std::format("no capture provider registered under '{}'", a_providerKey));

			const std::string checkpointId = a_args.value("checkpointId", std::string{});
			if (checkpointId.empty())
				throw ToolError(400, "capture requires 'checkpointId'");
			const std::string recording = a_args.value("recording", std::string("adhoc"));
			const std::string variant = a_args.value("variant", std::string("default"));
			std::string       stem = checkpointId;
			if (a_args.contains("repeat"))
				stem += "__r" + std::to_string(a_args.value("repeat", 0));

			const fs::path  capDir = fs::path(g_captureDir) / recording / variant;
			std::error_code ec;
			fs::create_directories(capDir, ec);
			const fs::path outputPath = capDir / (stem + ".png");

			const std::string requestId = std::format("{}#{}#{}", checkpointId,
				a_args.value("runId", 0), g_requestSeq.fetch_add(1, std::memory_order_relaxed));

			json providerArgs = a_args;
			providerArgs["outputPath"] = GenericPath(outputPath);
			providerArgs["requestId"] = requestId;

			// Snapshot BEFORE invoking the provider — a fast provider could publish its
			// capture.ready event before an after-the-fact snapshot, causing a false timeout.
			const std::uint64_t sinceSeq = g_events ? g_events->HeadSeq() : 0;

			const json queued = entry->handler(providerArgs, a_ctx);
			if (queued.contains("error"))
				throw ToolError(502, queued.value("error", std::string("capture provider failed")));

			const long  timeoutMs = a_args.value("timeoutMs", g_captureTimeoutMs);
			const long  pollMs = a_args.value("pollMs", static_cast<long>(100));
			const Ready ready = AwaitArtifact(sinceSeq, requestId, outputPath, timeoutMs, pollMs);
			if (!ready.ok) {
				PublishAbandoned(requestId, GenericPath(outputPath), a_args, "timeout");
				throw ToolError(504, std::format("capture provider '{}' did not deliver: {}", a_providerKey, ready.error));
			}

			const auto sceneStamp = ReadSceneStamp();
			json       degraded = json::array();
			if (a_args.value("sceneMismatch", false))
				degraded.push_back("sceneMismatch");
			if (ready.uiExcluded.has_value() && !*ready.uiExcluded)
				degraded.push_back("uiIncluded");

			json result{
				{ "ok", true },
				{ "provider", a_providerKey },
				{ "kind", "screenshot" },
				{ "path", GenericPath(outputPath) },
				{ "file", outputPath.filename().string() },
				{ "bytes", ready.bytes },
				{ "checkpointId", checkpointId },
				{ "recording", recording },
				{ "variant", variant },
				{ "frame", game::CurrentFrame() },
				{ "epoch", static_cast<long long>(std::time(nullptr)) },
				{ "sceneMismatch", a_args.value("sceneMismatch", false) },
				{ "uiExcluded", ready.uiExcluded.value_or(false) },
				{ "readyBy", ready.readyBy },
				{ "elapsedMs", duration_cast<milliseconds>(steady_clock::now() - start).count() },
				{ "requestId", requestId },
				{ "degraded", std::move(degraded) },
			};
			if (ready.width)
				result["width"] = *ready.width;
			if (ready.height)
				result["height"] = *ready.height;
			if (a_args.contains("runId"))
				result["runId"] = a_args["runId"];
			if (a_args.contains("repeat"))
				result["repeat"] = a_args["repeat"];
			for (const char* k : { "atMs", "resolvedAtMs", "resolvedIndex" })
				if (a_args.contains(k))
					result[k] = a_args[k];
			result.update(sceneStamp);

			WriteSidecar(outputPath, result);
			PublishSaved(result);
			return result;
		}
	}

	std::filesystem::path GameRoot()
	{
		wchar_t     buffer[MAX_PATH]{};
		const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
		if (length == 0 || length >= MAX_PATH)
			return fs::current_path();
		return fs::path(buffer).parent_path();
	}

	json ListScreenshots(const json& a_args)
	{
		std::vector<fs::path> dirs;
		if (const std::string dirArg = a_args.value("dir", std::string{}); !dirArg.empty()) {
			dirs.push_back(dirArg);
		} else {
			for (const auto& rel : g_captureScanDirs)
				dirs.push_back(rel.empty() ? GameRoot() : GameRoot() / rel);
		}
		auto files = ScanDirs(dirs);
		std::sort(files.begin(), files.end(), [](const ScannedFile& a, const ScannedFile& b) { return a.mtime > b.mtime; });

		const int limit = a_args.value("limit", 50);
		json      shots = json::array();
		for (std::size_t i = 0; i < files.size() && static_cast<int>(i) < limit; ++i) {
			const auto&     f = files[i];
			std::error_code ec;
			shots.push_back(json{
				{ "file", f.path.filename().string() },
				{ "path", GenericPath(f.path) },
				{ "bytes", f.size },
				{ "mtimeEpoch", duration_cast<seconds>(f.mtime.time_since_epoch()).count() },
			});
		}
		json dirList = json::array();
		for (const auto& d : dirs)
			dirList.push_back(GenericPath(d));
		return json{
			{ "dirs", std::move(dirList) },
			{ "count", static_cast<int>(files.size()) },
			{ "returned", static_cast<int>(shots.size()) },
			{ "truncated", static_cast<int>(files.size()) > static_cast<int>(shots.size()) },
			{ "screenshots", std::move(shots) },
		};
	}

	json Handle(const json& a_args, const ToolContext& a_ctx)
	{
		const std::string kind = a_args.value("kind", std::string("auto"));

		if (kind == "native")
			return Native(a_args);

		if (kind == "providers") {
			json keys = json::array();
			for (const auto& k : ToolExtensions::Keys("capture"))
				keys.push_back(k);
			return json{ { "providers", std::move(keys) } };
		}

		if (kind == "extensions") {
			json out = json::array();
			for (const auto& k : ToolExtensions::Keys("capture")) {
				json e{ { "kind", k } };
				if (auto entry = ToolExtensions::Find("capture", k))
					e["descriptor"] = entry->descriptor;
				out.push_back(std::move(e));
			}
			return json{ { "extensions", std::move(out) } };
		}

		if (kind == "auto") {
			const auto keys = ToolExtensions::Keys("capture");
			if (keys.size() == 1)
				return DispatchToProvider(keys.front(), a_args, a_ctx);
			if (keys.empty()) {
				if (a_args.value("allowNative", false))
					return Native(a_args);
				throw ToolError(404,
					"no capture provider registered; install one (see inspect kind=registrants), "
					"or pass kind='native' / allowNative:true for the low-fidelity vanilla fallback "
					"(NOT comparable against provider-authored goldens)");
			}
			std::string names;
			for (const auto& k : keys)
				names += (names.empty() ? "" : ", ") + k;
			throw ToolError(400, std::format("multiple capture providers registered ({}) — pass an explicit kind", names));
		}

		// A named provider.
		return DispatchToProvider(kind, a_args, a_ctx);
	}

	ToolDescriptor BuildCaptureDescriptor()
	{
		ToolDescriptor d;
		d.name = "capture";
		d.description =
			"Capture a frame (screenshot) and return its file path plus correlation metadata. "
			"devbench never scores images — comparison/SSIM/thresholds/pass-fail are entirely an "
			"external concern; this tool only captures and signals readiness. kind='auto' (default) "
			"picks the sole registered capture provider (a mod that registered under the C-ABI "
			"RegisterToolExtension(\"capture\", <key>, …) — see inspect kind=registrants); zero "
			"registered providers is a 404 UNLESS allowNative=true, and two or more is a 400 naming "
			"them (never silently guessed). kind='native' forces the low-fidelity vanilla fallback "
			"(main-thread MenuControls::QueueScreenshot() + a directory poll — no path control, no "
			"completion signal beyond polling, format fixed by the user's .ini, may include open UI) "
			"— NOT comparable against a provider-authored golden image. kind='providers' lists "
			"registered provider keys; kind='extensions' lists them with descriptors.";
		d.readOnly = false;
		json kinds = json::array({ "auto", "native", "providers", "extensions" });
		for (const auto& k : ToolExtensions::Keys("capture"))
			kinds.push_back(k);
		d.inputSchema = json{
			{ "type", "object" },
			{ "properties", json{
								{ "kind", json{ { "type", "string" }, { "enum", kinds }, { "description", "auto (default) | native | providers | extensions | a registered provider key" } } },
								{ "checkpointId", json{ { "type", "string" }, { "description", "REQUIRED — stable file stem and correlation key" } } },
								{ "recording", json{ { "type", "string" }, { "description", "recording file stem, for correlation (default 'adhoc')" } } },
								{ "variant", json{ { "type", "string" }, { "description", "variant under test, for correlation (default 'default')" } } },
								{ "allowNative", json{ { "type", "boolean" }, { "description", "permit kind=auto to fall back to the vanilla path when no provider is registered (default false)" } } },
								{ "excludeUi", json{ { "type", "boolean" }, { "description", "request a pre-UI capture source; native cannot honor this (default true)" } } },
								{ "outDir", json{ { "type", "string" }, { "description", "override the capture bundle directory" } } },
								{ "timeoutMs", json{ { "type", "integer" }, { "description", "how long to wait for the capture to be ready (default 8000)" } } },
								{ "pollMs", json{ { "type", "integer" }, { "description", "poll interval while waiting (default 100)" } } },
								{ "subrect", json{ { "type", "object" }, { "description", "optional {x,y,w,h} in 0..1 UV — provider-only, native ignores it" } } },
								{ "cleanup", json{ { "type", "boolean" }, { "description", "native only: delete the game's source screenshot after copying (default false)" } } },
							} },
			{ "required", json::array({ "checkpointId" }) },
		};
		return d;
	}

	void SetEvents(EventBus* a_events)
	{
		g_events = a_events;
	}

	void SetDefaults(const Config& a_cfg)
	{
		g_captureDir = a_cfg.captureDir;
		g_captureScanDirs = a_cfg.captureScanDirs;
		g_captureTimeoutMs = a_cfg.captureTimeoutMs;
		g_captureSettleMs = a_cfg.captureSettleMs;
	}
}
