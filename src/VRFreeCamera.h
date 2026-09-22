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

	/// A replay's hold on the free camera: activated once its recorded trajectory begins (not
	/// during scene setup/restore, so a content-mismatch modal or the scene assert still run
	/// against the normal camera) and released on any exit path. No-op outside VR. Both
	/// Activate() and the destructor marshal to the main thread themselves.
	class ReplayHold
	{
	public:
		ReplayHold() = default;
		~ReplayHold();
		ReplayHold(const ReplayHold&) = delete;
		ReplayHold& operator=(const ReplayHold&) = delete;

		/// Throws ToolError if activation fails (e.g. the free camera is owned elsewhere); the
		/// caller decides whether that aborts the replay or is logged and continued without
		/// camera-drive. No-op if already active or outside VR.
		void Activate();

	private:
		bool         m_active = false;
		SessionToken m_session = 0;
	};
}
