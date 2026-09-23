#include "GameClock.h"

#include "GameState.h"
#include "TimeScaleControl.h"

#include <RE/B/BSTimer.h>
#include <SKSE/SKSE.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace dvb::GameClock
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		std::atomic<double> g_gameMs{ 0.0 };
		std::atomic<int>    g_engaged{ 0 };
		std::atomic<bool>   g_rebase{ false };

		std::mutex              g_pumpMutex;
		std::condition_variable g_pumpCv;
		std::once_flag          g_pumpOnce;

		// Main thread only (Tick), so no synchronization beyond the rebase flag.
		Clock::time_point g_lastWall{};
		int               g_lastFrame = -1;

		bool SameFrame(int a_frame)
		{
			// An unreadable frame counter cannot gate anything — integrate every tick instead.
			return a_frame >= 0 && a_frame == g_lastFrame;
		}

		// Wakes while the clock is engaged and posts a frame-gated tick. Blocked on the CV (no
		// CPU) whenever nothing is waiting on the clock.
		void Pump()
		{
			for (;;) {
				{
					std::unique_lock lock(g_pumpMutex);
					g_pumpCv.wait(lock, [] { return g_engaged.load(std::memory_order_acquire) > 0; });
				}
				while (g_engaged.load(std::memory_order_acquire) > 0) {
					std::this_thread::sleep_for(std::chrono::milliseconds(kTickPumpMs));
					if (auto* task = SKSE::GetTaskInterface())
						task->AddTask([] { Tick(); });
				}
			}
		}
	}

	double Now()
	{
		return g_gameMs.load(std::memory_order_acquire);
	}

	void Tick()
	{
		const int  frame = game::CurrentFrame();
		const auto now = Clock::now();
		if (g_rebase.exchange(false, std::memory_order_acq_rel)) {
			// The clock was idle: start from here so the idle gap is not counted as play.
			g_lastWall = now;
			g_lastFrame = frame;
			return;
		}
		if (SameFrame(frame))
			return;
		const double wallDeltaMs = std::chrono::duration<double, std::milli>(now - g_lastWall).count();
		g_lastWall = now;
		g_lastFrame = frame;

		const Integrator next = Integrate(Integrator{ g_gameMs.load(std::memory_order_relaxed) },
			wallDeltaMs, static_cast<double>(RE::BSTimer::QGlobalTimeMultiplier()), kMaxFrameDeltaMs);
		g_gameMs.store(next.gameMs, std::memory_order_release);

		TimeScaleControl::Reconcile();
	}

	void Engage()
	{
		if (g_engaged.fetch_add(1, std::memory_order_acq_rel) == 0) {
			g_rebase.store(true, std::memory_order_release);
			std::call_once(g_pumpOnce, [] { std::thread(&Pump).detach(); });
			// Pump's wait(lock, pred) checks the predicate and starts blocking under
			// g_pumpMutex; taking the same lock here before notifying closes the window where
			// this g_engaged transition and the notify could both land before Pump blocks.
			{
				std::lock_guard lock(g_pumpMutex);
			}
			g_pumpCv.notify_all();
		}
	}

	void Disengage()
	{
		int engaged = g_engaged.load(std::memory_order_acquire);
		while (engaged > 0 && !g_engaged.compare_exchange_weak(engaged, engaged - 1,
								  std::memory_order_acq_rel, std::memory_order_acquire)) {
		}
	}

	void SleepMs(long a_gameMs)
	{
		if (a_gameMs <= 0)
			return;
		Engaged      engaged;
		const double deadline = Now() + static_cast<double>(a_gameMs);
		// Wall-clock backstop: game time can legitimately run up to 10x slower than real time
		// (TimeScaleControl::kMinScale), so a genuinely frozen/stalled main thread — not just a
		// slow one — is the only thing this bound should ever catch.
		const auto wallDeadline = Clock::now() +
		                          std::chrono::milliseconds(static_cast<long long>(a_gameMs / TimeScaleControl::kMinScale) + 5000);
		while (Now() < deadline && Clock::now() < wallDeadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(kTickPumpMs));
	}
}
