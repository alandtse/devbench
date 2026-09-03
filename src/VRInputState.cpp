#include "VRInputState.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <optional>
#include <stdexcept>

namespace dvb
{
	bool VRSequenceTransaction::Begin(std::string a_owner, std::string a_controlToken,
		bool a_surviveLifecycle)
	{
		if (Busy())
			return false;
		m_starting = true;
		m_active = false;
		m_restoring = false;
		m_restoreAttemptActive = false;
		m_terminalClaimed = false;
		m_indicesApplied = false;
		m_surviveLifecycle = a_surviveLifecycle;
		m_owner = std::move(a_owner);
		m_controlToken = std::move(a_controlToken);
		++m_generation;
		return true;
	}

	bool VRSequenceTransaction::CanApplyIndices(std::uint64_t a_generation) const
	{
		return m_starting && m_generation == a_generation;
	}

	bool VRSequenceTransaction::MarkIndicesApplied(std::uint64_t a_generation)
	{
		if (!CanApplyIndices(a_generation))
			return false;
		m_indicesApplied = true;
		return true;
	}

	bool VRSequenceTransaction::Commit(std::uint64_t a_generation)
	{
		if (!CanApplyIndices(a_generation) || !m_indicesApplied)
			return false;
		m_starting = false;
		m_active = true;
		return true;
	}

	VRSequenceLifecycleDecision VRSequenceTransaction::CancelForLifecycle()
	{
		VRSequenceLifecycleDecision out{ Busy(), false, m_generation, m_owner };
		if (!out.present)
			return out;
		if ((m_starting || m_active) && m_surviveLifecycle) {
			out.preserved = true;
			return out;
		}
		if (m_starting || m_active) {
			m_starting = false;
			m_active = false;
			// Reserve this generation until the asynchronous cleanup claims it.
			m_restoring = true;
		}
		return out;
	}

	VRSequenceStopAccess VRSequenceTransaction::AuthorizeStop(std::string_view a_owner,
		std::string_view a_controlToken, bool a_force, bool a_internal) const
	{
		if (!Busy())
			return VRSequenceStopAccess::kNotActive;
		if (a_force && !a_internal)
			return VRSequenceStopAccess::kForceRequiresInternal;
		if (!a_internal && a_controlToken != m_controlToken)
			return VRSequenceStopAccess::kControlTokenMismatch;
		if (!a_force && a_owner != m_owner)
			return VRSequenceStopAccess::kOwnerMismatch;
		return VRSequenceStopAccess::kAllowed;
	}

	VRSequenceFinishAction VRSequenceTransaction::ClaimFinish(std::uint64_t a_generation)
	{
		if (m_generation != a_generation)
			return VRSequenceFinishAction::kNone;
		if (!m_terminalClaimed) {
			m_terminalClaimed = true;
			m_starting = false;
			m_active = false;
			m_restoring = m_indicesApplied;
			if (!m_restoring)
				return VRSequenceFinishAction::kPublish;
			m_restoreAttemptActive = true;
			return VRSequenceFinishAction::kRestore;
		}
		if (m_restoring && !m_restoreAttemptActive) {
			m_restoreAttemptActive = true;
			return VRSequenceFinishAction::kRestore;
		}
		return VRSequenceFinishAction::kNone;
	}

	bool VRSequenceTransaction::RestoreSucceeded(std::uint64_t a_generation)
	{
		if (m_generation != a_generation || !m_restoring)
			return false;
		m_indicesApplied = false;
		m_restoring = false;
		m_restoreAttemptActive = false;
		return true;
	}

	void VRSequenceTransaction::RestoreFailed(std::uint64_t a_generation)
	{
		if (m_generation == a_generation && m_restoring)
			m_restoreAttemptActive = false;
	}

