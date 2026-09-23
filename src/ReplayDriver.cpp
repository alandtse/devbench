#include "ReplayDriver.h"

#include "FreeCamera.h"
#include "GameClock.h"
#include "GameState.h"
#include "TimeScaleControl.h"
#include "ToolRegistry.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace dvb::Recording::ReplayDriver
{
	namespace
	{
		constexpr double kDegToRad = 0.017453292519943295;
		// Eye height as a fraction of standing height.
		constexpr float kEyeHeightRatio = 0.93F;
		constexpr auto  kPacerRetryDelay = std::chrono::milliseconds(1);
		constexpr auto  kFinalPoseWait = std::chrono::milliseconds(500);
		// A wait re-checks the game clock this often, so a mid-wait scale change is felt promptly
		// without spinning.
		constexpr auto kSleepSlice = std::chrono::milliseconds(5);

		class State : public std::enable_shared_from_this<State>
		{
		public:
			explicit State(Trajectory a_trajectory) :
				m_trajectory(std::move(a_trajectory)), m_startGameMs(GameClock::Now())
			{
				GameClock::Engage();
			}

			void Schedule()
			{
				if (auto* task = SKSE::GetTaskInterface())
					task->AddTask([self = shared_from_this()]() { self->Tick(); });
			}

			void StartPacer()
			{
				m_pacer = std::thread([this]() {
					for (;;) {
						{
							std::unique_lock lock(m_pacerMutex);
							m_pacerCv.wait(lock, [this]() { return m_pacerWake || m_cancelled.load(); });
							if (m_cancelled.load())
								return;
							m_pacerWake = false;
						}
						std::this_thread::sleep_for(kPacerRetryDelay);
						if (!m_cancelled.load())
							Schedule();
					}
				});
			}

			// Must run before the last shared_ptr is released: the pacer thread borrows `this`.
			void Stop()
			{
				if (m_stopped.exchange(true))
					return;
				{
					std::lock_guard lock(m_pacerMutex);
					m_cancelled.store(true);
				}
				m_pacerCv.notify_all();
				if (m_pacer.joinable())
					m_pacer.join();
				GameClock::Disengage();
			}

			bool WaitFinished(std::chrono::milliseconds a_timeout)
			{
				std::unique_lock lock(m_statsMutex);
				return m_finishedCv.wait_for(lock, a_timeout, [this]() { return m_finished; });
			}

			[[nodiscard]] json Stats() const
			{
				std::lock_guard lock(m_statsMutex);
				return json{ { "applied", m_applied }, { "skippedNoPlayer", m_skippedNoPlayer },
					{ "framesSpanned", m_framesSpanned }, { "maxFrameGap", m_maxFrameGap },
					{ "sameFrameRequeues", m_sameFrameRequeues }, { "finished", m_finished } };
			}

		private:
			void Tick()
			{
				if (m_cancelled.load(std::memory_order_relaxed))
					return;

				// The driver's own once-per-frame hook is the clock's frame tick, so no extra
				// engine hook is needed while a trajectory runs.
				GameClock::Tick();

				const int frame = game::CurrentFrame();
				if (frame == m_lastFrame) {
					// The queue ran us again inside the frame we already served; retry shortly
					// rather than spinning the main thread.
					{
						std::lock_guard lock(m_statsMutex);
						++m_sameFrameRequeues;
					}
					{
						std::lock_guard lock(m_pacerMutex);
						m_pacerWake = true;
					}
					m_pacerCv.notify_one();
					return;
				}

				const double elapsedMs = GameClock::Now() - m_startGameMs;
				const double tMs = static_cast<double>(m_trajectory.StartMs()) + elapsedMs;
				const bool   finishing = tMs >= static_cast<double>(m_trajectory.EndMs());
				const Pose   pose = m_trajectory.Sample(tMs);
				const bool   applied = Apply(pose);
				if (applied)
					DriveCamera(pose);

				{
					std::lock_guard lock(m_statsMutex);
					if (applied) {
						++m_applied;
						if (m_lastAppliedFrame >= 0) {
							const int gap = frame - m_lastAppliedFrame;
							m_framesSpanned += gap;
							m_maxFrameGap = std::max(m_maxFrameGap, gap);
						}
						m_lastAppliedFrame = frame;
					} else {
						++m_skippedNoPlayer;
					}
					m_finished = finishing;
				}
				if (finishing)
					m_finishedCv.notify_all();
				m_lastFrame = frame;
				if (!finishing)
					Schedule();
			}

			static bool Apply(const Pose& a_pose)
			{
				auto* player = RE::PlayerCharacter::GetSingleton();
				if (!player || !player->Get3D())
					return false;
				player->SetPosition(RE::NiPoint3(static_cast<float>(a_pose.x), static_cast<float>(a_pose.y),
										static_cast<float>(a_pose.z)),
					true);
				player->SetHeading(static_cast<float>(a_pose.yawDeg * kDegToRad));
				player->SetLooking(static_cast<float>(a_pose.pitchDeg * kDegToRad));
				return true;
			}

			// GetHeight() is the actor's bounding-box height, in the same units as position;
			// cached per replay since it doesn't change while standing/moving upright.
			float HeadHeightOffset()
			{
				if (m_headHeightOffset)
					return *m_headHeightOffset;
				auto* player = RE::PlayerCharacter::GetSingleton();
				m_headHeightOffset = player ? player->GetHeight() * kEyeHeightRatio : 0.0F;
				return *m_headHeightOffset;
			}

			// Drives the free camera every frame from the same interpolated pose, using the
			// recording's own captured camera transform when present, else one derived from the
			// player's pose.
			void DriveCamera(const Pose& a_pose)
			{
				try {
					if (a_pose.HasCam()) {
						FreeCamera::Drive(static_cast<float>(*a_pose.camX), static_cast<float>(*a_pose.camY),
							static_cast<float>(*a_pose.camZ), static_cast<float>(*a_pose.camPitch),
							static_cast<float>(*a_pose.camYaw), FreeCamera::CurrentSession());
						return;
					}
					FreeCamera::Drive(static_cast<float>(a_pose.x), static_cast<float>(a_pose.y),
						static_cast<float>(a_pose.z) + HeadHeightOffset(),
						static_cast<float>(a_pose.pitchDeg * kDegToRad), static_cast<float>(a_pose.yawDeg * kDegToRad),
						FreeCamera::CurrentSession());
				} catch (const std::exception& e) {
					// The hold may not have activated yet for this exact frame, or the free camera
					// was taken by another owner; either way the next frame retries on its own.
					logs::warn("devbench: replay camera drive skipped this frame: {}", e.what());
				}
			}

			Trajectory              m_trajectory;
			double                  m_startGameMs;
			std::atomic<bool>       m_cancelled{ false };
			std::atomic<bool>       m_stopped{ false };
			std::mutex              m_pacerMutex;
			std::condition_variable m_pacerCv;
			bool                    m_pacerWake = false;
			std::thread             m_pacer;
			int                     m_lastFrame = -1;
			int                     m_lastAppliedFrame = -1;
			mutable std::mutex      m_statsMutex;
			std::condition_variable m_finishedCv;
			std::uint64_t           m_applied = 0;
			std::uint64_t           m_skippedNoPlayer = 0;
			std::uint64_t           m_framesSpanned = 0;
			int                     m_maxFrameGap = 0;
			std::uint64_t           m_sameFrameRequeues = 0;
			bool                    m_finished = false;
			std::optional<float>    m_headHeightOffset;
		};

		class SessionImpl final : public Session
		{
		public:
			explicit SessionImpl(std::shared_ptr<State> a_state) :
				m_state(std::move(a_state)) {}
			~SessionImpl() override { m_state->Stop(); }
			[[nodiscard]] json Stats() const override { return m_state->Stats(); }
			bool               WaitFinished(std::chrono::milliseconds a_timeout) override { return m_state->WaitFinished(a_timeout); }

		private:
			std::shared_ptr<State> m_state;
		};
	}

	std::unique_ptr<Session> Start(Trajectory a_trajectory)
	{
		if (game::CurrentFrame() < 0)
			return nullptr;
		auto state = std::make_shared<State>(std::move(a_trajectory));
		state->StartPacer();
		state->Schedule();
		return std::make_unique<SessionImpl>(std::move(state));
	}

	Playback::Playback(const json& a_steps, bool a_enabled) :
		m_steps(a_steps), m_enabled(a_enabled)
	{
		if (!m_enabled)
			return;
		for (const auto& step : m_steps)
			if (step.contains("pose") && !IsValidPose(step["pose"]))
				throw ToolError(400, "pose step needs [x, y, z, yawDeg, pitchDeg]");
	}

	bool Playback::Handle(const json& a_step)
	{
		if (!m_enabled || m_unavailable || !a_step.contains("pose"))
			return false;
		if (!m_session) {
			m_session = Start(Trajectory(ExtractKeyframes(m_steps)));
			if (!m_session) {
				m_unavailable = true;
				logs::warn("devbench: pose driver unavailable (engine frame counter unreadable); using per-sample teleports");
				return false;
			}
			m_deadlineGameMs = GameClock::Now();
		}
		return true;
	}

	bool Playback::Sleep(long a_ms)
	{
		if (a_ms <= 0)
			return true;
		// Only accumulate without drift while the pose session is actively running (back-to-back
		// waits driving smooth playback); a setup/settle wait before it starts should anchor to
		// now, not a deadline left stale by whatever untracked work happened before this call.
		if (!m_session || !m_deadlineGameMs)
			m_deadlineGameMs = GameClock::Now();
		*m_deadlineGameMs += static_cast<double>(a_ms);
		GameClock::Engaged engaged;
		// Wall-clock backstop: bounds a genuinely stalled main thread (frame counter not
		// advancing) rather than blocking this worker forever; full cooperative cancellation
		// through Finish()/Stop() is a larger, separate change.
		const auto wallDeadline = std::chrono::steady_clock::now() +
		                          std::chrono::milliseconds(static_cast<long long>(a_ms / TimeScaleControl::kMinScale) + 5000);
		while (GameClock::Now() < *m_deadlineGameMs && std::chrono::steady_clock::now() < wallDeadline)
			std::this_thread::sleep_for(kSleepSlice);
		return GameClock::Now() >= *m_deadlineGameMs;
	}

	void Playback::Finish()
	{
		if (!m_session)
			return;
		m_session->WaitFinished(kFinalPoseWait);
		m_stats = m_session->Stats();
		m_session.reset();
		m_deadlineGameMs.reset();
	}
}
