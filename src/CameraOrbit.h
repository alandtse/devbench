#pragma once

#include "CameraOrbitLogic.h"

#include <cstdint>

namespace dvb::CameraOrbit
{
	// Holds the gameplay third-person camera at an angle round the player (heading, tilt, zoom and offset re-applied on
	// every camera update) so a front or side view needs no free camera. The player keeps the gameplay input context,
	// so held input (a charging attack or spell) keeps acting on the player.
	//
	// Every change happens on the main thread. An orbit ends, writing back each field it overwrote, when asked to, when
	// the camera leaves the third-person state (any POV switch, the free camera), or when devbench turns the free camera
	// on or switches POV. A load or new game drops it without writing anything into the new scene.
	void Install();

	/// Any thread: the session a request is made in. Capture it before queuing Enable.
	std::uint64_t CurrentSession();

	/// Main thread. Refuses a request made before a load / new game, during a load, or while the free camera is on.
	Admission Enable(const Request& a_request, std::uint64_t a_session);
	/// Main thread. True if the orbit was on; it is off after this either way.
	bool End(EndReason a_reason);
	/// Main thread, at a load or new game.
	void EndSession();

	/// Main thread.
	const OrbitState& State();
}