	namespace
	{
		std::int64_t BoundedIntegerValue(const json& a_value, std::string_view a_name,
			std::int64_t a_minimum, std::int64_t a_maximum)
		{
			if (!a_value.is_number_integer())
				throw std::invalid_argument(std::format("'{}' must be an integer", a_name));
			if (a_value.is_number_unsigned()) {
				const auto value = a_value.get<std::uint64_t>();
				if ((a_minimum > 0 && value < static_cast<std::uint64_t>(a_minimum)) ||
					(a_maximum < 0 || value > static_cast<std::uint64_t>(a_maximum)))
					throw std::invalid_argument(std::format("'{}' is outside [{},{}]", a_name, a_minimum, a_maximum));
				return static_cast<std::int64_t>(value);
			}
			const auto value = a_value.get<std::int64_t>();
			if (value < a_minimum || value > a_maximum)
				throw std::invalid_argument(std::format("'{}' is outside [{},{}]", a_name, a_minimum, a_maximum));
			return value;
		}

		bool Boolean(const json& a_parent, const char* a_name, bool a_default)
		{
			if (!a_parent.contains(a_name))
				return a_default;
			if (!a_parent.at(a_name).is_boolean())
				throw std::invalid_argument(std::format("'{}' must be a boolean", a_name));
			return a_parent.at(a_name).get<bool>();
		}

		template <std::size_t N>
		std::array<float, N> FloatArray(const json& a_parent, const char* a_name,
			bool a_required = true)
		{
			std::array<float, N> out{};
			if (!a_parent.contains(a_name)) {
				if (a_required)
					throw std::invalid_argument(std::format("missing '{}'", a_name));
				return out;
			}
			const auto& value = a_parent.at(a_name);
			if (!value.is_array() || value.size() != N)
				throw std::invalid_argument(std::format("'{}' must contain exactly {} numbers", a_name, N));
			for (std::size_t i = 0; i < N; ++i) {
				if (!value[i].is_number())
					throw std::invalid_argument(std::format("'{}[{}]' must be a number", a_name, i));
				const double number = value[i].get<double>();
				if (!std::isfinite(number) || number < -std::numeric_limits<float>::max() ||
					number > std::numeric_limits<float>::max())
					throw std::invalid_argument(std::format("'{}[{}]' must be finite", a_name, i));
				out[i] = static_cast<float>(number);
			}
			return out;
		}

		VRTrackedPoseState Pose(const json& a_value, const char* a_role)
		{
			if (!a_value.is_object())
				throw std::invalid_argument(std::format("'{}' must be an object", a_role));
			VRTrackedPoseState out;
			out.available = Boolean(a_value, "available", false);
			out.connected = Boolean(a_value, "connected", out.available);
			out.valid = Boolean(a_value, "valid", false);
			if (!out.available) {
				if (out.connected || out.valid)
					throw std::invalid_argument(std::format("'{}' cannot be connected/valid when unavailable", a_role));
				return out;
			}
			if (!a_value.contains("index") || !a_value["index"].is_number_integer())
				throw std::invalid_argument(std::format("'{}.index' must be an unsigned OpenVR device index", a_role));
			const auto index = a_value["index"].get<std::int64_t>();
			if (index < 0 || index >= 64)
				throw std::invalid_argument(std::format("'{}.index' must be below 64", a_role));
			out.index = static_cast<std::uint32_t>(index);
			if (a_value.contains("trackingResult"))
				out.trackingResult = static_cast<std::int32_t>(BoundedIntegerValue(a_value["trackingResult"],
					std::format("{}.trackingResult", a_role), 0, 300));
			out.velocity = FloatArray<3>(a_value, "velocity", false);
			out.angularVelocity = FloatArray<3>(a_value, "angularVelocity", false);
			if (out.valid)
				out.matrix = FloatArray<12>(a_value, "matrix");
			return out;
		}

