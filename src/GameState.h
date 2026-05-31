#pragma once

namespace dvb::game
{
	// The game's global frame counter (read-only, hook-free). devbench takes no per-frame
	// hook by design, so this reads the same engine global CS uses
	// (REL::RelocationID(525008, 411489)) — a plain int incremented by the main loop. Stamped
	// onto events / recording samples / inspect so they can be synced to a Tracy or CS capture.
	// Returns -1 if the address can't be resolved (e.g. id absent on this runtime).
	int CurrentFrame();
}
