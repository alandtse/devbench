#pragma once

#include "Json.h"

#include <cmath>
#include <cstdint>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace dvb::TimeScaleControl
{
	// The game's normal speed, and the value a lease restores to when nothing else was set.
	inline constexpr double kNormalScale = 1.0;
	// Slowest supported scale (further down is a freeze, which needs its own flag).
	inline constexpr double kMinScale = 0.1;
	// Normal ceiling. kHighMaxScale is the hard ceiling, reachable only with an explicit allowHigh.
	inline constexpr double kMaxScale = 3.0;
	inline constexpr double kHighMaxScale = 10.0;
	inline constexpr double kFreezeScale = 0.0;
	// How close the engine's live multiplier must get to a request before it counts as applied
	// (the engine ramps toward a new multiplier when bChangeTimeMultSlowly is on).
	inline constexpr float        kEffectiveTolerance = 0.01F;
	inline constexpr std::int64_t kDefaultLeaseMs = 60000;
	inline constexpr std::int64_t kMaximumLeaseMs = 3600000;

	struct Validation
	{
		bool        accepted = false;
		float       value = static_cast<float>(kNormalScale);
		std::string error;
	};

	// Pure: validate/clamp a requested scale. Rejects non-finite values, a freeze without its
	// flag, anything under kMinScale, and anything above kMaxScale/kHighMaxScale.
	inline Validation Validate(double a_scale, bool a_freeze, bool a_allowHigh)
	{
		const auto reject = [](std::string a_error) { return Validation{ false, static_cast<float>(kNormalScale), std::move(a_error) }; };
		if (!std::isfinite(a_scale))
			return reject("scale must be a finite number");
		if (a_freeze) {
			if (a_scale != kFreezeScale)
				return reject("freeze requires scale=0");
			return { true, static_cast<float>(kFreezeScale), {} };
		}
		if (a_scale == kFreezeScale)
			return reject("scale=0 freezes the game — pass freeze:true if that is intended");
		if (a_scale < kMinScale)
			return reject(std::format("scale must be at least {}", kMinScale));
		if (a_scale <= kMaxScale)
			return { true, static_cast<float>(a_scale), {} };
		if (a_scale > kHighMaxScale)
			return reject(std::format("scale must be at most {}", kHighMaxScale));
		if (!a_allowHigh)
			return reject(std::format("scale above {} needs allowHigh:true (hard maximum {})", kMaxScale, kHighMaxScale));
		return { true, static_cast<float>(a_scale), {} };
	}

	// Pure lease + reconcile state. Any thread may request; the main thread reconciles once per
	// engine frame. Never re-issues a value the engine already has, so a ramp set in motion by an
	// earlier write is not restarted.
	class Reconciler
	{
	public:
		// Sets a_value for a_owner until a_expiresAtWallMs (0 = no expiry). The first acquisition
		// captures the currently applied value as the baseline a lease end restores to; a later
		// request while that lease is live keeps the baseline. The very first request this
		// Reconciler ever sees also seeds m_applied from a_liveValue (the engine's actual
		// multiplier), not the kNormalScale default — the engine may already be off-normal from
		// an external console command before anyone ever called Set().
		void Request(float a_value, std::string a_owner, std::int64_t a_expiresAtWallMs,
			float a_liveValue = static_cast<float>(kNormalScale))
		{
			if (m_expiresAtWallMs == 0) {
				if (!m_seeded) {
					m_applied = a_liveValue;
					m_seeded = true;
				}
				m_restoreValue = m_applied;
			}
			m_requested = a_value;
			m_owner = std::move(a_owner);
			m_expiresAtWallMs = a_expiresAtWallMs;
		}

		// Extends a lease only while a_owner still holds it, so an ad-hoc override that displaced a
		// run cannot be extended by the run it displaced.
		void RenewLease(std::string_view a_owner, std::int64_t a_expiresAtWallMs)
		{
			if (m_expiresAtWallMs != 0 && m_owner == a_owner)
				m_expiresAtWallMs = a_expiresAtWallMs;
		}

		// Requests the baseline value and drops the lease: an empty a_owner drops whoever holds it
		// (an explicit return to normal speed), a named one only releases that holder's own lease.
		void Release(std::string_view a_owner = {})
		{
			if (!a_owner.empty() && m_owner != a_owner)
				return;
			m_expiresAtWallMs = 0;
			m_owner.clear();
			m_requested = m_restoreValue;
		}

		// Makes the next Reconcile() actually write to the engine when a_live has drifted from
		// what we last told it, even though our own m_applied bookkeeping still matches
		// m_requested — otherwise an external change (console sgtm, another mod) that happens
		// to match our default is invisible to Reconcile's "nothing changed" shortcut.
		void Resync(float a_live)
		{
			if (a_live != m_requested)
				m_applied = a_live;
		}

		// The value the engine must be given now, or nullopt when nothing has to change.
		std::optional<float> Reconcile(std::int64_t a_nowWallMs)
		{
			if (m_expiresAtWallMs != 0 && a_nowWallMs >= m_expiresAtWallMs) {
				m_expiresAtWallMs = 0;
				m_owner.clear();
				m_requested = m_restoreValue;
			}
			if (m_requested == m_applied)
				return std::nullopt;
			m_applied = m_requested;
			m_seeded = true;
			return m_applied;
		}

		float       Effective() const { return m_applied; }
		float       Requested() const { return m_requested; }
		std::string Owner() const { return m_owner; }
		bool        Leased(std::int64_t a_nowWallMs) const
		{
			return m_expiresAtWallMs != 0 && a_nowWallMs < m_expiresAtWallMs;
		}
		// True while the engine has not yet caught up to the requested value.
		bool         Pending() const { return m_requested != m_applied; }
		std::int64_t LeaseRemainingMs(std::int64_t a_nowWallMs) const
		{
			return m_expiresAtWallMs == 0 || a_nowWallMs >= m_expiresAtWallMs ? 0 : m_expiresAtWallMs - a_nowWallMs;
		}

	private:
		float        m_applied = static_cast<float>(kNormalScale);
		float        m_requested = static_cast<float>(kNormalScale);
		std::string  m_owner;
		float        m_restoreValue = static_cast<float>(kNormalScale);
		std::int64_t m_expiresAtWallMs = 0;
		bool         m_seeded = false;
	};

	// --- engine-facing (src/TimeScaleControl.cpp) ---

	struct SetResult
	{
		bool        ok = false;
		std::string error;
	};

	// Shared serialization point for the moment recording start, capture start, and a
	// non-normal time-scale request each decide whether to proceed against the others:
	// whichever locks it first excludes the rest until it commits its own state (or aborts).
	// Set() locks it internally; Recording/Capture hold it across their own admission check
	// + state transition so the two can never interleave.
	std::mutex& AdmissionMutex();

	// Any thread, non-blocking: the main-thread reconciler applies the request on the next engine
	// frame. Refuses a non-normal scale while a recording or a capture is in flight unless
	// a_allowTimeScale. a_holdMs <= 0 uses kDefaultLeaseMs.
	SetResult Set(float a_scale, std::int64_t a_holdMs, const std::string& a_owner,
		bool a_allowTimeScale);

	// Main thread, once per engine frame (driven by GameClock::Tick).
	void Reconcile();

	// The engine's live global time multiplier (last sampled frame) — what the game is actually
	// running at, which lags a request while the engine ramps toward it.
	float Effective();

	// { requested, effective, owner, leased, leaseRemainingMs }.
	json Status();

	// A run's hold on the scale: set for the run's duration and restored when the scope ends, on
	// any exit path. Logs every change the reconciler issues while it is open, so a run can report
	// whether its captures were taken at a comparable speed. Throws ToolError(409) if the request
	// is refused (a recording or capture in flight without a_allowTimeScale).
	class RunHold
	{
	public:
		RunHold(float a_scale, std::int64_t a_holdMs, const std::string& a_owner, bool a_allowTimeScale);
		~RunHold();
		RunHold(const RunHold&) = delete;
		RunHold& operator=(const RunHold&) = delete;
		RunHold(RunHold&&) = delete;
		RunHold& operator=(RunHold&&) = delete;

		// True when the run's whole window ran at the normal scale.
		bool Eligible() const;
		// [{ gameMs, effective }] — one entry per change issued while the run was open.
		json Changes() const;

		// Raises the hold to a_scale partway through an already-open run (e.g. once its setup/
		// settle phase ends and the recorded trajectory begins), instead of for the whole run.
		// Same refusal/restore semantics as the constructor: throws ToolError(409) if refused, and
		// whatever scale is current when this RunHold is destroyed is what gets released.
		void Escalate(float a_scale, std::int64_t a_holdMs, bool a_allowTimeScale);

	private:
		std::string m_owner;
		bool        m_open = false;
		bool        m_restore = false;
	};
}
