#include "TimeScaleControl.h"

#include "Capture.h"
#include "GameClock.h"
#include "MainThread.h"
#include "Recording.h"
#include "ToolRegistry.h"

#include <RE/B/BSTimer.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <vector>

namespace dvb::TimeScaleControl
{
	namespace
	{
		struct Change
		{
			double gameMs;
			float  effective;
		};

		std::mutex          g_mutex;
		Reconciler          g_reconciler;
		bool                g_leaseEngaged = false;
		bool                g_runActive = false;
		std::string         g_runOwner;
		std::vector<Change> g_changes;

		// Written by the main-thread reconciler, read by any thread.
		std::atomic<float> g_liveMultiplier{ static_cast<float>(kNormalScale) };

		std::int64_t NowWallMs()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch())
			    .count();
		}

		// Called with g_mutex held. Returns the engagement change to apply after unlocking, so the
		// pump keeps running exactly while a lease is outstanding.
		std::optional<bool> LatchEngagement(bool a_leased)
		{
			if (g_leaseEngaged == a_leased)
				return std::nullopt;
			g_leaseEngaged = a_leased;
			return a_leased;
		}

		void ApplyEngagement(const std::optional<bool>& a_engagement)
		{
			if (!a_engagement)
				return;
			if (*a_engagement)
				GameClock::Engage();
			else
				GameClock::Disengage();
		}

		// True until the ENGINE itself (not our own bookkeeping) reports the requested value —
		// Reconciler::Pending() clears too early, mid-ramp, and would stop the pump prematurely.
		bool NeedsPump(std::int64_t a_now)
		{
			return g_reconciler.Leased(a_now) ||
			       std::fabs(g_liveMultiplier.load(std::memory_order_acquire) - g_reconciler.Requested()) >
			           kEffectiveTolerance;
		}
	}

	std::mutex& AdmissionMutex() { return g_mutex; }

	SetResult Set(float a_scale, std::int64_t a_holdMs, const std::string& a_owner,
		bool a_allowTimeScale)
	{
		const std::int64_t holdMs = a_holdMs > 0 ? std::min<std::int64_t>(a_holdMs, kMaximumLeaseMs) : kDefaultLeaseMs;

		// The cached Effective() can be stale until the pump has run at least once (e.g. an
		// external console change before any devbench lease ever engaged the clock), which would
		// make Resync below compare the cache against itself and see no drift. Sample the engine
		// directly on the main thread first; on timeout (main thread stalled) fall back to the
		// cached value rather than fail Set() outright.
		float liveNow = Effective();
		try {
			const json sampled = MainThread::RunAndWait([]() -> json {
				return json{ { "live", static_cast<double>(RE::BSTimer::QGlobalTimeMultiplier()) } };
			},
				std::chrono::milliseconds(2000));
			liveNow = static_cast<float>(sampled.value("live", static_cast<double>(liveNow)));
		} catch (const std::exception&) {
		}

		std::optional<bool> engagement;
		{
			// The admission check and the reservation it gates must be atomic with respect to
			// Recording::start/Capture::Handle's own check-and-commit (same mutex) — otherwise a
			// recording/capture could start in the gap between this check and g_reconciler.Request.
			std::lock_guard lock(g_mutex);
			if (a_scale != static_cast<float>(kNormalScale) && !a_allowTimeScale) {
				if (Recording::IsActive())
					return { false, "a recording is in progress — stop it first, or pass allowTimeScale:true to change the game's speed anyway" };
				if (Capture::InFlight())
					return { false, "a capture is in flight — retry once it finishes, or pass allowTimeScale:true to change the game's speed anyway" };
			}
			const std::int64_t now = NowWallMs();
			g_liveMultiplier.store(liveNow, std::memory_order_release);
			if (a_scale == static_cast<float>(kNormalScale))
				// Request kNormalScale directly rather than Release()'s lease-restore baseline:
				// an explicit "set scale to 1" means exactly that, not "whatever it was before
				// devbench's current hold started".
				g_reconciler.Request(static_cast<float>(kNormalScale), {}, 0, liveNow);
			else
				g_reconciler.Request(a_scale, a_owner, now + holdMs, liveNow);
			// Catches a scale that drifted externally (console sgtm, another mod) while our own
			// bookkeeping still matches the new request, which would otherwise make Reconcile
			// think there's nothing to write.
			g_reconciler.Resync(liveNow);
			engagement = LatchEngagement(NeedsPump(now));
		}
		ApplyEngagement(engagement);
		return { true, {} };
	}

	void Reconcile()
	{
		const std::int64_t   now = NowWallMs();
		std::optional<float> value;
		std::optional<bool>  engagement;
		{
			std::lock_guard lock(g_mutex);
			const float     previousLive = g_liveMultiplier.load(std::memory_order_relaxed);
			const float     live = RE::BSTimer::QGlobalTimeMultiplier();
			g_liveMultiplier.store(live, std::memory_order_release);
			// A run in flight keeps its own lease alive, so a long one never expires mid-run.
			if (g_runActive)
				g_reconciler.RenewLease(g_runOwner, now + kDefaultLeaseMs);
			value = g_reconciler.Reconcile(now);
			if (g_runActive) {
				if (value)
					g_changes.push_back({ GameClock::Now(), *value });
				else if (std::fabs(live - previousLive) > kEffectiveTolerance)
					// Nothing we issued moved the engine — an external sgtm/console change did, so
					// the run must still be flagged ineligible for a golden comparison.
					g_changes.push_back({ GameClock::Now(), live });
			}
			engagement = LatchEngagement(NeedsPump(now));
		}
		ApplyEngagement(engagement);
		if (!value)
			return;
		if (auto* timer = RE::BSTimer::GetSingleton())
			timer->SetGlobalTimeMultiplier(*value, false);
	}

	float Effective()
	{
		return g_liveMultiplier.load(std::memory_order_acquire);
	}

	json Status()
	{
		const std::int64_t now = NowWallMs();
		std::lock_guard    lock(g_mutex);
		return json{
			{ "requested", g_reconciler.Requested() },
			{ "effective", Effective() },
			{ "owner", g_reconciler.Owner() },
			{ "leased", g_reconciler.Leased(now) },
			{ "leaseRemainingMs", g_reconciler.LeaseRemainingMs(now) },
		};
	}

	RunHold::RunHold(float a_scale, std::int64_t a_holdMs, const std::string& a_owner,
		bool a_allowTimeScale) :
		m_owner(a_owner)
	{
		// A throw here must happen before anything below is touched: a constructor that throws
		// never runs its own destructor, so a refusal after Engage()/g_runActive would leak them.
		if (a_scale != static_cast<float>(kNormalScale)) {
			const SetResult set = Set(a_scale, a_holdMs, a_owner, a_allowTimeScale);
			if (!set.ok)
				throw ToolError(409, std::format("time scale refused: {}", set.error));
			m_restore = true;
		}
		GameClock::Engage();
		const float startEffective = Effective();
		{
			std::lock_guard lock(g_mutex);
			g_runActive = true;
			g_runOwner = a_owner;
			g_changes.clear();
			// A run that starts non-normal is already incomparable to a golden, so record why.
			if (std::fabs(startEffective - static_cast<float>(kNormalScale)) > kEffectiveTolerance)
				g_changes.push_back({ GameClock::Now(), startEffective });
		}
		m_open = true;  // every path from here out is covered by the destructor
	}

	RunHold::~RunHold()
	{
		if (!m_open)
			return;
		std::optional<bool> engagement;
		{
			std::lock_guard lock(g_mutex);
			g_runActive = false;
			g_runOwner.clear();
			if (m_restore)
				g_reconciler.Release(m_owner);
			// NeedsPump, not just Leased: keeps pumping until the engine actually converges.
			const std::int64_t now = NowWallMs();
			engagement = LatchEngagement(NeedsPump(now));
		}
		ApplyEngagement(engagement);
		GameClock::Disengage();
	}

	void RunHold::Escalate(float a_scale, std::int64_t a_holdMs, bool a_allowTimeScale)
	{
		if (a_scale == static_cast<float>(kNormalScale))
			return;
		const SetResult set = Set(a_scale, a_holdMs, m_owner, a_allowTimeScale);
		if (!set.ok)
			throw ToolError(409, std::format("time scale refused: {}", set.error));
		m_restore = true;
	}

	bool RunHold::Eligible() const
	{
		std::lock_guard lock(g_mutex);
		return g_changes.empty();
	}

	json RunHold::Changes() const
	{
		std::lock_guard lock(g_mutex);
		json            out = json::array();
		for (const auto& change : g_changes)
			out.push_back(json{ { "gameMs", change.gameMs }, { "effective", change.effective } });
		return out;
	}
}
