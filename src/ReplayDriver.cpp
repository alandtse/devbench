#include "ReplayDriver.h"

#include "GameState.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace dvb::Recording::ReplayDriver
{
	namespace
	{
		constexpr double kDegToRad = 0.017453292519943295;

		using Clock = std::chrono::steady_clock;

		class State : public std::enable_shared_from_this<State>
		{
		public:
			explicit State(Trajectory a_trajectory) :
				m_trajectory(std::move(a_trajectory)), m_start(Clock::now()) {}

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
						std::this_thread::sleep_for(std::chrono::milliseconds(1));
						if (!m_cancelled.load())
							Schedule();
					}
				});
			}

			// Must run before the last shared_ptr is released: the pacer thread borrows `this`.
			void Stop()
			{
				{
					std::lock_guard lock(m_pacerMutex);
					m_cancelled.store(true);
				}
				m_pacerCv.notify_all();
				if (m_pacer.joinable())
					m_pacer.join();
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

				const double elapsedMs =
					std::chrono::duration<double, std::milli>(Clock::now() - m_start).count();
				const double tMs = static_cast<double>(m_trajectory.StartMs()) + elapsedMs;
				const bool   finishing = tMs >= static_cast<double>(m_trajectory.EndMs());
				const bool   applied = Apply(m_trajectory.Sample(tMs));

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

			Trajectory              m_trajectory;
			Clock::time_point       m_start;
			std::atomic<bool>       m_cancelled{ false };
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
}
