#include "ReachControl.h"

#include "MainThread.h"
#include "ReachLogic.h"
#include "VRInput.h"

#include <RE/B/BSFixedString.h>
#include <RE/N/NiAVObject.h>
#include <RE/P/PlayerCharacter.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace dvb::ReachControl
{
	namespace
	{
		namespace RL = dvb::ReachLogic;

		constexpr std::size_t kMaxRuns = 16;
		constexpr double      kCalibrationStep = 0.2;
		constexpr long        kIterationSettleMs = 2400;
		constexpr long        kStopPollMs = 100;
		constexpr long        kBetweenIterationsMs = 400;
		constexpr long        kShortHoldMs = 3000;
		constexpr long        kSequenceTailMs = 300;

		const RL::Vec3 kRestHandRight{ 0.25, 0.0, -0.3 };
		const RL::Vec3 kRestHandLeft{ -0.25, 0.0, -0.3 };
		const RL::Vec3 kDefaultHmd{ 0.0, 0.03, 0.0 };

		constexpr const char* kRightHandNode = "NPC R Hand [RHnd]";
		constexpr const char* kLeftHandNode = "NPC L Hand [LHnd]";

		struct Run
		{
			long              runId = 0;
			std::string       owner;
			std::atomic<bool> stop{ false };
			std::atomic<bool> active{ true };
			RL::Progress      progress{ RL::Phase::kQueued, 0, std::nullopt, std::nullopt, std::string{} };
		};

		std::mutex                        g_mutex;
		std::vector<std::shared_ptr<Run>> g_runs;
		long                              g_nextRunId = 1;

		struct ActiveFlag
		{
			std::shared_ptr<Run> run;
			~ActiveFlag() { run->active.store(false); }
		};

		struct StopRequested
		{
		};

		void SetPhase(const std::shared_ptr<Run>& a_run, RL::Phase a_phase)
		{
			std::lock_guard lock(g_mutex);
			a_run->progress.phase = a_phase;
		}

		void SetError(const std::shared_ptr<Run>& a_run, std::string a_error)
		{
			std::lock_guard lock(g_mutex);
			a_run->progress.error = std::move(a_error);
		}

		std::string TokenOf(const json& a_result)
		{
			if (a_result.contains("controlToken") && a_result["controlToken"].is_string())
				return a_result["controlToken"].get<std::string>();
			return {};
		}

		json StartSequence(const std::string& a_owner, const json& a_frames, long a_tailMs, const ToolContext& a_ctx)
		{
			const json args{
				{ "action", "sequence" },
				{ "owner", a_owner },
				{ "frames", a_frames },
				{ "tailMs", a_tailMs },
			};
			return dvb::HandleVRInput(args, a_ctx);
		}

		void StopSequence(const std::string& a_owner, const std::string& a_token, const ToolContext& a_ctx)
		{
			try {
				json args{
					{ "action", "stop" },
					{ "owner", a_owner },
				};
				if (!a_token.empty())
					args["controlToken"] = a_token;
				const json result = dvb::HandleVRInput(args, a_ctx);
				if (result.value("restorationPending", false))
					logs::warn("devbench: reach '{}' sequence stop left controller restoration pending", a_owner);
			} catch (const std::exception& e) {
				// The sequence may already have expired on its own; anything else is worth a log line.
				logs::warn("devbench: reach '{}' sequence stop failed: {}", a_owner, e.what());
			}
		}

		class SequenceGuard
		{
		public:
			SequenceGuard(const std::string& a_owner, const ToolContext& a_ctx) :
				m_owner(a_owner), m_ctx(a_ctx)
			{
			}

			~SequenceGuard()
			{
				Stop();
			}

			SequenceGuard(const SequenceGuard&) = delete;
			SequenceGuard& operator=(const SequenceGuard&) = delete;

			void Start(const json& a_frames, long a_tailMs)
			{
				Stop();
				const json result = StartSequence(m_owner, a_frames, a_tailMs, m_ctx);
				m_token = TokenOf(result);
				m_active = true;
			}

			void Stop()
			{
				if (!m_active)
					return;
				m_active = false;
				const std::string token = m_token;
				m_token.clear();
				StopSequence(m_owner, token, m_ctx);
			}

		private:
			const std::string& m_owner;
			const ToolContext& m_ctx;
			std::string        m_token;
			bool               m_active = false;
		};

		double ReadPlayerYaw()
		{
			const json result = dvb::MainThread::RunAndWait([]() -> json {
				auto* pc = RE::PlayerCharacter::GetSingleton();
				if (!pc)
					throw ToolError(503, "player is not available");
				return json{ { "yaw", pc->GetAngleZ() } };
			});
			return result.at("yaw").get<double>();
		}

		RL::Vec3 ReadNodeWorld(const std::string& a_name)
		{
			const json result = dvb::MainThread::RunAndWait([a_name]() -> json {
				auto* pc = RE::PlayerCharacter::GetSingleton();
				auto* root = pc ? pc->Get3D(false) : nullptr;
				auto* node = root ? root->GetObjectByName(RE::BSFixedString(a_name.c_str())) : nullptr;
				if (!node)
					throw ToolError(404, std::format("node '{}' not found on the player", a_name));
				const auto& t = node->world.translate;
				return json{ { "x", t.x }, { "y", t.y }, { "z", t.z } };
			});
			return RL::Vec3{
				result.at("x").get<double>(),
				result.at("y").get<double>(),
				result.at("z").get<double>(),
			};
		}

		RL::Vec3 WorldTarget(const std::string& a_node, double a_front, const RL::Vec3& a_offset, const RL::Vec3& a_facing)
		{
			const RL::Vec3 node = ReadNodeWorld(a_node);
			return RL::Add(node, RL::Add(RL::Scale(a_facing, a_front), a_offset));
		}

		std::optional<RL::HandProgress> MeasureHand(const std::optional<RL::HandTarget>& a_target, const char* a_handNode, const RL::Vec3& a_facing)
		{
			if (!a_target)
				return std::nullopt;
			const RL::Vec3 hand = ReadNodeWorld(a_handNode);
			const RL::Vec3 node = ReadNodeWorld(a_target->node);
			const RL::Vec3 target = RL::Add(node, RL::Add(RL::Scale(a_facing, a_target->front), a_target->offset));
			return RL::HandProgress{ hand, node, RL::Sub(target, hand) };
		}

		void SleepChecked(const std::shared_ptr<Run>& a_run, long a_ms)
		{
			long remaining = a_ms;
			while (remaining > 0) {
				if (a_run->stop.load())
					throw StopRequested{};
				const long slice = std::min<long>(remaining, kStopPollMs);
				std::this_thread::sleep_for(std::chrono::milliseconds(slice));
				remaining -= slice;
			}
			if (a_run->stop.load())
				throw StopRequested{};
		}

		void SleepUntil(const std::shared_ptr<Run>& a_run, std::chrono::steady_clock::time_point a_deadline)
		{
			for (;;) {
				if (a_run->stop.load())
					throw StopRequested{};
				const auto now = std::chrono::steady_clock::now();
				if (now >= a_deadline)
					return;
				const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(a_deadline - now).count();
				const long slice = std::min<long>(static_cast<long>(remaining), kStopPollMs);
				std::this_thread::sleep_for(std::chrono::milliseconds(std::max<long>(slice, 1)));
			}
		}

		RL::Calibration CalibrateHand(bool a_rightHand, const RL::Pose& a_hmd, const RL::Vec3& a_rest, const RL::Vec3& a_otherTracking, double a_step, SequenceGuard& a_guard, const std::shared_ptr<Run>& a_run)
		{
			const json        frames = RL::CalibrationFrames(a_rightHand, a_hmd, a_rest, a_otherTracking, a_step);
			const std::string nodeName = a_rightHand ? kRightHandNode : kLeftHandNode;

			const auto t0 = std::chrono::steady_clock::now();
			a_guard.Start(frames, kSequenceTailMs);

			std::array<RL::Vec3, 4> points{};
			for (int i = 0; i < 4; ++i) {
				SleepUntil(a_run, t0 + std::chrono::milliseconds(RL::CalibrationSampleMs(i)));
				points[static_cast<std::size_t>(i)] = ReadNodeWorld(nodeName);
			}
			SleepUntil(a_run, t0 + std::chrono::milliseconds(RL::kCalibrationTotalMs));
			a_guard.Stop();

			const auto cal = RL::CalibrationFromPoints(points, a_step);
			if (!cal)
				throw ToolError(500, "calibration is singular: the hand did not move along every axis");
			return *cal;
		}

		void RunWorker(std::shared_ptr<Run> a_run, RL::ReachRequest a_req, ToolContext a_ctx)
		{
			SequenceGuard    guard(a_run->owner, a_ctx);
			const ActiveFlag activeFlag{ a_run };
			try {
				SetPhase(a_run, RL::Phase::kCalibrating);

				const double   yaw = ReadPlayerYaw();
				const RL::Vec3 facing{ std::sin(yaw), std::cos(yaw), 0.0 };
				const RL::Pose calibHmd = RL::Upright(kDefaultHmd);

				std::optional<RL::Calibration> calLeft;
				std::optional<RL::Calibration> calRight;
				if (a_req.left)
					calLeft = CalibrateHand(false, calibHmd, kRestHandLeft, kRestHandRight, kCalibrationStep, guard, a_run);
				if (a_req.right)
					calRight = CalibrateHand(true, calibHmd, kRestHandRight, kRestHandLeft, kCalibrationStep, guard, a_run);

				RL::Vec3 trackLeft = kRestHandLeft;
				RL::Vec3 trackRight = kRestHandRight;
				if (a_req.left)
					trackLeft = RL::TrackingFor(*calLeft, kRestHandLeft, WorldTarget(a_req.left->node, a_req.left->front, a_req.left->offset, facing));
				if (a_req.right)
					trackRight = RL::TrackingFor(*calRight, kRestHandRight, WorldTarget(a_req.right->node, a_req.right->front, a_req.right->offset, facing));

				RL::Pose hmd = RL::Upright(kDefaultHmd);
				if (a_req.head.aim != RL::HeadTarget::Aim::kNone) {
					RL::Vec3 aim{};
					if (a_req.head.aim == RL::HeadTarget::Aim::kHands) {
						if (a_req.left && a_req.right)
							aim = RL::Scale(RL::Add(trackLeft, trackRight), 0.5);
						else if (a_req.right)
							aim = trackRight;
						else
							aim = trackLeft;
					} else {
						const bool             useRight = a_req.right.has_value();
						const RL::Calibration& cal = useRight ? *calRight : *calLeft;
						const RL::Vec3         rest = useRight ? kRestHandRight : kRestHandLeft;
						aim = RL::TrackingFor(cal, rest, ReadNodeWorld(a_req.head.node));
					}
					RL::Vec3 pos = a_req.head.position ? *a_req.head.position : RL::Add(aim, RL::Vec3{ 0.0, a_req.head.up, a_req.head.back });
					if (a_req.head.eyeHeight)
						pos.y = *a_req.head.eyeHeight;
					hmd = RL::LookAt(pos, aim);
				}

				SetPhase(a_run, RL::Phase::kReaching);

				const int iterations = std::max(1, a_req.iterations);
				for (int attempt = 1; attempt <= iterations; ++attempt) {
					const bool last = (attempt == iterations);
					const long hold = last ? a_req.holdMs : kShortHoldMs;

					RL::Vec3 startLeft = trackLeft;
					RL::Vec3 startRight = trackRight;
					if (attempt == 1) {
						const RL::Vec3 back = RL::Scale(facing, -a_req.approachUnits);
						if (a_req.left) {
							if (const auto d = RL::TrackingDelta(*calLeft, back))
								startLeft = RL::Add(trackLeft, *d);
						}
						if (a_req.right) {
							if (const auto d = RL::TrackingDelta(*calRight, back))
								startRight = RL::Add(trackRight, *d);
						}
					}

					guard.Start(RL::ApproachAndHoldFrames(hmd, startLeft, trackLeft, startRight, trackRight, hold), kSequenceTailMs);
					auto sequenceEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds(RL::ApproachAndHoldDurationMs(hold));
					SleepChecked(a_run, kIterationSettleMs);

					const auto left = MeasureHand(a_req.left, kLeftHandNode, facing);
					const auto right = MeasureHand(a_req.right, kRightHandNode, facing);
					double     worst = 0.0;
					if (left)
						worst = std::max(worst, RL::Norm(left->error));
					if (right)
						worst = std::max(worst, RL::Norm(right->error));

					{
						std::lock_guard lock(g_mutex);
						a_run->progress.iteration = attempt;
						a_run->progress.left = left;
						a_run->progress.right = right;
					}

					if (RL::Converged(worst, a_req.toleranceUnits)) {
						if (!last) {
							guard.Stop();
							SleepChecked(a_run, kBetweenIterationsMs);
							guard.Start(RL::ApproachAndHoldFrames(hmd, trackLeft, trackLeft, trackRight, trackRight, a_req.holdMs), kSequenceTailMs);
							sequenceEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds(RL::ApproachAndHoldDurationMs(a_req.holdMs));
						}
						SetPhase(a_run, RL::Phase::kHolding);
						SleepUntil(a_run, sequenceEnd);
						SetPhase(a_run, RL::Phase::kDone);
						return;
					}

					if (!last) {
						if (left) {
							if (const auto d = RL::TrackingDelta(*calLeft, left->error))
								trackLeft = RL::Add(trackLeft, *d);
						}
						if (right) {
							if (const auto d = RL::TrackingDelta(*calRight, right->error))
								trackRight = RL::Add(trackRight, *d);
						}
						guard.Stop();
						SleepChecked(a_run, kBetweenIterationsMs);
						continue;
					}

					SetPhase(a_run, RL::Phase::kHolding);
					SleepUntil(a_run, sequenceEnd);
					SetError(a_run, std::format("did not reach tolerance after {} iterations", iterations));
					SetPhase(a_run, RL::Phase::kDone);
					return;
				}
			} catch (const StopRequested&) {
				guard.Stop();
				SetPhase(a_run, RL::Phase::kStopped);
			} catch (const ToolError& e) {
				guard.Stop();
				SetError(a_run, e.what());
				SetPhase(a_run, RL::Phase::kFailed);
			} catch (const std::exception& e) {
				guard.Stop();
				SetError(a_run, e.what());
				SetPhase(a_run, RL::Phase::kFailed);
			} catch (...) {
				guard.Stop();
				SetError(a_run, "unknown error");
				SetPhase(a_run, RL::Phase::kFailed);
			}
		}
	}

	json Start(const json& a_args, const std::string& a_owner, const ToolContext& a_ctx)
	{
		std::string error;
		const auto  parsed = RL::ParseReach(a_args, error);
		if (!parsed)
			throw ToolError(400, std::format("reach: {}", error));

		const json status = dvb::HandleVRInput(json{ { "action", "status" } }, a_ctx);
		if (!status.value("ready", false))
			throw ToolError(503, "OpenVR is not ready");

		std::shared_ptr<Run> run;
		{
			std::lock_guard lock(g_mutex);
			for (const auto& existing : g_runs) {
				if (existing->active.load())
					throw ToolError(409, "a reach run is already active");
			}
			run = std::make_shared<Run>();
			run->runId = g_nextRunId++;
			run->owner = a_owner;
			g_runs.push_back(run);
			while (g_runs.size() > kMaxRuns)
				g_runs.erase(g_runs.begin());
		}

		const long        runId = run->runId;
		const std::string owner = run->owner;
		ToolContext       ctx = a_ctx;
		try {
			std::thread([run, request = *parsed, ctx]() { RunWorker(run, request, ctx); }).detach();
		} catch (...) {
			run->active.store(false);
			throw;
		}

		return json{
			{ "queued", true },
			{ "runId", runId },
			{ "owner", owner },
			{ "contract", "devbench.input.vrTrackedSet" },
		};
	}

	json Status(const json& a_args)
	{
		std::optional<long> runId;
		if (a_args.contains("runId") && !a_args["runId"].is_null()) {
			if (!a_args["runId"].is_number_integer())
				throw ToolError(400, "'runId' must be an integer");
			runId = a_args["runId"].get<long>();
		}

		std::lock_guard      lock(g_mutex);
		std::shared_ptr<Run> run;
		if (runId) {
			for (const auto& candidate : g_runs) {
				if (candidate->runId == *runId) {
					run = candidate;
					break;
				}
			}
			if (!run)
				throw ToolError(404, std::format("no reach run with runId {}", *runId));
		} else {
			if (g_runs.empty())
				throw ToolError(404, "no reach run");
			run = g_runs.back();
		}

		json result = RL::ProgressJson(run->progress);
		result["runId"] = run->runId;
		result["owner"] = run->owner;
		const bool active = run->active.load();
		result["active"] = active;
		result["stopping"] = active && run->stop.load();
		return result;
	}

	json Stop(const std::string& a_owner)
	{
		std::lock_guard      lock(g_mutex);
		std::shared_ptr<Run> active;
		for (const auto& candidate : g_runs) {
			if (candidate->active.load())
				active = candidate;
		}
		if (!active)
			return json{ { "stopped", false }, { "notActive", true } };
		if (active->owner != a_owner)
			throw ToolError(409, std::format("reach run {} is owned by '{}'", active->runId, active->owner));
		active->stop.store(true);
		return json{
			{ "stopped", true },
			{ "runId", active->runId },
			{ "owner", active->owner },
		};
	}

	bool Active()
	{
		std::lock_guard lock(g_mutex);
		for (const auto& candidate : g_runs) {
			if (candidate->active.load())
				return true;
		}
		return false;
	}

	void EndSession()
	{
		std::lock_guard lock(g_mutex);
		for (const auto& candidate : g_runs) {
			if (candidate->active.load())
				candidate->stop.store(true);
		}
	}
}
