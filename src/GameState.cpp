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
		// The engine main loop writes this global while we read it off the listener thread;
		// atomic_ref makes that read well-defined (a raw *counter is a formal data race the
		// compiler may cache/hoist).
		return counter ? std::atomic_ref<int32_t>(*counter).load(std::memory_order_relaxed) : -1;
	}
}
