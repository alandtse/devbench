#pragma once

#include <cstdint>

namespace dvb::VRFreeCamera
{
	using SessionToken = std::uint64_t;

	/// Capture on the caller thread before queuing a camera mutation.
	SessionToken CurrentSession();

	/// Main-thread operations. Leave the engine's freeze-time flag unchanged.
	/// Freecam requests retry pending load recovery; unavailable or rejected recovery reports HTTP 500.
	void SetEnabled(bool a_enabled, SessionToken a_session);
	void Drive(float a_x, float a_y, float a_z, float a_pitch, float a_yaw, SessionToken a_session);
	/// Reconcile ownership with the current registered camera state on the main thread.
	bool IsOwned();

	/// Main-thread lifecycle: discard old pointers and invalidate queued mutations at load boundaries.
	/// Failed pre-load restoration recovers through the loaded scene's registered normal VR state.
	void BeginLoad();
	void EndLoad();
}