		VRTrackedDeviceState Controller(const json& a_value, const char* a_role)
		{
			VRTrackedDeviceState out;
			out.pose = Pose(a_value, a_role);
			const json state = a_value.value("controller", json::object());
			if (!state.is_object())
				throw std::invalid_argument(std::format("'{}.controller' must be an object", a_role));
			for (const char* name : { "packetNumber", "pressed", "touched" })
				if (state.contains(name) && (!state[name].is_number_integer() ||
												(state[name].type() == json::value_t::number_integer && state[name].get<std::int64_t>() < 0)))
					throw std::invalid_argument(std::format("'{}.controller.{}' must be a non-negative integer", a_role, name));
			if (state.contains("packetNumber"))
				out.controller.packetNumber = static_cast<std::uint32_t>(BoundedIntegerValue(state["packetNumber"],
					std::format("{}.controller.packetNumber", a_role), 0,
					std::numeric_limits<std::uint32_t>::max()));
			out.controller.pressed = state.value("pressed", std::uint64_t{ 0 });
			out.controller.touched = state.value("touched", std::uint64_t{ 0 });
			if (state.contains("axes")) {
				const auto& axes = state["axes"];
				if (!axes.is_array() || axes.size() != 5)
					throw std::invalid_argument(std::format("'{}.controller.axes' must contain five [x,y] pairs", a_role));
				for (std::size_t i = 0; i < 5; ++i) {
					if (!axes[i].is_array() || axes[i].size() != 2 ||
						!axes[i][0].is_number() || !axes[i][1].is_number())
						throw std::invalid_argument(std::format("'{}.controller.axes[{}]' must be [x,y]", a_role, i));
					const double x = axes[i][0].get<double>();
					const double y = axes[i][1].get<double>();
					if (!std::isfinite(x) || !std::isfinite(y) || x < -1.001 || x > 1.001 || y < -1.001 || y > 1.001)
						throw std::invalid_argument(std::format("'{}.controller.axes[{}]' must be finite values in [-1,1]", a_role, i));
					out.controller.axes[i * 2] = static_cast<float>(x);
					out.controller.axes[i * 2 + 1] = static_cast<float>(y);
				}
			}
			return out;
		}

		json PoseJson(const VRTrackedPoseState& a_pose)
		{
			json out{
				{ "available", a_pose.available },
				{ "connected", a_pose.connected },
				{ "valid", a_pose.valid },
			};
			if (!a_pose.available)
				return out;
			out["index"] = a_pose.index;
			out["trackingResult"] = a_pose.trackingResult;
			out["velocity"] = a_pose.velocity;
			out["angularVelocity"] = a_pose.angularVelocity;
			if (a_pose.valid)
				out["matrix"] = a_pose.matrix;
			return out;
		}
	}

	std::int64_t ParseBoundedIntegerArgument(const json& a_parent, const char* a_name,
		std::int64_t a_default, std::int64_t a_minimum, std::int64_t a_maximum)
	{
		if (!a_parent.contains(a_name))
			return a_default;
		return BoundedIntegerValue(a_parent.at(a_name), a_name, a_minimum, a_maximum);
	}

