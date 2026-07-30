#include "GameState.h"

#include <RE/Skyrim.h>

#include <atomic>

namespace dvb::game
{
	int CurrentFrame()
	{
		// Resolve once (RelocationID lookup is not free); the address is stable for the
		// process. First call happens well after load, so the address library is ready.
		static int32_t* counter = []() -> int32_t* {
			try {
				return reinterpret_cast<int32_t*>(REL::RelocationID(525008, 411489).address());
			} catch (...) {
				return nullptr;
			}
		}();
		// The engine's main loop writes this global every frame; we read it off the
		// listener thread. Read through atomic_ref so the access is well-defined (not a
		// torn/hoisted read) rather than a raw *counter, which is a formal data race and
		// lets the compiler cache the value across calls.
		return counter ? std::atomic_ref<int32_t>(*counter).load(std::memory_order_relaxed) : -1;
	}
}
