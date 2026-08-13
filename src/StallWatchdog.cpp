#include "StallWatchdog.h"

#include "EventBus.h"
#include "GameEvents.h"
#include "GameState.h"
#include "Json.h"

#include <chrono>
#include <thread>

namespace dvb::StallWatchdog
{
	namespace
	{
		using namespace std::chrono;
		constexpr int kPollMs = 1000;
	}

	void Start(EventBus& a_bus, int a_stallMs)
	{
		if (a_stallMs <= 0)
			return;
		EventBus* bus = &a_bus;
		std::thread([bus, a_stallMs]() {
			int  lastFrame = game::CurrentFrame();
			auto lastAdvance = steady_clock::now();
			bool stalled = false;
			for (;;) {
				std::this_thread::sleep_for(milliseconds(kPollMs));
				const int frame = game::CurrentFrame();
				if (frame < 0)
					continue;  // address library not resolved yet -- nothing to watch
				const auto now = steady_clock::now();
				if (frame != lastFrame) {
					if (stalled) {
						const long long stalledForMs = duration_cast<milliseconds>(now - lastAdvance).count();
						bus->Publish("health.resumed", json{ { "frame", frame }, { "stalledForMs", stalledForMs } });
						stalled = false;
					}
					lastFrame = frame;
					lastAdvance = now;
					continue;
				}
				if (stalled)
					continue;  // already reported; wait for it to resolve
				const long long sinceAdvance = duration_cast<milliseconds>(now - lastAdvance).count();
				if (sinceAdvance < a_stallMs)
					continue;
				stalled = true;
				json j{ { "frame", frame }, { "stalledForMs", sinceAdvance } };
				// Best-effort context: read from the live tracked set (no main-thread marshal),
				// so this still reports something even though the stall itself means RE::UI
				// can't be safely queried right now.
				if (auto menus = GetOpenMenus(); !menus.empty())
					j["lastKnownOpenMenus"] = std::move(menus);
				bus->Publish("health.stalled", j);
			}
		}).detach();
	}
}