	std::vector<VRTrackedInputFrame> ParseVRTrackedInputFrames(const json& a_frames)
	{
		if (!a_frames.is_array() || a_frames.empty() || a_frames.size() > kMaximumVRTrackedFrames)
			throw std::invalid_argument(std::format("'frames' must contain 1..{} atomic VR frames", kMaximumVRTrackedFrames));
		std::vector<VRTrackedInputFrame> out;
		out.reserve(a_frames.size());
		std::int64_t                 previous = -1;
		std::uint64_t                previousSeq = 0;
		std::optional<std::uint32_t> leftIndex;
		std::optional<std::uint32_t> rightIndex;
		std::optional<std::int32_t>  originCode;
		for (std::size_t i = 0; i < a_frames.size(); ++i) {
			const auto& item = a_frames[i];
			if (!item.is_object())
				throw std::invalid_argument(std::format("frames[{}] must be an object", i));
			VRTrackedInputFrame frame;
			if (!item.contains("tMs") || !item["tMs"].is_number_integer())
				throw std::invalid_argument(std::format("frames[{}].tMs must be an integer", i));
			frame.tMs = item["tMs"].get<std::int64_t>();
			if (frame.tMs < 0 || frame.tMs > kMaximumVRTrackedDurationMs || (i > 0 && frame.tMs <= previous))
				throw std::invalid_argument(std::format("frames[{}].tMs must be strictly increasing in [0,{}]", i, kMaximumVRTrackedDurationMs));
			if (i == 0 && frame.tMs != 0)
				throw std::invalid_argument("frames[0].tMs must be 0 so the complete tracked set is defined immediately");
			previous = frame.tMs;
			if (item.contains("seq") && (!item["seq"].is_number_integer() ||
											(item["seq"].type() == json::value_t::number_integer && item["seq"].get<std::int64_t>() < 0)))
				throw std::invalid_argument(std::format("frames[{}].seq must be a non-negative integer", i));
			frame.seq = item.value("seq", static_cast<std::uint64_t>(i + 1));
			if (frame.seq == 0 || (i > 0 && frame.seq <= previousSeq))
				throw std::invalid_argument(std::format("frames[{}].seq must be strictly increasing and non-zero", i));
			previousSeq = frame.seq;
			if (item.contains("originCode"))
				frame.originCode = static_cast<std::int32_t>(BoundedIntegerValue(item["originCode"],
					std::format("frames[{}].originCode", i), 0, 2));
			if (originCode && frame.originCode != *originCode)
				throw std::invalid_argument(std::format("frames[{}].originCode changed within one atomic sequence", i));
			originCode = frame.originCode;
			if (!item.contains("hmd") || !item.contains("left") || !item.contains("right"))
				throw std::invalid_argument(std::format("frames[{}] must contain hmd, left, and right", i));
			frame.hmd = Pose(item["hmd"], "hmd");
			frame.left = Controller(item["left"], "left");
			frame.right = Controller(item["right"], "right");
			if (!frame.hmd.available || !frame.left.pose.available || !frame.right.pose.available)
				throw std::invalid_argument(std::format("frames[{}] must make hmd, left, and right available", i));
			if (frame.hmd.index != 0)
				throw std::invalid_argument(std::format("frames[{}].hmd.index must be OpenVR HMD index 0", i));
			if (frame.left.pose.index == 0 || frame.right.pose.index == 0 ||
				frame.left.pose.index == frame.right.pose.index)
				throw std::invalid_argument(std::format("frames[{}] hmd/left/right indices must be pairwise distinct", i));
			const auto stableRoleIndex = [i](const char* a_role, const VRTrackedPoseState& a_pose,
											 std::optional<std::uint32_t>& a_index) {
				if (!a_pose.available)
					return;
				if (a_index && a_pose.index != *a_index)
					throw std::invalid_argument(std::format("frames[{}].{}.index changed within one atomic sequence", i, a_role));
				a_index = a_pose.index;
			};
			stableRoleIndex("left", frame.left.pose, leftIndex);
			stableRoleIndex("right", frame.right.pose, rightIndex);
			out.push_back(std::move(frame));
		}
		return out;
	}

	json VRTrackedInputFrameJson(const VRTrackedInputFrame& a_frame)
	{
		json       left = PoseJson(a_frame.left.pose);
		json       right = PoseJson(a_frame.right.pose);
		const auto controllerJson = [](const VRControllerState& a_controller) {
			json axes = json::array();
			for (std::size_t i = 0; i < 5; ++i)
				axes.push_back(json::array({ a_controller.axes[i * 2], a_controller.axes[i * 2 + 1] }));
			return json{ { "packetNumber", a_controller.packetNumber },
				{ "pressed", a_controller.pressed }, { "touched", a_controller.touched },
				{ "axes", std::move(axes) } };
		};
		left["controller"] = controllerJson(a_frame.left.controller);
		right["controller"] = controllerJson(a_frame.right.controller);
		return json{ { "tMs", a_frame.tMs }, { "seq", a_frame.seq },
			{ "originCode", a_frame.originCode }, { "hmd", PoseJson(a_frame.hmd) },
			{ "left", std::move(left) }, { "right", std::move(right) } };
	}
}
