#pragma once

#include <cmath>

namespace dvb::GameClock
{
	// A wall gap this large is a hitch or a loading screen, not elapsed play; clamping keeps one
	// stall from jumping the clock and every deadline measured against it.
	inline constexpr double kMaxFrameDeltaMs = 250.0;

	// Pump cadence for the frame-gated tick. Only affects resolution: the clock is integrated
	// from measured wall deltas, never from an assumed cadence.
	inline constexpr int kTickPumpMs = 4;

	struct Integrator
	{
		double gameMs = 0.0;
	};

	// Pure: advance a_state by one frame of play. a_multiplier is the engine's EFFECTIVE global
	// time multiplier (never a requested one), so a mid-run change, a ramp, or a scale the player
	// set by console is honored without the caller knowing about it.
	inline Integrator Integrate(const Integrator& a_state, double a_wallDeltaMs, double a_multiplier,
		double a_maxDeltaMs)
	{
		double delta = a_wallDeltaMs;
		if (!(delta > 0.0))  // also rejects NaN
			delta = 0.0;
		else if (delta > a_maxDeltaMs)
			delta = a_maxDeltaMs;
		double multiplier = a_multiplier;
		if (!std::isfinite(multiplier) || multiplier <= 0.0)
			multiplier = 0.0;  // a broken multiplier freezes the clock rather than corrupting it
		return { a_state.gameMs + delta * multiplier };
	}

	// Game ms accumulated so far, readable from any thread. Only differences are meaningful;
	// the origin is arbitrary.
	double Now();

	// Main thread, at most once per engine frame: integrates the elapsed wall delta scaled by the
	// engine's current multiplier, then reconciles any pending time-scale change.
	void Tick();

	// Keeps the tick pump running while any consumer is waiting on Now() (a replay, a scenario
	// wait, a held key, a time-scale lease). Refcounted; Engage on an idle clock rebases the
	// integrator so the idle wall gap is not counted as play.
	void Engage();
	void Disengage();

	// Engage/Disengage for the lifetime of a scope.
	struct Engaged
	{
		Engaged() { Engage(); }
		~Engaged() { Disengage(); }
		Engaged(const Engaged&) = delete;
		Engaged& operator=(const Engaged&) = delete;
	};

	// Blocks the calling thread for a_gameMs of GAME time (engaging the clock for the duration),
	// so a caller's own wait shrinks/grows with the run the same way replay pose/wait steps do.
	void SleepMs(long a_gameMs);
}
