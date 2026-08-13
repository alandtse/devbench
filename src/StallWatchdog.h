#pragma once

namespace dvb
{
	class EventBus;

	/// Background watchdog for a frozen main thread — the case `menu`/`lifecycle` events can
	/// never cover, since those are published BY the main thread (nothing publishes if it's
	/// stalled). Samples the lock-free game frame counter off its own thread; if the frame
	/// hasn't advanced for `a_stallMs`, publishes "health.stalled" { frame, stalledForMs,
	/// lastKnownOpenMenus? } once, then "health.resumed" { frame, stalledForMs } once frame
	/// advances again. `a_stallMs <= 0` disables the watchdog (no-op). Call once, after the
	/// server's EventBus exists; the thread runs for the plugin's lifetime (detached — devbench
	/// never unloads mid-process, so there is no shutdown point to join from).
	namespace StallWatchdog
	{
		void Start(EventBus& a_bus, int a_stallMs);
	}
}
