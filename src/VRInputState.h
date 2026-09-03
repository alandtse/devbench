#pragma once

#include "Json.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dvb
{
	inline constexpr std::size_t  kMaximumVRTrackedFrames = 60000;
	inline constexpr std::int64_t kMaximumVRTrackedDurationMs = 30 * 60 * 1000;

	struct VRTrackedPoseState
	{
		bool                  available = false;
		bool                  connected = false;
		bool                  valid = false;
		std::uint32_t         index = 0;
		std::int32_t          trackingResult = 0;
		std::array<float, 12> matrix{};
		std::array<float, 3>  velocity{};
		std::array<float, 3>  angularVelocity{};
	};

	struct VRControllerState
	{
		std::uint32_t         packetNumber = 0;
		std::uint64_t         pressed = 0;
		std::uint64_t         touched = 0;
		std::array<float, 10> axes{};  // five OpenVR x/y axis pairs
	};

	struct VRTrackedDeviceState
	{
		VRTrackedPoseState pose;
		VRControllerState  controller;
	};

	struct VRTrackedInputFrame
	{
		std::int64_t         tMs = 0;
		std::uint64_t        seq = 0;
		std::int32_t         originCode = 1;  // OpenVR standing
		VRTrackedPoseState   hmd;
		VRTrackedDeviceState left;
		VRTrackedDeviceState right;
	};

	enum class VRSequenceFinishAction
	{
		kNone,
		kPublish,
		kRestore
	};

	enum class VRSequenceStopAccess
	{
		kAllowed,
		kNotActive,
		kForceRequiresInternal,
		kControlTokenMismatch,
		kOwnerMismatch
	};

	struct VRSequenceLifecycleDecision
	{
		bool          present = false;
		bool          preserved = false;
		std::uint64_t generation = 0;
		std::string   owner;
	};

	// Pure transaction state shared by the runtime manager and host-side tests.
	class VRSequenceTransaction
	{
	public:
		bool                        Begin(std::string a_owner, std::string a_controlToken, bool a_surviveLifecycle);
		bool                        CanApplyIndices(std::uint64_t a_generation) const;
		bool                        MarkIndicesApplied(std::uint64_t a_generation);
		bool                        Commit(std::uint64_t a_generation);
		VRSequenceLifecycleDecision CancelForLifecycle();
		VRSequenceStopAccess        AuthorizeStop(std::string_view a_owner,
			std::string_view a_controlToken, bool a_force, bool a_internal) const;
		VRSequenceFinishAction      ClaimFinish(std::uint64_t a_generation);
		bool                        RestoreSucceeded(std::uint64_t a_generation);
		void                        RestoreFailed(std::uint64_t a_generation);

		bool               Busy() const { return m_starting || m_active || m_restoring || m_indicesApplied; }
		bool               Starting() const { return m_starting; }
		bool               Active() const { return m_active; }
		bool               Restoring() const { return m_restoring; }
		bool               RestoreAttemptActive() const { return m_restoreAttemptActive; }
		bool               TerminalClaimed() const { return m_terminalClaimed; }
		bool               IndicesApplied() const { return m_indicesApplied; }
		bool               SurvivesLifecycle() const { return m_surviveLifecycle; }
		std::uint64_t      Generation() const { return m_generation; }
		const std::string& Owner() const { return m_owner; }

	private:
		bool          m_starting = false;
		bool          m_active = false;
		bool          m_restoring = false;
		bool          m_restoreAttemptActive = false;
		bool          m_terminalClaimed = false;
		bool          m_indicesApplied = false;
		bool          m_surviveLifecycle = false;
		std::uint64_t m_generation = 0;
		std::string   m_owner;
		std::string   m_controlToken;
	};

	// Parse and completely validate an atomic HMD + left-controller + right-controller sequence.
	// Throws std::invalid_argument before any game-facing mutation can occur.
	std::vector<VRTrackedInputFrame> ParseVRTrackedInputFrames(const json& a_frames);

	// Decode a bounded JSON integer without narrowing before range validation.
	std::int64_t ParseBoundedIntegerArgument(const json& a_parent, const char* a_name,
		std::int64_t a_default, std::int64_t a_minimum, std::int64_t a_maximum);

	// Canonical JSON used by status/events and by host-independent round-trip tests.
	json VRTrackedInputFrameJson(const VRTrackedInputFrame& a_frame);
}
