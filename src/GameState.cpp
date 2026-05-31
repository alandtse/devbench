#include "GameState.h"

#include <RE/Skyrim.h>

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
		return counter ? *counter : -1;
	}
}
