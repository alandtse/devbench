#include "Capture.h"

#include "Config.h"
#include "EventBus.h"
#include "GameState.h"
#include "MainThread.h"
#include "ToolExtensions.h"

// Decode-only: two already-captured images -> pixel buffers, for the native SSIM comparison
// below. STBI_ONLY_* keeps the codec surface to exactly the formats devbench's own captures can
// actually be (provider captures are PNG per the provider contract; the native fallback can also
// land BMP depending on the user's vanilla screenshot .ini setting) -- not a general-purpose
// image loader.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#include <stb_image.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <optional>
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

		// ---- image comparison (SSIM) ----
		//
		// The ONLY comparison logic devbench itself owns — a fast, single-checkpoint pass/fail
		// verdict inline in the capture response ("replayed -> matched" in one call). This does
		// NOT replace tests/http/visual.py, which stays devbench's own batch/corpus-scoring tool
		// (many recordings, per-checkpoint config files, --visual-update across a whole tree) —
		// that job needs an ecosystem (report generation, golden management across many files)
		// that has no business being native code. This one only answers "did THIS capture match
		// THIS golden," using the same threshold/regions JSON shape visual.py already defined, so
		// a region written for one is copy-pasteable to the other.
		//
		// Decided via a real bounded debate (cross-model, independent judge): the principled line
		// for what belongs in-process is NOT "small code stays in, big code stays out" — it's
		// "provider-shaped external-ecosystem concerns stay out (why D3D11 capture is a provider
		// extension); closed-form deterministic math on data devbench already holds stays in."
		// SSIM has one universal definition and runs on two buffers devbench already has (one
		// just came out of the capture call this same request made) — there's no competing
		// "SSIM provider" ecosystem to be agnostic about the way there is for rendering backends.

		struct GrayImage
		{
			std::vector<float> px;  // row-major, 0..255 range as float (SSIM math needs float)
			int                width = 0;
			int                height = 0;
		};

		std::optional<GrayImage> DecodeGray(const fs::path& a_path)
		{
			int            w = 0, h = 0, channels = 0;
			const auto     pathStr = a_path.string();
			unsigned char* data = stbi_load(pathStr.c_str(), &w, &h, &channels, 1);  // force 1-channel grayscale
			if (!data)
				return std::nullopt;
			GrayImage img;
			img.width = w;
			img.height = h;
			img.px.resize(static_cast<size_t>(w) * h);
			for (size_t i = 0; i < img.px.size(); ++i)
				img.px[i] = static_cast<float>(data[i]);
			stbi_image_free(data);
			return img;
		}

		// Crop a 0..1 UV rect {x,y,w,h} — the SAME convention the capture tool's own `subrect`
		// arg and visual.py's regions both use, so one rect definition works everywhere.
		GrayImage CropUv(const GrayImage& a_img, const json& a_uv)
		{
			const double x0d = a_uv.value("x", 0.0), y0d = a_uv.value("y", 0.0);
			const double wd = a_uv.value("w", 1.0), hd = a_uv.value("h", 1.0);
			const int    x0 = std::clamp(static_cast<int>(std::lround(x0d * a_img.width)), 0, a_img.width);
			const int    y0 = std::clamp(static_cast<int>(std::lround(y0d * a_img.height)), 0, a_img.height);
			const int    x1 = std::clamp(static_cast<int>(std::lround((x0d + wd) * a_img.width)), x0, a_img.width);
			const int    y1 = std::clamp(static_cast<int>(std::lround((y0d + hd) * a_img.height)), y0, a_img.height);

			GrayImage out;
			out.width = x1 - x0;
			out.height = y1 - y0;
			out.px.resize(static_cast<size_t>(out.width) * out.height);
			for (int y = 0; y < out.height; ++y)
				for (int x = 0; x < out.width; ++x)
					out.px[static_cast<size_t>(y) * out.width + x] = a_img.px[static_cast<size_t>(y0 + y) * a_img.width + (x0 + x)];
			return out;
		}

		// Windowed SSIM (Wang et al.), OVERLAPPING windows with a stride shorter than the window
		// — a deliberately simpler approximation than skimage's Gaussian-weighted sliding window
		// (used by visual.py) — uniform weighting, not Gaussian — but critically NOT
		// non-overlapping blocks: a non-overlapping-block version was tried first and FAILED a
		// basic sanity check (a fully value-inverted image scored 0.54, not strongly negative) —
		// any image whose content happens to be flat within each block-sized tile has zero
		// intra-block variance, which makes the contrast/structure term blind to inversion
		// entirely, regardless of correlation sign. A stride shorter than the window guarantees
		// most windows straddle any such boundary, restoring real sensitivity. Verified against
		// synthetic images: identical -> ~1.0, a value-inverted image -> strongly negative,
		// matching the standard algorithm's known behavior (confirmed independently against
		// tests/http/visual.py's skimage-based SSIM on the same class of input). Standard
		// constants (K1=0.01, K2=0.03, L=255 dynamic range).
		double ComputeSsim(const GrayImage& a_a, const GrayImage& a_b)
		{
			constexpr double kC1 = (0.01 * 255.0) * (0.01 * 255.0);
			constexpr double kC2 = (0.03 * 255.0) * (0.03 * 255.0);
			constexpr int    kWindow = 8;
			constexpr int    kStride = 3;  // < kWindow so windows overlap and straddle any block-periodic content

			const int w = a_a.width, h = a_a.height;
			if (w <= 0 || h <= 0)
				return 0.0;
			const int win = std::min({ kWindow, w, h });
			if (win <= 1)
				return 0.0;

			double sum = 0.0;
			int    windows = 0;
			for (int by = 0; by <= h - win; by += kStride) {
				for (int bx = 0; bx <= w - win; bx += kStride) {
					const int n = win * win;

					double meanA = 0.0, meanB = 0.0;
					for (int y = 0; y < win; ++y)
						for (int x = 0; x < win; ++x) {
							const size_t idx = static_cast<size_t>(by + y) * w + (bx + x);
							meanA += a_a.px[idx];
							meanB += a_b.px[idx];
						}
					meanA /= n;
					meanB /= n;

					double varA = 0.0, varB = 0.0, covAB = 0.0;
					for (int y = 0; y < win; ++y)
						for (int x = 0; x < win; ++x) {
							const size_t idx = static_cast<size_t>(by + y) * w + (bx + x);
							const double da = a_a.px[idx] - meanA;
							const double db = a_b.px[idx] - meanB;
							varA += da * da;
							varB += db * db;
							covAB += da * db;
						}
					varA /= (n - 1);
					varB /= (n - 1);
					covAB /= (n - 1);

					const double numerator = (2 * meanA * meanB + kC1) * (2 * covAB + kC2);
					const double denominator = (meanA * meanA + meanB * meanB + kC1) * (varA + varB + kC2);
					sum += denominator > 0.0 ? (numerator / denominator) : 1.0;
					++windows;
				}
			}
			// win > w or win > h (a crop smaller than the window) — fall back to one whole-image window.
			if (windows == 0) {
				double       meanA = 0.0, meanB = 0.0;
				const size_t n = static_cast<size_t>(w) * h;
				for (size_t i = 0; i < n; ++i) {
					meanA += a_a.px[i];
					meanB += a_b.px[i];
				}
				meanA /= n;
				meanB /= n;
				double varA = 0.0, varB = 0.0, covAB = 0.0;
				for (size_t i = 0; i < n; ++i) {
					const double da = a_a.px[i] - meanA;
					const double db = a_b.px[i] - meanB;
					varA += da * da;
					varB += db * db;
					covAB += da * db;
				}
				varA /= (n - 1);
				varB /= (n - 1);
				covAB /= (n - 1);
				const double numerator = (2 * meanA * meanB + kC1) * (2 * covAB + kC2);
				const double denominator = (meanA * meanA + meanB * meanB + kC1) * (varA + varB + kC2);
				return denominator > 0.0 ? (numerator / denominator) : 1.0;
			}
			return sum / windows;
		}

		struct RegionScore
		{
			std::string name;
			double      score = 0.0;
			double      threshold = 0.0;
			bool        passed = false;
		};

		// Everything a `golden` result needs, whether it succeeded or not — a decode failure
		// (missing golden, corrupt file, dimension mismatch) is reported as `error`, not thrown:
		// a checkpoint that can't be scored still has a real capture worth returning, it just
		// can't carry a verdict.
		struct GoldenScore
		{
			bool                     ok = false;
			std::string              error;
			double                   score = 0.0;
			double                   threshold = 0.0;
			bool                     passed = false;
			std::vector<RegionScore> regions;
		};

		GoldenScore ScoreAgainstGolden(const fs::path& a_capturedPath, const json& a_goldenCfg)
		{
			GoldenScore       result;
			const std::string goldenArg = a_goldenCfg.value("golden", std::string{});
			if (goldenArg.empty()) {
				result.error = "goldenCfg missing 'golden'";
				return result;
			}
			fs::path goldenPath(goldenArg);
			if (goldenPath.is_relative())
				goldenPath = GameRoot() / goldenPath;

			result.threshold = a_goldenCfg.value("threshold", 0.98);

			if (!fs::exists(goldenPath)) {
				result.error = std::format("no golden at {}", GenericPath(goldenPath));
				return result;
			}
			const auto candidate = DecodeGray(a_capturedPath);
			const auto golden = DecodeGray(goldenPath);
			if (!candidate || !golden) {
				result.error = "failed to decode candidate or golden image";
				return result;
			}
			if (candidate->width != golden->width || candidate->height != golden->height) {
				result.error = std::format("shape mismatch: candidate {}x{} vs golden {}x{}",
					candidate->width, candidate->height, golden->width, golden->height);
				return result;
			}

			const json regionCfgs = a_goldenCfg.value("regions", json::array());
			if (!regionCfgs.empty()) {
				double worst = 1.0;
				bool   allPassed = true;
				for (const auto& r : regionCfgs) {
					const GrayImage rc = CropUv(*candidate, r);
					const GrayImage rg = CropUv(*golden, r);
					const double    score = ComputeSsim(rc, rg);
					const double    threshold = r.value("threshold", result.threshold);
					const bool      passed = score >= threshold;
					result.regions.push_back(RegionScore{ r.value("name", std::string("?")), score, threshold, passed });
					worst = std::min(worst, score);
					allPassed = allPassed && passed;
				}
				result.ok = true;
				result.score = worst;
				result.passed = allPassed;
				return result;
			}

			result.ok = true;
			result.score = ComputeSsim(*candidate, *golden);
			result.passed = result.score >= result.threshold;
			return result;
		}

		// Merges {ssim, threshold, passed, regions?} into a_result IN PLACE when a_args carries a
		// "golden" config -- a no-op (result unchanged) when it doesn't, so every existing caller
		// that never passes a golden sees no behavior change at all.
		void MaybeScoreAgainstGolden(json& a_result, const json& a_args, const fs::path& a_capturedPath)
		{
			if (!a_args.contains("golden") || !a_args["golden"].is_string() || a_args["golden"].get<std::string>().empty())
				return;
			const GoldenScore score = ScoreAgainstGolden(a_capturedPath, a_args);
			if (!score.ok) {
				a_result["goldenError"] = score.error;
				return;
			}
			a_result["ssim"] = score.score;
			a_result["threshold"] = score.threshold;
			a_result["passed"] = score.passed;
			if (!score.regions.empty()) {
				json regions = json::array();
				for (const auto& r : score.regions)
					regions.push_back(json{ { "name", r.name }, { "ssim", r.score }, { "threshold", r.threshold }, { "passed", r.passed } });
				a_result["regions"] = std::move(regions);
			}
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
			const fs::path  capDir = CaptureDir(a_args);
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
			MaybeScoreAgainstGolden(result, a_args, dest);

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

			const fs::path  capDir = CaptureDir(a_args);
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
			MaybeScoreAgainstGolden(result, a_args, outputPath);

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

	std::filesystem::path CaptureDir(const json& a_args)
	{
		// ALWAYS absolute — this is what the provider contract promises callers ("outputPath —
		// absolute, forward-slash") and what a `capture` result's returned `path` must be too.
		// A relative path here silently resolves against the GAME's cwd on the caller's end,
		// not the caller's own — confirmed live as a real bug (an external tool's cwd rarely
		// matches the Skyrim install dir). `outDir` overrides the configured base; both are
		// resolved against GameRoot() when relative, absolute when already absolute.
		fs::path base = a_args.value("outDir", g_captureDir);
		if (base.is_relative())
			base = GameRoot() / base;
		const std::string recording = a_args.value("recording", std::string("adhoc"));
		const std::string variant = a_args.value("variant", std::string("default"));
		return base / recording / variant;
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
			"kind='auto' (default) picks the sole registered capture provider (a mod that "
			"registered under the C-ABI RegisterToolExtension(\"capture\", <key>, …) — see inspect "
			"kind=registrants); zero registered providers is a 404 UNLESS allowNative=true, and two "
			"or more is a 400 naming them (never silently guessed). kind='native' forces the "
			"low-fidelity vanilla fallback (main-thread MenuControls::QueueScreenshot() + a "
			"directory poll — no path control, no completion signal beyond polling, format fixed by "
			"the user's .ini, may include open UI) — NOT comparable against a provider-authored "
			"golden image. kind='providers' lists registered provider keys; kind='extensions' lists "
			"them with descriptors. Optional 'golden' compares the capture against a reference image "
			"via SSIM and adds {ssim, threshold, passed} to the result (or 'regions' for independent "
			"per-region scores, {name,ssim,threshold,passed} each, overall 'passed' is AND across "
			"all — see record{action:'replay'}'s 'goldens' arg, the normal way this gets set for a "
			"checkpoint). This is devbench's fast single-checkpoint verdict; batch/corpus regression "
			"across many recordings stays in tests/http/visual.py.";
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
								{ "golden", json{ { "type", "string" }, { "description", "path to a reference image to SSIM-compare this capture against (absolute, or relative to the game root); adds {ssim,threshold,passed} to the result" } } },
								{ "threshold", json{ { "type", "number" }, { "description", "golden: SSIM >= threshold passes (default 0.98)" } } },
								{ "regions", json{ { "type", "array" }, { "description", "golden: optional [{name,x,y,w,h,threshold?}] in 0..1 UV — score independent regions instead of the whole frame; overall passed is AND across all" } } },
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
