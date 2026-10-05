#include "CameraOrbitLogic.h"

namespace dvb::CameraOrbit
{
	const char* EndReasonName(EndReason a_reason)
	{
		switch (a_reason) {
		case EndReason::kRequested:
			return "requested";
		case EndReason::kLeftThirdPerson:
			return "leftThirdPerson";
		case EndReason::kFreeCamera:
			return "freeCamera";
		case EndReason::kPov:
			return "setPov";
		case EndReason::kSessionEnded:
			return "loadOrNewGame";
		default:
			return "none";
		}
	}

	Admission OrbitState::Enable(const Request& a_request, std::uint64_t a_requestSession, bool a_loading, bool a_freeCamera)
	{
		if (a_requestSession != m_session)
			return Admission::kStaleSession;
		if (a_loading)
			return Admission::kLoading;
		if (a_freeCamera)
			return Admission::kFreeCamera;
		if (!m_on) {
			m_baseline.reset();
			m_overridden = {};
		}
		m_request = a_request;
		m_on = true;
		m_lastEnd = EndReason::kNone;
		++m_revision;
		return Admission::kAccepted;
	}

	std::optional<OrbitState::Write> OrbitState::Apply(const Baseline& a_current)
	{
		if (!m_on)
			return std::nullopt;
		if (!m_baseline)
			m_baseline = a_current;
		const Baseline& base = *m_baseline;

		Write out;
		out.playerYaw = base.playerYaw;
		out.camera = a_current.camera;
		const float yaw = base.playerYaw + m_request.yawRad;
		out.camera.targetYaw = yaw;
		out.camera.currentYaw = yaw;
		out.camera.freeRotationX = m_request.yawRad;
		if (m_request.offset) {
			out.camera.offsetExpected = *m_request.offset;
			out.camera.offsetActual = *m_request.offset;
			m_overridden.offset = true;
		}
		if (m_request.zoom) {
			out.camera.targetZoom = *m_request.zoom;
			out.camera.currentZoom = *m_request.zoom;
			m_overridden.zoom = true;
		}
		if (m_request.pitchRad) {
			out.playerPitch = *m_request.pitchRad;
			m_overridden.pitch = true;
		}
		return out;
	}

	std::optional<OrbitState::Restore> OrbitState::End(EndReason a_reason, const CameraFields& a_current)
	{
		if (!m_on)
			return std::nullopt;
		m_on = false;
		m_lastEnd = a_reason;
		++m_revision;
		if (!m_baseline)
			return std::nullopt;

		const Baseline& base = *m_baseline;
		Restore         out;
		out.camera = a_current;
		out.camera.targetYaw = base.camera.targetYaw;
		out.camera.currentYaw = base.camera.currentYaw;
		out.camera.freeRotationX = base.camera.freeRotationX;
		if (m_overridden.zoom) {
			out.camera.targetZoom = base.camera.targetZoom;
			out.camera.currentZoom = base.camera.currentZoom;
		}
		if (m_overridden.offset) {
			out.camera.offsetExpected = base.camera.offsetExpected;
			out.camera.offsetActual = base.camera.offsetActual;
		}
		if (m_overridden.pitch)
			out.playerPitch = base.playerPitch;
		m_baseline.reset();
		m_overridden = {};
		return out;
	}

	void OrbitState::EndSession()
	{
		if (m_on)
			m_lastEnd = EndReason::kSessionEnded;
		m_on = false;
		m_baseline.reset();
		m_overridden = {};
		++m_session;
		++m_revision;
	}
}
