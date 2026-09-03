#include "Recording.h"

#include "GameEvents.h"
#include "GameState.h"
#include "MainThread.h"
#include "RecordingActivity.h"
#include "ToolExtensions.h"
#include "ToolRegistry.h"
#include "VRInputState.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
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

		constexpr long        kDefaultIntervalMs = 10;
		constexpr long        kMinIntervalMs = 10;
		constexpr std::size_t kMaximumRetainedActivityEvents = kMaximumVRTrackedFrames;
		constexpr std::size_t kMaximumRetainedPoseSamples = kMaximumVRTrackedFrames;
		constexpr double      kRadToDeg = 57.295779513082323;  // 180/pi — console setangle is degrees, data.angle is radians

		// Last entry point devbench brokered into the current scene (a save load or a coc),
		// so a recording can stamp a reproducible "how to get here" into its manifest. Guarded
		// because the game/console tools write it from the listener thread and the recorder
		// reads it at start. Empty kind → entry unknown (player walked here).
		struct EntryPoint
		{
			std::string              kind;   // "save" | "coc" | ""
			std::string              value;  // save name | cell id
			steady_clock::time_point at{};   // when brokered — for the coupling age (default-constructed = unknown)
		};
		std::mutex g_entryMtx;
		EntryPoint g_entry;
		int        g_loadSettleMs = 3000;                     // set from config via SetLoadSettleMs
		long       g_defaultIntervalMs = kDefaultIntervalMs;  // set from config via SetDefaultIntervalMs

		// Scene-coupling defaults (set from config via SetCoupling). See Recording.h.
		long        g_anchorMs = 10000;
		long        g_cellMs = 60000;
		bool        g_cleanTransition = true;
		std::string g_cleanTransitionCell = "QASmoke";

		// Default settle (ms) inserted before a checkpoint's capture, when the checkpoint doesn't
		// specify its own settleMs. Set from config via SetCaptureDefaults.
		long g_captureSettleMs = 500;

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

		std::string InputDeviceName(RE::INPUT_DEVICE a_device)
		{
			// Raw device codes are intentional: code 3 identifies different devices in VR and flat runtimes.
			const int code = static_cast<int>(a_device);
			switch (code) {
			case 0:
				return "keyboard";
			case 1:
				return "mouse";
			case 2:
				return "gamepad";
			case 3:
				return REL::Module::IsVR() ? "vivePrimary" : "virtualKeyboard";
			case 4:
				return "viveSecondary";
			case 5:
				return "oculusPrimary";
			case 6:
				return "oculusSecondary";
			case 7:
				return "wmrPrimary";
			case 8:
				return "wmrSecondary";
			case 9:
				return "vrVirtualKeyboard";
			default:
				return code < 0 ? "none" : "unknown";
			}
		}

		std::string InputEventTypeName(int a_type)
		{
			switch (a_type) {
			case 0:
				return "button";
			case 1:
				return "mouseMove";
			case 2:
				return "char";
			case 3:
				return "thumbstick";
			case 4:
				return "deviceConnect";
			case 5:
				return "kinect";
			case 6:
				return REL::Module::IsVR() ? "vrTouchpadPosition" : "sixaxis";
			case 7:
				return REL::Module::IsVR() ? "vrTouchpadSwipe" : "motionGesture";
			case 8:
				return "amiibo";
			default:
				return "unknown";
			}
		}

		json SerializeInputEvent(const RE::InputEvent& a_event)
		{
			const int type = static_cast<int>(a_event.GetEventType());
			const int device = static_cast<int>(a_event.GetDevice());
			json      out{
				{ "kind", "input" },
				{ "eventType", InputEventTypeName(type) },
				{ "eventTypeCode", type },
				{ "device", InputDeviceName(a_event.GetDevice()) },
				{ "deviceCode", device },
			};
			if (const auto* id = a_event.AsIDEvent()) {
				out["idCode"] = id->GetIDCode();
				if (const char* user = id->userEvent.c_str(); user && *user)
					out["userEvent"] = user;
			}

			if (const auto* button = a_event.AsButtonEvent()) {
				out["value"] = button->Value();
				out["heldSeconds"] = button->HeldDuration();
				out["state"] = button->IsDown() ? "down" : button->IsHeld() ? "held" :
				                                       button->IsUp()       ? "up" :
				                                                              "changed";
				if (REL::Module::IsVR())
					if (const auto* wand = button->AsVRWandEvent())
						out["wandIndex"] = wand->unkVR28;
			} else if (const auto* mouse = a_event.AsMouseMoveEvent()) {
				out["x"] = mouse->mouseInputX;
				out["y"] = mouse->mouseInputY;
			} else if (const auto* character = a_event.AsCharEvent()) {
				out["keyCode"] = character->keyCode;
			} else if (const auto* stick = a_event.AsThumbstickEvent()) {
				out["x"] = stick->xValue;
				out["y"] = stick->yValue;
			} else if (type == 4) {
				out["connected"] = static_cast<const RE::DeviceConnectEvent&>(a_event).connected;
			} else if (REL::Module::IsVR() && type == 6) {
				const auto& touch = static_cast<const RE::VrWandTouchpadPositionEvent&>(a_event);
				out["wandIndex"] = touch.unkVR28;
				out["raw"] = json::array({ touch.unk30, touch.unk38, touch.unk40 });
			} else if (REL::Module::IsVR() && type == 7) {
				const auto& swipe = static_cast<const RE::VrWandTouchpadSwipeEvent&>(a_event);
				out["wandIndex"] = swipe.unkVR28;
				out["raw"] = json::array({ swipe.unk30, swipe.unk38 });
			}
			return out;
		}

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
			if (REL::Module::IsVR()) {
				const auto transform = [](const RE::NiAVObject* a_node) -> json {
					if (!a_node)
						return nullptr;
					const auto& t = a_node->world;
					return json::array({
						t.translate.x,
						t.translate.y,
						t.translate.z,
						t.rotate.entry[0][0],
						t.rotate.entry[0][1],
						t.rotate.entry[0][2],
						t.rotate.entry[1][0],
						t.rotate.entry[1][1],
						t.rotate.entry[1][2],
						t.rotate.entry[2][0],
						t.rotate.entry[2][1],
						t.rotate.entry[2][2],
						t.scale,
					});
				};
				if (const auto* nodes = pc->GetVRNodeData()) {
					s["hmdTransform"] = transform(nodes->HmdNode.get());
					s["leftWandTransform"] = transform(nodes->LeftWandNode.get());
					s["rightWandTransform"] = transform(nodes->RightWandNode.get());
				}
			}
			return s;
		}

		// Raw OpenVR tracking-space poses remain available at the main menu, before Skyrim has a
		// PlayerCharacter/3D. They are essential for reconstructing which UI element a wand was
		// aimed at. GetLastPoses is non-blocking and works with both SteamVR and OpenComposite;
		// do not use WaitGetPoses from this sampler because Skyrim owns frame pacing.
		json ReadVRTracking()
		{
			if (!REL::Module::IsVR())
				return nullptr;
			auto* openvr = RE::BSOpenVR::GetSingleton();
			auto* system = openvr ? openvr->vrSystem : nullptr;
			auto* compositor = RE::BSOpenVR::GetIVRCompositor();
			if (!compositor && openvr)
				compositor = openvr->vrContext.vrCompositor;
			if (!system || !compositor)
				return nullptr;
			const auto  trackingOrigin = compositor->GetTrackingSpace();
			const char* trackingOriginName = trackingOrigin == vr::TrackingUniverseSeated             ? "seated" :
			                                 trackingOrigin == vr::TrackingUniverseStanding           ? "standing" :
			                                 trackingOrigin == vr::TrackingUniverseRawAndUncalibrated ? "rawAndUncalibrated" :
			                                                                                            "unknown";

			std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount> poses{};
			if (compositor->GetLastPoses(poses.data(), static_cast<std::uint32_t>(poses.size()),
					nullptr, 0) != vr::VRCompositorError_None)
				return nullptr;

			const auto encode = [&](vr::TrackedDeviceIndex_t a_index, const char* a_role) -> json {
				if (a_index == vr::k_unTrackedDeviceIndexInvalid || a_index >= poses.size())
					return json{ { "role", a_role }, { "available", false } };
				const auto& p = poses[a_index];
				json        out{
					{ "role", a_role },
					{ "available", true },
					{ "index", a_index },
					{ "connected", p.bDeviceIsConnected },
					{ "valid", p.bPoseIsValid },
					{ "trackingResult", static_cast<int>(p.eTrackingResult) },
					{ "velocity", json::array({ p.vVelocity.v[0], p.vVelocity.v[1], p.vVelocity.v[2] }) },
					{ "angularVelocity", json::array({ p.vAngularVelocity.v[0], p.vAngularVelocity.v[1], p.vAngularVelocity.v[2] }) },
				};
				if (p.bPoseIsValid) {
					json matrix = json::array();
					for (int row = 0; row < 3; ++row)
						for (int col = 0; col < 4; ++col)
							matrix.push_back(p.mDeviceToAbsoluteTracking.m[row][col]);
					out["matrix"] = std::move(matrix);
				}
				out["deviceClass"] = static_cast<int>(system->GetTrackedDeviceClass(a_index));
				if (std::string_view(a_role) != "hmd") {
					vr::VRControllerState_t state{};
					if (system->GetControllerState(a_index, &state, sizeof(state))) {
						json axes = json::array();
						for (std::uint32_t axis = 0; axis < vr::k_unControllerStateAxisCount; ++axis) {
							axes.push_back(json::array({ state.rAxis[axis].x, state.rAxis[axis].y }));
						}
						out["controller"] = json{ { "packetNumber", state.unPacketNum },
							{ "pressed", state.ulButtonPressed }, { "touched", state.ulButtonTouched },
							{ "axes", std::move(axes) } };
					}
				}
				return out;
			};

			return json{
				{ "origin", trackingOriginName },
				{ "originCode", static_cast<int>(trackingOrigin) },
				{ "hmd", encode(vr::k_unTrackedDeviceIndex_Hmd, "hmd") },
				{ "left", encode(system->GetTrackedDeviceIndexForControllerRole(
									 vr::TrackedControllerRole_LeftHand),
							  "left") },
				{ "right", encode(system->GetTrackedDeviceIndexForControllerRole(
									  vr::TrackedControllerRole_RightHand),
							   "right") },
			};
		}

		json ReadFrameSample()
		{
			return json{ { "frame", game::CurrentFrame() }, { "pose", ReadPose() },
				{ "tracking", ReadVRTracking() } };
		}

		// One-time scene manifest captured at start: the location and lighting state a
		// shader benchmark must reproduce to be comparable (worldspace/cell, time of day,
		// weather), plus the anchor pose and runtime. MUST run on the main thread.
		json ReadManifest()
		{
			const bool isVR = REL::Module::IsVR();
			json       m{ { "vr", isVR } };
			// runtime.compat: a flat setpos/setangle path replays on SE+AE, but VR drives pitch and
			// culling from the HMD, so a VR recording is its own bucket. Replay gates on this.
			m["runtime"] = json{ { "recordedOnVR", isVR },
				{ "compat", isVR ? json::array({ "vr" }) : json::array({ "se", "ae" }) } };
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
			if (const EntryPoint e = CurrentEntry(); !e.kind.empty()) {
				json ep{ { "kind", e.kind }, { "value", e.value } };
				// Age of the entry at record-start: small => the save/coc was staged for this
				// recording (couple it tightly); large => incidental. Drives the replay tier.
				if (e.at.time_since_epoch().count() != 0)
					ep["ageMs"] = duration_cast<milliseconds>(steady_clock::now() - e.at).count();
				m["entryPoint"] = std::move(ep);
			} else
				m["entryPoint"] = json{ { "kind", "unknown" }, { "note", "no save/coc brokered by devbench before recording; replay cannot restore the scene — load from a save and re-record, or set entryPoint manually" } };
			return m;
		}

		// Background activity recorder. One instance (function-local static). start() spawns the
		// sampler; stop() joins and serializes. Sample streams/manifest/intervalMs are guarded
		// by `mtx` (sampler appends, status reads); the thread lifecycle is gated by `running`.
		enum class RecorderState
		{
			idle,
			starting,
			running,
			stopping,
		};

		const char* RecorderStateName(RecorderState a_state)
		{
			switch (a_state) {
			case RecorderState::idle:
				return "idle";
			case RecorderState::starting:
				return "starting";
			case RecorderState::running:
				return "running";
			case RecorderState::stopping:
				return "stopping";
			}
			return "unknown";
		}

		struct Recorder
		{
			std::atomic<bool>        running{ false };
			std::thread              worker;
			std::mutex               mtx;
			std::condition_variable  cv;
			RecorderState            state = RecorderState::idle;
			std::uint64_t            generation = 0;
			std::vector<json>        samples;
			std::vector<json>        commands;         // console commands seen mid-recording: { command, frame }
			std::vector<json>        checkpoints;      // screenshot checkpoints marked mid-recording: { id, atMs, excludeUi }
			std::vector<json>        activityEvents;   // input/menu/lifecycle/cell on the same monotonic clock
			std::vector<json>        trackingSamples;  // raw OpenVR tracking space; available before player load
			std::uint64_t            nextActivitySeq = 1;
			bool                     limitReached = false;
			std::string              limitReason;
			json                     manifest;
			long                     intervalMs = kDefaultIntervalMs;
			steady_clock::time_point startTick;

			void Sample(std::uint64_t a_generation)
			{
				steady_clock::time_point started;
				long                     interval = kDefaultIntervalMs;
				{
					std::lock_guard lock(mtx);
					started = startTick;
					interval = intervalMs;
				}
				const auto deadline = started + milliseconds(kMaximumVRTrackedDurationMs);
				const auto waitFor = [&](milliseconds a_delay) {
					std::unique_lock lock(mtx);
					const auto       target = std::min(steady_clock::now() + a_delay, deadline);
					if (cv.wait_until(lock, target, [&] {
							return !running.load(std::memory_order_relaxed) ||
						           state != RecorderState::running || generation != a_generation;
						}))
						return false;
					if (steady_clock::now() < deadline)
						return true;
					if (state == RecorderState::running && generation == a_generation) {
						limitReached = true;
						limitReason = "maximum replayable duration reached";
						running.store(false, std::memory_order_relaxed);
					}
					return false;
				};
				for (;;) {
					if (!waitFor(milliseconds(interval)))
						break;
					json frameSample;
					try {
						// Pass &running so a stop() aborts the in-flight wait within one slice
						// instead of blocking join() for the full 2s during a load screen.
						frameSample = MainThread::RunAndWait(&ReadFrameSample, milliseconds(2000), &running);
					} catch (const std::exception&) {
						continue;  // main thread stalled mid-load — skip this tick
					}
					json pose = frameSample.value("pose", json(nullptr));
					json tracking = frameSample.value("tracking", json(nullptr));
					if (pose.is_null() && tracking.is_null()) {
						// Main-menu/new-game capture needs the input sink, not 100 empty pose
						// probes per second. Keep detection responsive without adding startup load.
						if (interval < 100 && !waitFor(milliseconds(100 - interval)))
							break;
						continue;  // player not loaded (or the wait was aborted by stop)
					}
					const auto tMs = duration_cast<milliseconds>(steady_clock::now() - started).count();
					if (tMs > kMaximumVRTrackedDurationMs) {
						std::lock_guard lock(mtx);
						limitReached = true;
						limitReason = "maximum replayable duration reached";
						running.store(false, std::memory_order_relaxed);
						break;
					}
					if (!tracking.is_null()) {
						tracking["tMs"] = tMs;
						tracking["frame"] = frameSample.value("frame", 0u);
						std::lock_guard lock(mtx);
						if (state == RecorderState::running && generation == a_generation) {
							if (trackingSamples.size() + activityEvents.size() >= kMaximumVRTrackedFrames) {
								limitReached = true;
								limitReason = "maximum replayable tracking/activity frame budget reached";
								running.store(false, std::memory_order_relaxed);
							} else {
								trackingSamples.push_back(std::move(tracking));
							}
						}
					}
					if (!running.load(std::memory_order_relaxed))
						break;
					if (pose.is_null() || g_replaying.load(std::memory_order_relaxed))
						continue;  // raw tracking is still sampled, but replay's teleported player pose is not
					// A main-menu-start recording has no anchor. Preserve its initial state and
					// also capture the first loaded scene without pretending that was the start.
					bool needFirstScene = false;
					{
						std::lock_guard lock(mtx);
						needFirstScene = !manifest.contains("anchor") && !manifest.contains("firstPlayerScene");
					}
					if (needFirstScene) {
						try {
							json first = MainThread::RunAndWait(&ReadManifest, milliseconds(2000), &running);
							if (first.contains("anchor")) {
								std::lock_guard lock(mtx);
								if (!manifest.contains("firstPlayerScene"))
									manifest["firstPlayerScene"] = std::move(first);
							}
						} catch (const std::exception&) {
						}
					}
					// Wall-clock offset so BuildScenario can use real inter-sample deltas as
					// wait values — RunAndWait latency inflates actual intervals above intervalMs.
					pose["tMs"] = tMs;
					std::lock_guard lock(mtx);
					if (state == RecorderState::running && generation == a_generation) {
						if (samples.size() >= kMaximumRetainedPoseSamples) {
							limitReached = true;
							limitReason = "maximum retained trajectory sample budget reached";
							running.store(false, std::memory_order_relaxed);
						} else {
							samples.push_back(std::move(pose));
						}
					}
				}
			}
		};

		Recorder& Get()
		{
			static Recorder r;
			return r;
		}

		std::uint64_t ActiveGeneration()
		{
			auto&           rec = Get();
			std::lock_guard lock(rec.mtx);
			if (!rec.running.load(std::memory_order_relaxed) ||
				rec.state != RecorderState::running)
				return 0;
			return rec.generation;
		}

		void AppendActivity(json a_event, std::uint64_t a_generation)
		{
			auto&           rec = Get();
			std::lock_guard lock(rec.mtx);
			if (!a_generation || !rec.running.load(std::memory_order_relaxed) ||
				rec.state != RecorderState::running || rec.generation != a_generation)
				return;
			a_event["tMs"] = duration_cast<milliseconds>(steady_clock::now() - rec.startTick).count();
			a_event["frame"] = game::CurrentFrame();
			if (rec.activityEvents.size() >= kMaximumRetainedActivityEvents ||
				rec.trackingSamples.size() + rec.activityEvents.size() >= kMaximumVRTrackedFrames) {
				rec.limitReached = true;
				rec.limitReason = "maximum replayable activity/tracking frame budget reached";
				rec.running.store(false, std::memory_order_relaxed);
				return;
			}
			a_event["seq"] = rec.nextActivitySeq++;
			rec.activityEvents.push_back(std::move(a_event));
		}

		// Build a replayable scenario: teleport the player to each sample (per-axis setpos +
		// setangle in degrees) with a wait of intervalMs between, so the captured path doubles
		// as the measure window. Player-teleport replay needs no new engine hooks; smooth
		// interpolation and a free-camera path are later enhancements.
		json BuildScenario(const Recorder& a_rec, long a_recordedMs)
		{
			const auto consoleStep = [](const std::string& a_cmd, long a_atMs) {
				json step{ { "tool", "console" }, { "args", json{ { "action", "exec" }, { "command", a_cmd } } } };
				if (a_atMs >= 0)
					step["atMs"] = a_atMs;
				return step;
			};
			const auto cameraStep = [](const std::string& a_pov, long a_atMs) {
				json step{ { "tool", "camera" }, { "args", json{ { "action", "setPov" }, { "pov", a_pov } } } };
				if (a_atMs >= 0)
					step["atMs"] = a_atMs;
				return step;
			};

			json                  steps = json::array();
			std::string           lastPov;     // emit a camera step only when the POV changes
			std::array<double, 5> lastPose{};  // previous emitted pose (round-2); a repeat → bare wait
			bool                  havePose = false;
			size_t                cmdIdx = 0;    // drain console commands captured up to each sample's frame
			long                  prevTMs = -1;  // previous sample's wall-clock offset for delta waits
			for (const auto& s : a_rec.samples) {
				// Replay console commands the user/agent ran during recording at the point in the
				// trajectory they were issued (ordered by frame), so value-setting is reproduced.
				const auto frame = s.value("frame", 0u);
				const long tMs = s.value("tMs", static_cast<long>(-1));
				for (; cmdIdx < a_rec.commands.size() && a_rec.commands[cmdIdx].value("frame", 0u) <= frame; ++cmdIdx)
					steps.push_back(consoleStep(a_rec.commands[cmdIdx].value("command", std::string{}),
						a_rec.commands[cmdIdx].value("tMs", static_cast<long>(-1))));

				if (const auto pov = s.value("pov", std::string{}); !pov.empty() && pov != lastPov) {
					steps.push_back(cameraStep(pov, tMs));
					lastPov = pov;
				}
				// One compact pose row per changed sample; a bare wait for a run of identical
				// (standing-still) samples. Store the 2-decimal values the setpos/setangle replay
				// uses, so the row carries no precision the replay would drop anyway.
				const auto                  r2 = [](double v) { return std::round(v * 100.0) / 100.0; };
				const std::array<double, 5> pose{
					r2(s.value("x", 0.0)),
					r2(s.value("y", 0.0)),
					r2(s.value("z", 0.0)),
					r2(s.value("angleZ", 0.0) * kRadToDeg),
					r2(s.value("angleX", 0.0) * kRadToDeg),  // pitch
				};
				const long waitMs = (tMs > 0 && prevTMs >= 0) ? std::max(1L, tMs - prevTMs) : a_rec.intervalMs;
				json       row{ { "wait", waitMs } };
				if (tMs >= 0)
					row["atMs"] = tMs;
				if (!havePose || pose != lastPose) {
					row["pose"] = pose;
					lastPose = pose;
					havePose = true;
				}
				steps.push_back(std::move(row));
				prevTMs = tMs;
			}
			// Trailing commands issued after the final pose sample.
			for (; cmdIdx < a_rec.commands.size(); ++cmdIdx)
				steps.push_back(consoleStep(a_rec.commands[cmdIdx].value("command", std::string{}),
					a_rec.commands[cmdIdx].value("tMs", static_cast<long>(-1))));

			json meta = a_rec.manifest;
			meta["format"] = "devbench-recording-3";
			meta["intervalMs"] = a_rec.intervalMs;
			meta["sampleCount"] = a_rec.samples.size();
			meta["commandCount"] = a_rec.commands.size();
			meta["activityCapture"] = ActivityCaptureContract();
			meta["trackingCapture"] = json{
				{ "name", "devbench.recording.openvrTracking" },
				{ "version", json{ { "major", 2 }, { "minor", 0 } } },
				{ "source", "IVRCompositor.GetLastPoses" },
				{ "origin", "captured per sample from IVRCompositor.GetTrackingSpace" },
				{ "devices", json::array({ "hmd", "left", "right" }) },
				{ "transformEncoding", "OpenVR device-to-absolute 3x4 row-major" },
				{ "velocityEncoding", "tracking-space metres/second" },
				{ "angularVelocityEncoding", "tracking-space radians/second" },
				{ "controllerEncoding", "OpenVR packetNumber/pressed/touched/five axes" },
				{ "identity", json::array({ "device index", "class", "role" }) },
				{ "availableBeforePlayerLoad", true },
				{ "replay", true },
				{ "replayDevice", "input.vrTrackedSet" },
			};
			meta["poseCapture"] = json{
				{ "name", "devbench.recording.pose" },
				{ "version", json{ { "major", 2 }, { "minor", 0 } } },
				{ "player", json::array({ "position", "yaw", "pitch" }) },
				{ "camera", json::array({ "worldPosition", "worldPitch", "worldYaw", "pov" }) },
				{ "vrTrackedNodes", json::array({ "hmd", "leftWand", "rightWand" }) },
				{ "transformEncoding", "[tx,ty,tz,r00,r01,r02,r10,r11,r12,r20,r21,r22,scale]" },
				{ "vrTransformReplay", false },
			};
			meta["activityCounts"] = SummarizeActivity(a_rec.activityEvents);
			meta["trackingSampleCount"] = a_rec.trackingSamples.size();
			meta["recordedMs"] = a_recordedMs;
			meta["recordedAt"] = static_cast<long long>(std::time(nullptr));  // record-time epoch, for tooling
			// Checkpoints marked live via record{action:"checkpoint"} during this session. Each
			// entry's atMs is already the recorder's own elapsed-ms clock (steady_clock since
			// startTick) -- the SAME clock BuildReplaySteps reconstructs by summing this scenario's
			// own "wait" values, so no reconciliation is needed here; the values just carry over.
			if (!a_rec.checkpoints.empty())
				meta["checkpoints"] = a_rec.checkpoints;
			return json{ { "meta", std::move(meta) },
				{ "activityEvents", a_rec.activityEvents },
				{ "trackingSamples", a_rec.trackingSamples }, { "steps", std::move(steps) } };
		}

		// Data/SKSE/Plugins/devbench/recordings/recording_<epoch>.json
		// One compact step per line (meta pretty-printed): a pose-row recording stays
		// hand-editable and git-diffable one sample at a time, instead of dump(2) exploding
		// every pose array across ~9 indented lines.
		std::string SerializeRecording(const json& a_scenario)
		{
			std::string s = "{\n\"meta\": " + a_scenario.value("meta", json::object()).dump(2);
			const auto  isCompactArray = [](std::string_view a_key, const json& a_value) {
				return a_value.is_array() &&
				       (a_key == "steps" || a_key == "activityEvents" || a_key == "trackingSamples");
			};
			// Preserve caller-added or wrong-typed top-level values verbatim. Only arrays with a
			// known high-volume shape receive compact one-item-per-line formatting.
			for (auto it = a_scenario.begin(); it != a_scenario.end(); ++it)
				if (it.key() != "meta" && !isCompactArray(it.key(), *it))
					s += ",\n" + json(it.key()).dump() + ": " + it->dump(2);
			if (a_scenario.contains("activityEvents") && a_scenario["activityEvents"].is_array()) {
				s += ",\n\"activityEvents\": [\n";
				const json& events = a_scenario["activityEvents"];
				for (size_t i = 0; i < events.size(); ++i)
					s += events[i].dump() + (i + 1 < events.size() ? ",\n" : "\n");
				s += "]";
			}
			if (a_scenario.contains("trackingSamples") && a_scenario["trackingSamples"].is_array()) {
				s += ",\n\"trackingSamples\": [\n";
				const json& samples = a_scenario["trackingSamples"];
				for (size_t i = 0; i < samples.size(); ++i)
					s += samples[i].dump() + (i + 1 < samples.size() ? ",\n" : "\n");
				s += "]";
			}
			if (!a_scenario.contains("steps") || a_scenario["steps"].is_array()) {
				s += ",\n\"steps\": [\n";
				const json steps = a_scenario.value("steps", json::array());
				for (size_t i = 0; i < steps.size(); ++i)
					s += steps[i].dump() + (i + 1 < steps.size() ? ",\n" : "\n");
				s += "]";
			}
			s += "\n}\n";
			return s;
		}

		fs::path WriteScenarioFile(const json& a_scenario)
		{
			const fs::path  dir = "Data/SKSE/Plugins/devbench/recordings";
			std::error_code ec;
			fs::create_directories(dir, ec);
			if (ec)
				throw std::runtime_error(std::format("could not create recording directory: {}", ec.message()));
			const auto        stamp = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
			const fs::path    path = dir / std::format("recording_{}.json", stamp);
			const fs::path    temporary = path.string() + ".tmp";
			const std::string serialized = SerializeRecording(a_scenario);
			{
				std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
				if (!out)
					throw std::runtime_error(std::format("could not open temporary recording file: {}", temporary.string()));
				out.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
				out.flush();
				if (!out)
					throw std::runtime_error(std::format("could not flush temporary recording file: {}", temporary.string()));
			}
			fs::rename(temporary, path, ec);
			if (ec)
				throw std::runtime_error(std::format("could not publish recording file: {}", ec.message()));
			if (!fs::exists(path, ec) || ec || fs::file_size(path, ec) != serialized.size() || ec)
				throw std::runtime_error("recording file verification failed after publication");
			return path;
		}
	}

	json Handle(const json& a_args, EventBus& a_events)
	{
		std::string action = a_args.value("action", std::string("status"));
		auto&       rec = Get();
		if (action == "toggle") {  // Auto-limited sessions still need hotkey finalization.
			std::lock_guard lock(rec.mtx);
			action = rec.state == RecorderState::idle ? "start" : "stop";
		}

		if (action == "start") {
			{
				std::lock_guard lock(rec.mtx);
				if (rec.state != RecorderState::idle)
					return json{ { "error", "recorder is not idle — stop or wait for the current operation" },
						{ "state", RecorderStateName(rec.state) } };
				rec.state = RecorderState::starting;
			}
			const auto cancelStart = [&rec]() {
				std::lock_guard lock(rec.mtx);
				rec.running.store(false, std::memory_order_relaxed);
				rec.state = RecorderState::idle;
			};

			if (a_args.contains("intervalMs") && !a_args["intervalMs"].is_number_integer()) {
				cancelStart();
				return json{ { "error", "intervalMs must be an integer" } };
			}
			long interval;
			try {
				interval = static_cast<long>(ParseBoundedIntegerArgument(a_args, "intervalMs",
					g_defaultIntervalMs, kMinIntervalMs, kMaximumVRTrackedDurationMs));
			} catch (const std::invalid_argument& e) {
				cancelStart();
				return json{ { "error", e.what() } };
			}

			// Capture the manifest synchronously: the player must be loaded to anchor the
			// scene, so fail fast (rather than starting an empty recording) if not.
			json manifest;
			try {
				manifest = MainThread::RunAndWait(&ReadManifest, milliseconds(3000));
			} catch (const std::exception& e) {
				cancelStart();
				logs::warn("devbench: record start failed — scene read timed out ({})", e.what());
				Notify("devbench: can't record — load a game first");
				return json{ { "error", "could not read scene — is a game loaded?" }, { "detail", e.what() } };
			}
			if (a_args.contains("allowNoPlayer") && !a_args["allowNoPlayer"].is_boolean()) {
				cancelStart();
				return json{ { "error", "allowNoPlayer must be a boolean" } };
			}
			const bool allowNoPlayer = a_args.value("allowNoPlayer", false);
			if (!manifest.contains("anchor") && !allowNoPlayer) {
				cancelStart();
				logs::warn("devbench: record start failed — player not loaded");
				Notify("devbench: can't record — load a game or use allowNoPlayer");
				return json{ { "error", "player not loaded — load a game or pass allowNoPlayer=true to capture main-menu/new-game activity" } };
			}
			const bool anchored = manifest.contains("anchor");
			if (a_args.contains("correlationId") && !a_args["correlationId"].is_string()) {
				cancelStart();
				return json{ { "error", "correlationId must be a string" } };
			}
			const std::string correlationId = a_args.value("correlationId", std::string{});
			if (correlationId.size() > 128) {
				cancelStart();
				return json{ { "error", "correlationId must contain at most 128 characters" } };
			}
			if (!correlationId.empty())
				manifest["correlationId"] = correlationId;
			json openMenus = json::array();
			for (const auto& menu : GetOpenMenus())
				openMenus.push_back(menu);
			manifest["openMenusAtStart"] = std::move(openMenus);
			manifest["startState"] = anchored ? "playerLoaded" : "noPlayer";
			if (manifest.value("entryPoint", json::object()).value("kind", std::string{}) == "unknown")
				logs::info(
					"devbench: recording with UNKNOWN entry point — replay won't restore the "
					"scene (load via the game tool or `coc` so devbench can capture it)");

			g_userCocPending.store(false, std::memory_order_relaxed);  // don't leak across sessions
			{
				std::lock_guard lock(rec.mtx);
				rec.samples.clear();
				rec.commands.clear();
				rec.checkpoints.clear();
				rec.activityEvents.clear();
				rec.trackingSamples.clear();
				rec.nextActivitySeq = 1;
				rec.limitReached = false;
				rec.limitReason.clear();
				rec.manifest = std::move(manifest);
				rec.intervalMs = interval;
				rec.startTick = steady_clock::now();
				const auto generation = ++rec.generation;
				rec.running.store(true, std::memory_order_relaxed);
				rec.state = RecorderState::running;
				try {
					rec.worker = std::thread([&rec, generation] { rec.Sample(generation); });
				} catch (...) {
					rec.running.store(false, std::memory_order_relaxed);
					rec.state = RecorderState::idle;
					throw;
				}
			}

			try {
				a_events.Publish("record.started", json{ { "intervalMs", interval },
													   { "anchored", anchored }, { "activityCapture", ActivityCaptureContract() } });
			} catch (const std::exception& e) {
				logs::warn("devbench: record.started event publish failed: {}", e.what());
			}
			Notify("devbench: recording started");
			logs::info("devbench: recording started (interval {}ms)", interval);
			return json{ { "action", "start" }, { "recording", true }, { "intervalMs", interval },
				{ "anchored", anchored }, { "correlationId", correlationId },
				{ "activityCapture", ActivityCaptureContract() } };
		}

		if (action == "checkpoint") {
			// Mark a screenshot checkpoint at THIS moment of an active recording -- mirrors how
			// `stop` already captures the trajectory with zero manual JSON editing after the fact.
			// Deliberately carries NO golden/threshold here: at mark-time there is by definition
			// no golden yet for a first-time checkpoint, and for a regression check the comparison
			// target belongs to replay (see record{action:"replay"}'s `goldens` arg), not to the
			// act of marking a moment -- baking it in here would reintroduce exactly the kind of
			// side-channel bookkeeping this action exists to eliminate.
			if (!rec.running.load())
				return json{ { "error", "not recording — call action=start first" } };
			const std::string id = a_args.value("id", std::string{});
			if (id.empty())
				return json{ { "error", "checkpoint requires a non-empty 'id'" } };

			long   atMs = 0;
			json   entry;
			size_t count = 0;
			{
				std::lock_guard lock(rec.mtx);
				if (rec.state != RecorderState::running)
					return json{ { "error", "not recording — call action=start first" } };
				atMs = static_cast<long>(duration_cast<milliseconds>(
					steady_clock::now() - rec.startTick)
						.count());
				entry = json{ { "id", id }, { "atMs", atMs },
					{ "excludeUi", a_args.value("excludeUi", true) } };
				if (std::any_of(rec.checkpoints.begin(), rec.checkpoints.end(),
						[&](const json& c) { return c.value("id", std::string{}) == id; }))
					return json{ { "error", std::format("checkpoint id '{}' already marked this recording", id) } };
				rec.checkpoints.push_back(entry);
				count = rec.checkpoints.size();
			}
			a_events.Publish("record.checkpoint", entry);
			Notify(std::format("devbench: checkpoint '{}' marked", id));
			logs::info("devbench: checkpoint '{}' marked at {}ms", id, atMs);
			return json{ { "action", "checkpoint" }, { "id", id }, { "atMs", atMs }, { "checkpointCount", count } };
		}

		if (action == "stop") {
			{
				std::lock_guard lock(rec.mtx);
				if (rec.state != RecorderState::running)
					return json{ { "error", "not recording" }, { "state", RecorderStateName(rec.state) } };
				rec.state = RecorderState::stopping;
				rec.running.store(false, std::memory_order_relaxed);
			}
			rec.cv.notify_all();
			if (rec.worker.joinable())
				rec.worker.join();  // sampler done → samples are stable, no lock needed below

			const long recordedMs = static_cast<long>(
				duration_cast<milliseconds>(steady_clock::now() - rec.startTick).count());
			json     scenario;
			fs::path path;
			try {
				scenario = BuildScenario(rec, recordedMs);
				path = WriteScenarioFile(scenario);
			} catch (const std::exception& e) {
				std::lock_guard lock(rec.mtx);
				rec.state = RecorderState::idle;
				throw ToolError(500, std::format("recording stopped but could not be persisted: {}", e.what()));
			}

			// generic_string(), not string(): a bare `dir / filename` join uses the native
			// separator (backslash on Windows) while the `dir` literal above keeps its forward
			// slashes verbatim, so string() mixed both in one path — fragile for callers that
			// split on '/'. generic_string() normalizes the whole path to forward slashes.
			const std::string pathStr = path.generic_string();
			const json        activityCounts = SummarizeActivity(rec.activityEvents);
			try {
				a_events.Publish("record.stopped", json{ { "sampleCount", rec.samples.size() },
													   { "trackingSampleCount", rec.trackingSamples.size() },
													   { "activityCounts", activityCounts }, { "path", pathStr } });
			} catch (const std::exception& e) {
				logs::warn("devbench: record.stopped event publish failed: {}", e.what());
			}
			Notify(std::format("devbench: recording stopped — {} samples, {:.1f}s", rec.samples.size(), recordedMs / 1000.0));
			logs::info("devbench: recording stopped — {} samples, {}ms -> {}", rec.samples.size(), recordedMs, pathStr);
			json response{
				{ "action", "stop" },
				{ "sampleCount", rec.samples.size() },
				{ "trackingSampleCount", rec.trackingSamples.size() },
				{ "checkpointCount", rec.checkpoints.size() },
				{ "activityCounts", activityCounts },
				{ "recordedMs", recordedMs },
				{ "limitReached", rec.limitReached },
				{ "limitReason", rec.limitReason },
				{ "path", pathStr },
				{ "meta", scenario["meta"] },
			};
			{
				std::lock_guard lock(rec.mtx);
				rec.state = RecorderState::idle;
			}
			return response;
		}

		if (action == "status") {
			std::lock_guard lock(rec.mtx);
			return json{
				{ "recording", rec.running.load() },
				{ "state", RecorderStateName(rec.state) },
				{ "correlationId", rec.manifest.value("correlationId", std::string{}) },
				{ "sampleCount", rec.samples.size() },
				{ "trackingSampleCount", rec.trackingSamples.size() },
				{ "limitReached", rec.limitReached },
				{ "limitReason", rec.limitReason },
				{ "maximumReplayableFrames", kMaximumVRTrackedFrames },
				{ "maximumReplayableDurationMs", kMaximumVRTrackedDurationMs },
				{ "intervalMs", rec.intervalMs },
				{ "checkpointCount", rec.checkpoints.size() },
				{ "activityCounts", SummarizeActivity(rec.activityEvents) },
			};
		}

		return json{ { "error", "unknown action (start|stop|status|checkpoint)" }, { "action", action } };
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
		g_entry = EntryPoint{ "save", a_saveName, steady_clock::now() };
		logs::info("devbench: entry point captured — save '{}'", a_saveName);
	}

	void NoteCocEntry(const std::string& a_cellId)
	{
		std::lock_guard lock(g_entryMtx);
		g_entry = EntryPoint{ "coc", a_cellId, steady_clock::now() };
	}

	void NoteConsoleCommand(const std::string& a_command)
	{
		auto&      rec = Get();
		const auto generation = ActiveGeneration();
		if (!generation)
			return;  // only capture while a recording is active
		// coc/cow are real user commands — capture them. But flag that the player just commanded a
		// transition, so the cell-load that follows (NoteCellChange) won't ALSO emit a coc for the
		// same move; the user's own command already reproduces it.
		const bool isCellTransition = a_command.size() >= 4 && a_command[3] == ' ' &&
		                              (a_command[0] | 0x20) == 'c' && (a_command[1] | 0x20) == 'o' &&
		                              ((a_command[2] | 0x20) == 'c' || (a_command[2] | 0x20) == 'w');
		{
			std::lock_guard lock(rec.mtx);
			if (rec.state != RecorderState::running || !rec.running.load(std::memory_order_relaxed) ||
				rec.generation != generation)
				return;
			rec.commands.push_back(json{ { "command", a_command }, { "frame", game::CurrentFrame() },
				{ "tMs", duration_cast<milliseconds>(steady_clock::now() - rec.startTick).count() } });
			if (isCellTransition)
				g_userCocPending.store(true, std::memory_order_relaxed);
		}
		AppendActivity(json{ { "kind", "console" }, { "command", a_command } }, generation);
	}

	void NoteCellChange(const std::string& a_command)
	{
		auto&      rec = Get();
		const auto generation = ActiveGeneration();
		if (!generation || a_command.empty())
			return;  // only capture while recording; caller passes "" when it can't build a command
		// If the player commanded this transition (a coc/cow was just captured), their own command
		// already reproduces it — consume the flag and skip, so we don't double it. A door issues
		// no console command, so the flag is clear and we capture the transition here.
		bool commandAlreadyCaptured = false;
		{
			std::lock_guard lock(rec.mtx);
			if (rec.state != RecorderState::running || !rec.running.load(std::memory_order_relaxed) ||
				rec.generation != generation)
				return;
			commandAlreadyCaptured = g_userCocPending.exchange(false, std::memory_order_relaxed);
			if (!commandAlreadyCaptured) {
				// Door and fast-travel transitions have no commanding console input. The caller
				// supplies a reproducible coc/cow command and the trajectory refines the position.
				rec.commands.push_back(json{ { "command", a_command }, { "frame", game::CurrentFrame() } });
			}
		}
		json activity{ { "kind", "cell" }, { "command", a_command } };
		if (commandAlreadyCaptured)
			activity["commandAlreadyCaptured"] = true;
		AppendActivity(std::move(activity), generation);
		if (!commandAlreadyCaptured)
			logs::info("devbench: recorded cell transition — {}", a_command);
	}

	void NoteInputEvents(RE::InputEvent* const* a_events)
	{
		const auto generation = ActiveGeneration();
		if (!a_events || !generation ||
			g_replaying.load(std::memory_order_relaxed))
			return;
		for (const auto* event = *a_events; event; event = event->next)
			AppendActivity(SerializeInputEvent(*event), generation);
	}

	void NoteMenuState(const std::string& a_menuName, bool a_opening)
	{
		const auto generation = ActiveGeneration();
		AppendActivity(json{ { "kind", "menu" }, { "name", a_menuName }, { "opening", a_opening } }, generation);
	}

	void NoteLifecycleEvent(const std::string& a_event)
	{
		const auto generation = ActiveGeneration();
		AppendActivity(json{ { "kind", "lifecycle" }, { "event", a_event } }, generation);
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
		g_defaultIntervalMs = std::clamp<long>(a_ms, kMinIntervalMs,
			kMaximumVRTrackedDurationMs);
	}

	void SetCoupling(int a_anchorMs, int a_cellMs, bool a_cleanTransition, const std::string& a_transitionCell)
	{
		g_anchorMs = (a_anchorMs < 0) ? 0 : a_anchorMs;
		g_cellMs = (a_cellMs < a_anchorMs) ? a_anchorMs : a_cellMs;  // cell window must cover the anchor window
		g_cleanTransition = a_cleanTransition;
		g_cleanTransitionCell = a_transitionCell;
	}

	void SetCaptureDefaults(int a_settleMs)
	{
		g_captureSettleMs = (a_settleMs < 0) ? 0 : a_settleMs;
	}

	namespace
	{
		// The recording's meta.capabilities entry for "capture", or an empty object if it
		// declares none (which means: no gate, any/no provider is fine).
		json FindCaptureCapability(const json& a_meta)
		{
			for (const auto& cap : a_meta.value("capabilities", json::array()))
				if (cap.value("capability", std::string{}) == "capture")
					return cap;
			return json::object();
		}

		// Validate + sort meta.checkpoints by atMs. Throws on a duplicate/missing id or a
		// negative atMs — a bad checkpoint should fail the replay call up front, not silently
		// misfire mid-trajectory.
		json SortedCheckpoints(const json& a_meta)
		{
			json checkpoints = a_meta.value("checkpoints", json::array());
			if (!checkpoints.is_array())
				throw ToolError(400, "meta.checkpoints must be an array");
			std::vector<std::string> seen;
			for (const auto& cp : checkpoints) {
				if (!cp.contains("id") || !cp["id"].is_string() || cp["id"].get<std::string>().empty())
					throw ToolError(400, "each checkpoint requires a non-empty string 'id'");
				const std::string id = cp["id"].get<std::string>();
				if (std::find(seen.begin(), seen.end(), id) != seen.end())
					throw ToolError(400, std::format("duplicate checkpoint id '{}'", id));
				seen.push_back(id);
				if (cp.value("atMs", 0LL) < 0)
					throw ToolError(400, std::format("checkpoint '{}' has negative atMs", id));
			}
			std::stable_sort(checkpoints.begin(), checkpoints.end(),
				[](const json& a, const json& b) { return a.value("atMs", 0LL) < b.value("atMs", 0LL); });
			return checkpoints;
		}

		// Expand one checkpoint into existing scenario primitives — a MACRO, not a new step
		// kind (the scenario step list stays a thin sequencer; see ROADMAP.md's "keep scenario
		// thin" scope guard). No pose step is emitted: the trajectory's own immediately-preceding
		// `pose` step already set position/angle, so re-issuing it would be a redundant no-op;
		// only POV is re-asserted (Skyrim's idle-vanity timer can flip it). No HUD-suppression
		// step either — UI exclusion is the capture provider's job (`excludeUi` in the capture
		// args), not something the step list can do reliably (a console `tm` toggle leaks HUD-
		// hidden state on any aborted step).
		void AppendCheckpointSteps(json& a_steps, const json& a_cp, const json& a_cap,
			const std::string& a_recordingStem, const json& a_args, long a_cumMs, long a_defaultSettleMs)
		{
			// waitUntil FIRST so a transient menu (a loading spinner, a fading message box) can
			// clear within its timeout; assert only fails the checkpoint if it's STILL blocked
			// afterward. The reverse order made the wait pointless -- assert fired on whatever
			// was open at this exact instant, before the wait ever got a chance to run.
			a_steps.push_back(json{ { "waitUntil", "noBlockingMenu" }, { "timeoutMs", 5000 }, { "pollMs", 100 } });
			a_steps.push_back(json{ { "assert", "noBlockingMenu" } });
			if (a_cp.contains("pov"))
				a_steps.push_back(json{ { "tool", "camera" }, { "args", json{ { "action", "setPov" }, { "pov", a_cp["pov"] } } } });
			if (const long settleMs = a_cp.value("settleMs", a_defaultSettleMs); settleMs > 0)
				a_steps.push_back(json{ { "wait", settleMs } });

			// kind is the CAPABILITY-RESOLVED provider (or "auto" if none was declared), never a
			// bare "auto" independent of the gate that already validated it above — otherwise the
			// gate and the macro could disagree (gate passes because the named provider IS
			// registered, but "auto" 400s at runtime if a second provider also happens to be
			// registered). ".value()" only substitutes the default when the key is ABSENT, so an
			// explicit-but-empty "provider": "" (a malformed recipe) needs its own fallback too.
			std::string provider = a_cap.value("provider", std::string("auto"));
			if (provider.empty())
				provider = "auto";
			json capArgs{
				{ "kind", provider },
				{ "allowNative", a_cap.value("allowNative", false) },
				{ "checkpointId", a_cp.at("id") },
				{ "recording", a_recordingStem },
				{ "variant", a_args.value("variant", std::string("default")) },
				{ "excludeUi", a_cp.value("excludeUi", true) },
				{ "atMs", a_cp.value("atMs", 0LL) },
				{ "resolvedAtMs", a_cumMs },
				{ "resolvedIndex", static_cast<long>(a_steps.size()) },
			};
			if (a_cp.contains("subrect"))
				capArgs["subrect"] = a_cp["subrect"];

			// golden/threshold/regions come from THIS replay call's "goldens" map, keyed by
			// checkpoint id — never from the checkpoint itself (meta.checkpoints carries no
			// golden; see record{action:"checkpoint"}'s doc comment for why). Lets the same
			// recording be replayed against different variants' goldens without touching the
			// recording file at all.
			if (const json goldens = a_args.value("goldens", json::object());
				goldens.is_object() && goldens.contains(a_cp.at("id").get<std::string>())) {
				const json& g = goldens.at(a_cp.at("id").get<std::string>());
				if (g.is_object()) {
					if (g.contains("golden"))
						capArgs["golden"] = g["golden"];
					if (g.contains("threshold"))
						capArgs["threshold"] = g["threshold"];
					if (g.contains("regions"))
						capArgs["regions"] = g["regions"];
				}
			}
			a_steps.push_back(json{ { "tool", "capture" }, { "args", std::move(capArgs) } });
		}
	}

	json BuildReplaySteps(const json& a_args)
	{
		std::string path = a_args.value("path", std::string{});
		if (path.empty()) {
			// No path → most recently RECORDED (replay hotkey / quick calls). Rank by the epoch in
			// the auto-generated recording_<epoch>.json name, not mtime — a copy/deploy/checkout
			// re-timestamps files, letting a shipped default shadow the user's real last. Unstamped
			// named files rank oldest (stamp 0); "last" is well-defined only when a stamp exists.
			const fs::path  dir = "Data/SKSE/Plugins/devbench/recordings";
			std::error_code ec;
			fs::path        newest;
			long long       bestStamp = -1;
			for (const auto& e : fs::directory_iterator(dir, ec)) {
				if (e.path().extension() != ".json")
					continue;
				const std::string stem = e.path().stem().string();
				long long         stamp = 0;
				if (stem.starts_with("recording_")) {
					try {
						stamp = std::stoll(stem.substr(10));
					} catch (...) {
					}
				}
				if (newest.empty() || stamp > bestStamp) {
					bestStamp = stamp;
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

		const json        meta = rec.value("meta", json::object());
		const json        entry = meta.value("entryPoint", json::object());
		const std::string kind = entry.value("kind", std::string{});
		const std::string value = entry.value("value", std::string{});

		// Recorded scene identity (for the assert + tier). Interiors carry no worldspace.
		const bool          interior = meta.value("interior", false);
		const std::uint32_t wsFormID = meta.value("worldspaceFormID", static_cast<std::uint32_t>(0));
		const std::uint32_t cellFormID = meta.value("cellFormID", static_cast<std::uint32_t>(0));
		const bool          haveScene = interior ? (cellFormID != 0) : (wsFormID != 0);

		// Coupling tier: a recipe may pin it (or its thresholds) in meta.coupling; otherwise
		// classify entryPoint.ageMs against the config windows. Unknown age (old recipe / a
		// walked-in entry) → "cell": restore best-effort and assert the scene.
		const json  coupling = meta.value("coupling", json::object());
		const long  anchorMs = coupling.value("anchorMs", g_anchorMs);
		const long  cellMs = coupling.value("cellMs", g_cellMs);
		std::string tier = coupling.value("tier", std::string{});
		if (tier.empty()) {
			if (entry.contains("ageMs")) {
				const std::int64_t age = entry.value("ageMs", static_cast<std::int64_t>(0));
				tier = (age <= anchorMs) ? "anchored" : (age <= cellMs) ? "cell" :
				                                                          "worldspace";
			} else {
				tier = "cell";
			}
		}

		// The recipe's tier is the PRODUCER's signal ("how tightly this needs its start").
		// A CONSUMER may override it — run looser than the producer asked, accepting it may
		// not reproduce (e.g. force "worldspace" to skip a save-coupled recipe's restore).
		const std::string producerTier = tier;
		if (const std::string ov = a_args.value("coupling", std::string{}); !ov.empty()) {
			if (ov != "anchored" && ov != "cell" && ov != "worldspace")
				throw ToolError(400, std::format("invalid coupling '{}' (anchored | cell | worldspace)", ov));
			tier = ov;
		}
		// `force`: proceed even if the scene doesn't match — the scene assert below becomes a
		// reported warning instead of an abort. The consumer explicitly opted into "may not work".
		const bool force = a_args.value("force", false);

		const bool captureCheckpoints = a_args.value("captureCheckpoints", true);

		// Runtime gate: a flat setpos/setangle recording gives non-comparable frames on VR (HMD
		// drives pitch + culling), and a VR recording won't drive a flat game. Abort on a runtime
		// the recording wasn't marked for; force downgrades it to a warning. Unmarked (v1) = ungated.
		if (const json compat = meta.value("runtime", json::object()).value("compat", json::array()); !compat.empty()) {
			const bool curVR = REL::Module::IsVR();
			bool       ok = false;
			for (const auto& c : compat)
				if (const std::string t = c.get<std::string>(); curVR ? t == "vr" : (t == "se" || t == "ae")) {
					ok = true;
					break;
				}
			if (!ok && !force)
				throw ToolError(409, std::format("recording is for runtimes {} but this game is {} — pass force to replay anyway",
										 compat.dump(), curVR ? "vr" : "flat (se/ae)"));
		}

		// Capability gate: if this recording declares checkpoints need a capture provider,
		// check it's actually available BEFORE running anything — same shape as the runtime
		// gate above (force downgrades an abort to a warning), so a missing provider fails
		// clearly and early instead of 400ing on the first checkpoint's capture step deep
		// into the trajectory.
		const json captureCap = FindCaptureCapability(meta);
		if (captureCheckpoints && !captureCap.empty() && captureCap.value("required", true)) {
			const std::string want = captureCap.value("provider", std::string{});
			const auto        keys = ToolExtensions::Keys("capture");
			bool              ok = want.empty() ? !keys.empty() : ToolExtensions::Find("capture", want).has_value();
			if (!ok && captureCap.value("allowNative", false))
				ok = true;  // native is always available as a (lower-fidelity) fallback
			if (!ok && !force) {
				std::string names;
				for (const auto& k : keys)
					names += (names.empty() ? "" : ", ") + k;
				throw ToolError(409, std::format(
										 "recording requires capture provider '{}' but none is registered (registered: [{}]) — "
										 "install the provider mod (see inspect kind=registrants), set "
										 "meta.capabilities[].allowNative, or pass force",
										 want.empty() ? "<any>" : want, names));
			}
		}

		const bool restoreScene = a_args.value("restoreScene", false);
		const long settleMs = a_args.value("settleMs", static_cast<long>(g_loadSettleMs));
		const bool cleanTransition = a_args.value("cleanTransition", g_cleanTransition);

		// Restore the recorded entry, per tier. "worldspace" treats the entry as incidental and
		// skips the restore — it only requires landing in the recorded worldspace, which the
		// assert below enforces (the trajectory's own cow/setpos handle the positioning).
		bool restored = false;
		if (restoreScene && tier != "worldspace" && !kind.empty() && kind != "unknown") {
			if (kind == "save" && !value.empty()) {
				// Quicksave/autosave names are rolling — the slot name changes on every save, so
				// the recorded value stales. Load most-recent instead. Named saves are stable.
				// Wait postLoadGame not playerLoaded: if already in-game, playerLoaded is true
				// before the reload completes.
				const bool isRollingSlot =
					value.compare(0, 9, "Quicksave") == 0 || value.compare(0, 8, "Autosave") == 0;
				if (isRollingSlot)
					steps.push_back(json{ { "tool", "game" }, { "args", json{ { "action", "loadLast" } } } });
				else
					steps.push_back(json{ { "tool", "game" }, { "args", json{ { "action", "load" }, { "name", value } } } });
				// A content-mismatch box ("save relies on content ... no longer present") gates the
				// load; auto-answer it (non-cancel) so the restore proceeds instead of stalling 60s.
				steps.push_back(json{ { "waitFor", "postLoadGame" }, { "timeoutMs", 60000 },
					{ "acceptModal", json{ { "matchBody", "no longer present" } } } });
				restored = true;
			} else if (kind == "coc" && !value.empty()) {
				// A raw coc can stream without the loading-screen teardown some mods rely on to
				// free resources (→ CTD). Bounce through a neutral interior first to force a clean
				// loading screen (save-loads already tear down, so they skip this).
				if (cleanTransition && !g_cleanTransitionCell.empty() && g_cleanTransitionCell != value) {
					steps.push_back(json{ { "tool", "console" }, { "args", json{ { "action", "exec" }, { "command", "coc " + g_cleanTransitionCell } } } });
					steps.push_back(json{ { "waitUntil", "playerLoaded" }, { "timeoutMs", 60000 } });
					if (settleMs > 0)
						steps.push_back(json{ { "wait", settleMs } });
				}
				// Exterior editor ids are not unique across worldspaces, and a
				// raw coc from an interior straight into a worldspace can wedge
				// the engine's streaming init (observed: permanent main-thread
				// hang). Exterior entries restore via the recorded worldspace +
				// anchor grid cell instead; interiors keep coc (unique ids).
				std::string       enter = "coc " + value;
				const std::string ws = meta.value("worldspace", std::string{});
				// A display name with spaces would not parse as a console arg;
				// such recordings keep the coc fallback.
				if (!interior && meta.contains("anchor") && !ws.empty() && ws.find(' ') == std::string::npos) {
					const json anchor = meta.value("anchor", json::object());
					const int  gx = static_cast<int>(std::floor(anchor.value("x", 0.0) / 4096.0));
					const int  gy = static_cast<int>(std::floor(anchor.value("y", 0.0) / 4096.0));
					enter = std::format("cow {} {} {}", ws, gx, gy);
				} else if (!interior) {
					logs::warn("devbench record(replay): exterior entry restored via coc ('{}' unusable for cow) -- editor-id ambiguity possible", ws);
				}
				steps.push_back(json{ { "tool", "console" }, { "args", json{ { "action", "exec" }, { "command", enter } } } });
				steps.push_back(json{ { "waitUntil", "playerLoaded" }, { "timeoutMs", 60000 } });
				restored = true;
				// anchored: a save-load would restore time/weather, but a coc doesn't — re-apply
				// the recorded lighting so the shader benchmark stays comparable.
				if (tier == "anchored") {
					if (meta.contains("gameHour"))
						steps.push_back(json{ { "tool", "console" }, { "args", json{ { "action", "exec" }, { "command", std::format("set gamehour to {}", meta.value("gameHour", 12.0)) } } } });
					if (meta.contains("weatherFormID"))
						steps.push_back(json{ { "tool", "console" }, { "args", json{ { "action", "exec" }, { "command", std::format("fw {:X}", meta.value("weatherFormID", static_cast<std::uint32_t>(0))) } } } });
				}
			}
			if (restored && settleMs > 0)
				steps.push_back(json{ { "wait", settleMs } });
			if (!restored)
				logs::warn("devbench record(replay): restoreScene requested but entryPoint is '{}' — running trajectory without scene restore",
					kind.empty() ? "unknown" : kind);
		}

		// Assert we're in the recorded scene before the trajectory runs, so a wrong worldspace
		// (e.g. coc ambiguity landing in Soul Cairn) aborts the replay instead of producing a
		// bogus benchmark. Runs even without a restore — catches an in-place replay in the wrong
		// scene. Coarse by design: the parent cell (interior) or the worldspace (exterior).
		if (haveScene) {
			steps.push_back(json{
				{ "assert", "scene" },
				{ "interior", interior },
				{ "worldspaceFormID", wsFormID },
				{ "cellFormID", cellFormID },
				{ "worldspace", meta.value("worldspace", std::string{}) },
				{ "cell", meta.value("cell", std::string{}) },
				{ "soft", force },  // forced → report a mismatch instead of aborting
			});
		}

		// Fail fast if a menu/modal is open before an in-game trajectory plays: its
		// setpos/setangle would otherwise be eaten. A recording explicitly started without a
		// player is a main-menu/new-game trace; its initial menus are the subject, not a blocker
		// (without the in-game guard, such a replay silently no-ops).
		const bool allowsInitialMenus = meta.value("startState", std::string{}) == "noPlayer";
		if (!allowsInitialMenus)
			steps.push_back(json{ { "assert", "noBlockingMenu" } });

		// Copy the trajectory, injecting a load-settle after any captured cell transition (coc/cow):
		// the destination cell must finish loading before the following setpos teleports the player,
		// or the replay teleports onto a not-yet-valid ref mid-load and CTDs. Done here (not baked
		// into the recording) so existing recipes get the fix too.
		//
		// Interleaved: checkpoint capture steps, flushed once cumMs (the cumulative sum of the
		// RECORDING'S OWN "wait" values — the same clock BuildScenario stamped from tMs deltas)
		// reaches each checkpoint's atMs. This is resolved HERE, once, at plan time — nothing
		// during replay execution (a slow cell load, a long waitFor) can move it, because the
		// checkpoint's insertion point is a fixed index in the already-built step list by the
		// time replay starts. cumMs deliberately sums ONLY the original recording's own "wait"
		// steps below — the coc/cow settle steps injected right after them, and the restore
		// prologue's settle waits above, are NOT part of that clock and must never be added in.
		const long        txnSettleMs = a_args.value("settleMs", static_cast<long>(g_loadSettleMs));
		const json        checkpoints = captureCheckpoints ? SortedCheckpoints(meta) : json::array();
		const std::string recordingStem = fs::path(path).stem().string();
		const bool        replayInputs = a_args.value("replayInputs", true);
		const std::string inputOwner = "recording:" + recordingStem;
		json              activityPlan;
		json              vrPlan;
		try {
			activityPlan = InterleaveReplayableActivity(rec["steps"],
				rec.value("activityEvents", json::array()), inputOwner, replayInputs);
			vrPlan = BuildVRTrackedSetReplay(rec.value("trackingSamples", json::array()),
				rec.value("activityEvents", json::array()), inputOwner, replayInputs);
		} catch (const json::exception& e) {
			throw ToolError(400, std::format("invalid recording activity/tracking data: {}", e.what()));
		} catch (const std::invalid_argument& e) {
			throw ToolError(400, std::format("invalid recording activity/tracking data: {}", e.what()));
		}
		const json& trajectory = activityPlan["steps"];
		long        cumMs = 0;
		size_t      cpIdx = 0;
		if (!vrPlan.value("step", json(nullptr)).is_null())
			steps.push_back(vrPlan["step"]);
		for (const auto& s : trajectory) {
			steps.push_back(s);
			if (s.contains("wait"))
				cumMs += s["wait"].get<long>();
			if (s.value("tool", std::string{}) == "console") {
				const std::string c = s.value("args", json::object()).value("command", std::string{});
				if (c.size() >= 4 && c[3] == ' ' && (c[0] | 0x20) == 'c' && (c[1] | 0x20) == 'o' &&
					((c[2] | 0x20) == 'c' || (c[2] | 0x20) == 'w')) {
					steps.push_back(json{ { "waitUntil", "playerLoaded" }, { "timeoutMs", 60000 } });
					if (txnSettleMs > 0)
						steps.push_back(json{ { "wait", txnSettleMs } });
				}
			}
			while (cpIdx < checkpoints.size() && checkpoints[cpIdx].value("atMs", 0LL) <= cumMs)
				AppendCheckpointSteps(steps, checkpoints[cpIdx++], captureCap, recordingStem, a_args, cumMs, g_captureSettleMs);
		}
		// A main-menu/new-game trace may have no player trajectory for its opening portion. Keep the
		// scenario alive until the atomic VR stream finishes instead of immediately cleaning it up.
		const long vrDurationMs = vrPlan.value("durationMs", 0L);
		if (vrDurationMs > cumMs) {
			steps.push_back(json{ { "wait", vrDurationMs - cumMs } });
			cumMs = vrDurationMs;
		}
		// Checkpoints anchored past the end of the trajectory still fire, at the end.
		while (cpIdx < checkpoints.size())
			AppendCheckpointSteps(steps, checkpoints[cpIdx++], captureCap, recordingStem, a_args, cumMs, g_captureSettleMs);

		// Return the steps plus the effective coupling so the caller can surface what it
		// actually did (which tier ran, whether the consumer overrode the producer's signal).
		json activity = activityPlan.value("report", json::object());
		activity["vrTrackedSet"] = vrPlan.value("report", json::object());
		return json{
			{ "steps", std::move(steps) },
			{ "activity", std::move(activity) },
			{ "inputOwner", (!activityPlan.value("inputOwner", std::string{}).empty() ||
								!vrPlan.value("inputOwner", std::string{}).empty()) ?
								inputOwner :
								std::string{} },
			{ "restored", restored },  // handler's sync menu pre-check skips restore plans (the load clears menus)
			{ "allowsInitialMenus", allowsInitialMenus },
			{ "coupling", json{
							  { "tier", tier },
							  { "producer", producerTier },
							  { "overridden", tier != producerTier },
							  { "forced", force },
						  } },
		};
	}

	json ManageRecordings(const json& a_args)
	{
		const fs::path    dir = "Data/SKSE/Plugins/devbench/recordings";
		const std::string action = a_args.value("action", std::string("list"));

		// Resolve a caller-supplied name INSIDE the recordings dir; reject a path separator or ".."
		// so a tool call can't read or delete outside the library.
		const auto safePath = [&](const std::string& a_file) -> fs::path {
			if (a_file.empty())
				throw ToolError(400, "'file' is required");
			const fs::path name(a_file);
			if (name.has_parent_path() || a_file.find("..") != std::string::npos)
				throw ToolError(400, "'file' must be a bare recording name (no path)");
			return dir / name;
		};

		// Summarize one recording's meta for the library list; a bad/half-written file is reported,
		// not fatal -- the list must still return.
		const auto summarize = [](const fs::path& a_p) -> json {
			json          out{ { "file", a_p.filename().string() } };
			std::ifstream in(a_p);
			json          rec;
			try {
				in >> rec;
			} catch (...) {
				out["error"] = "unreadable / invalid JSON";
				return out;
			}
			const json m = rec.value("meta", json::object());
			for (const char* k : { "name", "format", "cell", "worldspace", "interior",
					 "sampleCount", "recordedMs", "recordedAt", "validated" })
				if (m.contains(k))
					out[k] = m[k];
			out["runtime"] = m.value("runtime", json::object()).value("compat", json::array());
			out["entry"] = m.value("entryPoint", json::object());
			return out;
		};

		const auto load = [&](const fs::path& a_p) -> json {
			std::ifstream in(a_p);
			if (!in)
				throw ToolError(404, std::format("recording not found: {}", a_p.filename().string()));
			json rec;
			try {
				in >> rec;
			} catch (const std::exception& e) {
				throw ToolError(400, std::format("invalid recording JSON: {}", e.what()));
			}
			return rec;
		};

		if (action == "list") {
			std::error_code   ec;
			std::vector<json> items;
			for (const auto& e : fs::directory_iterator(dir, ec))
				if (e.path().extension() == ".json")
					items.push_back(summarize(e.path()));
			// newest recorded first; files without recordedAt (v1) sort last.
			std::sort(items.begin(), items.end(), [](const json& a, const json& b) {
				return a.value("recordedAt", 0LL) > b.value("recordedAt", 0LL);
			});
			json arr = json::array();
			for (auto& it : items)
				arr.push_back(std::move(it));
			return json{ { "dir", dir.string() }, { "count", arr.size() }, { "recordings", std::move(arr) } };
		}

		if (action == "describe") {
			const fs::path p = safePath(a_args.value("file", std::string{}));
			return json{ { "file", p.filename().string() }, { "meta", load(p).value("meta", json::object()) } };
		}

		if (action == "validate") {
			const fs::path p = safePath(a_args.value("file", std::string{}));
			const bool     value = a_args.value("value", true);
			json           rec = load(p);
			rec["meta"]["validated"] = value;
			std::ofstream out(p, std::ios::trunc);
			if (!out)
				throw ToolError(500, "could not write recording");
			out << SerializeRecording(rec);
			return json{ { "file", p.filename().string() }, { "validated", value } };
		}

		if (action == "delete") {
			const fs::path  p = safePath(a_args.value("file", std::string{}));
			std::error_code ec;
			if (!fs::exists(p, ec))
				throw ToolError(404, std::format("recording not found: {}", p.filename().string()));
			if (!fs::remove(p, ec) || ec)
				throw ToolError(500, std::format("could not delete {}: {}", p.filename().string(), ec.message()));
			return json{ { "file", p.filename().string() }, { "deleted", true } };
		}

		throw ToolError(400, std::format("unknown action '{}' (list | describe | validate | delete)", action));
	}
}
