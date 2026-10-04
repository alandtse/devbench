#pragma once

#include <cstdint>
#include <optional>

namespace dvb::CameraOrbit
{
	struct Vec3
	{
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
	};

	/// What a caller asked for. Unset optionals leave that part of the game's camera alone.
	struct Request
	{
		float                yawRad = 0.0f;  // heading round the player from its facing (pi = in front)
		std::optional<float> pitchRad;       // the player's look pitch, which the third-person tilt follows
		std::optional<float> zoom;           // the third-person zoom offset, [-1, 1]
		std::optional<Vec3>  offset;         // the camera's positional offset (right, 0, up)
	};

	/// The third-person camera fields an orbit writes.
	struct CameraFields
	{
		float targetYaw = 0.0f;
		float currentYaw = 0.0f;
		float targetZoom = 0.0f;
		float currentZoom = 0.0f;
		Vec3  offsetExpected;
		Vec3  offsetActual;
		float freeRotationX = 0.0f;  // heading offset from the player's facing, used while free rotation is on
	};

	/// The player and camera as they were when an orbit first applied, before any write.
	struct Baseline
	{
		float        playerYaw = 0.0f;
		float        playerPitch = 0.0f;
		CameraFields camera;
	};

	/// Which parts of the camera this orbit has overwritten so far.
	struct Overridden
	{
		bool pitch = false;
		bool zoom = false;
		bool offset = false;
	};

	enum class EndReason
	{
		kNone,
		kRequested,        // camera orbit on=false
		kLeftThirdPerson,  // the camera left the third-person state (POV switch, free camera, ...)
		kFreeCamera,       // devbench turned the free camera on
		kPov,              // camera setPov
		kSessionEnded,     // a load or new game; nothing is restored
	};
	const char* EndReasonName(EndReason a_reason);

	enum class Admission
	{
		kAccepted,
		kStaleSession,  // a load or new game happened after the request was made
		kLoading,
		kFreeCamera,
		kUnsupportedRuntime,  // VR: the third-person state's layout there is not known
	};

	/// One orbit's configuration, baseline and lifecycle. Every change bumps the revision, and every
	/// load or new game bumps the session, which invalidates requests made before it. Main thread
	/// only; Session() is mirrored to other threads by the caller.
	class OrbitState
	{
	public:
		[[nodiscard]] bool                           On() const { return m_on; }
		[[nodiscard]] std::uint64_t                  Session() const { return m_session; }
		[[nodiscard]] std::uint64_t                  Revision() const { return m_revision; }
		[[nodiscard]] const Request&                 Requested() const { return m_request; }
		[[nodiscard]] bool                           Applied() const { return m_baseline.has_value(); }
		[[nodiscard]] const Overridden&              OverriddenSoFar() const { return m_overridden; }
		[[nodiscard]] EndReason                      LastEnd() const { return m_lastEnd; }
		[[nodiscard]] const std::optional<Baseline>& CapturedBaseline() const { return m_baseline; }

		/// Starts or reconfigures the orbit. A reconfigured orbit keeps its first baseline.
		Admission Enable(const Request& a_request, std::uint64_t a_requestSession, bool a_loading, bool a_freeCamera);

		/// What one camera update writes, capturing a_current as the baseline on the first one.
		/// Empty when the orbit is off.
		struct Write
		{
			float                playerYaw = 0.0f;
			std::optional<float> playerPitch;
			CameraFields         camera;
		};
		std::optional<Write> Apply(const Baseline& a_current);

		/// Ends the orbit. Returns what to write back to undo it - the baseline's value for every
		/// field it overwrote, a_current's for the rest - or nothing if it never applied.
		struct Restore
		{
			std::optional<float> playerPitch;
			CameraFields         camera;
		};
		std::optional<Restore> End(EndReason a_reason, const CameraFields& a_current);

		/// A load or new game: drops the orbit and its baseline without restoring anything, since the
		/// old scene's camera and player are gone.
		void EndSession();

	private:
		Request                 m_request;
		std::optional<Baseline> m_baseline;
		Overridden              m_overridden;
		std::uint64_t           m_session = 1;
		std::uint64_t           m_revision = 0;
		EndReason               m_lastEnd = EndReason::kNone;
		bool                    m_on = false;
	};
}
