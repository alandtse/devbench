#include "ReachLogic.h"

#include <algorithm>
#include <array>
#include <format>

namespace dvb::ReachLogic
{

	namespace
	{

		constexpr double kEpsilon = 1e-12;
		constexpr double kDetEpsilon = 1e-6;
		constexpr long   kFrameStepMs = 1000;
		constexpr long   kCalibrationSettleMs = 2600;
		constexpr int    kBlendFrames = 6;
		constexpr long   kBlendStepMs = 300;
		constexpr long   kBlendDurationMs = (kBlendFrames - 1) * kBlendStepMs;

		std::string FormatNumber(double a_value)
		{
			return std::format("{}", a_value);
		}

		bool ReadNumber(const json& a_value, const std::string& a_path, double a_min, double a_max, double& a_out, std::string& a_error)
		{
			const std::string range = FormatNumber(a_min) + ".." + FormatNumber(a_max);
			if (!a_value.is_number()) {
				a_error = "'" + a_path + "' must be a number " + range;
				return false;
			}
			const double value = a_value.get<double>();
			if (!std::isfinite(value) || value < a_min || value > a_max) {
				a_error = "'" + a_path + "' must be a number " + range;
				return false;
			}
			a_out = value;
			return true;
		}

		bool ReadInteger(const json& a_value, const std::string& a_path, long long a_min, long long a_max, long long& a_out, std::string& a_error)
		{
			const std::string range = FormatNumber(static_cast<double>(a_min)) + ".." + FormatNumber(static_cast<double>(a_max));
			if (!a_value.is_number_integer()) {
				a_error = "'" + a_path + "' must be an integer " + range;
				return false;
			}
			const double value = a_value.get<double>();
			if (!std::isfinite(value) || value < static_cast<double>(a_min) || value > static_cast<double>(a_max)) {
				a_error = "'" + a_path + "' must be an integer " + range;
				return false;
			}
			a_out = static_cast<long long>(value);
			return true;
		}

		bool ReadVec3(const json& a_value, const std::string& a_path, double a_limit, Vec3& a_out, std::string& a_error)
		{
			const std::string range = "+-" + FormatNumber(a_limit);
			if (!a_value.is_array() || a_value.size() != 3) {
				a_error = "'" + a_path + "' must be an array of three numbers within " + range;
				return false;
			}
			double components[3] = { 0.0, 0.0, 0.0 };
			for (int i = 0; i < 3; ++i) {
				const json& element = a_value[static_cast<std::size_t>(i)];
				if (!element.is_number()) {
					a_error = "'" + a_path + "[" + std::to_string(i) + "]' must be a number within " + range;
					return false;
				}
				const double value = element.get<double>();
				if (!std::isfinite(value) || value < -a_limit || value > a_limit) {
					a_error = "'" + a_path + "[" + std::to_string(i) + "]' must be a number within " + range;
					return false;
				}
				components[i] = value;
			}
			a_out = Vec3{ components[0], components[1], components[2] };
			return true;
		}

		json PoseToJson(const Pose& a_pose)
		{
			json result = json::array();
			for (double value : a_pose) {
				result.push_back(value);
			}
			return result;
		}

		json DeviceJson(int a_index, const Pose& a_pose, bool a_controller, int a_packet)
		{
			json device;
			device["available"] = true;
			device["index"] = a_index;
			device["connected"] = true;
			device["valid"] = true;
			device["trackingResult"] = 200;
			device["matrix"] = PoseToJson(a_pose);
			if (a_controller) {
				json controller;
				controller["packetNumber"] = a_packet;
				controller["pressed"] = 0;
				controller["touched"] = 0;
				json axes = json::array();
				for (int i = 0; i < 5; ++i) {
					json axis = json::array();
					axis.push_back(0);
					axis.push_back(0);
					axes.push_back(axis);
				}
				controller["axes"] = axes;
				device["controller"] = controller;
			}
			return device;
		}

	}

	Vec3 Add(const Vec3& a_a, const Vec3& a_b)
	{
		return Vec3{ a_a.x + a_b.x, a_a.y + a_b.y, a_a.z + a_b.z };
	}

	Vec3 Sub(const Vec3& a_a, const Vec3& a_b)
	{
		return Vec3{ a_a.x - a_b.x, a_a.y - a_b.y, a_a.z - a_b.z };
	}

	Vec3 Scale(const Vec3& a_v, double a_s)
	{
		return Vec3{ a_v.x * a_s, a_v.y * a_s, a_v.z * a_s };
	}

	Vec3 Cross(const Vec3& a_a, const Vec3& a_b)
	{
		return Vec3{
			a_a.y * a_b.z - a_a.z * a_b.y,
			a_a.z * a_b.x - a_a.x * a_b.z,
			a_a.x * a_b.y - a_a.y * a_b.x
		};
	}

	double Dot(const Vec3& a_a, const Vec3& a_b)
	{
		return a_a.x * a_b.x + a_a.y * a_b.y + a_a.z * a_b.z;
	}

	double Norm(const Vec3& a_v)
	{
		return std::sqrt(Dot(a_v, a_v));
	}

	Vec3 Unit(const Vec3& a_v)
	{
		const double length = Norm(a_v);
		if (length < kEpsilon) {
			return Vec3{};
		}
		return Scale(a_v, 1.0 / length);
	}

	double Distance(const Vec3& a_a, const Vec3& a_b)
	{
		return Norm(Sub(a_a, a_b));
	}

	Pose Upright(const Vec3& a_position)
	{
		return Pose{
			1.0, 0.0, 0.0, a_position.x,
			0.0, 1.0, 0.0, a_position.y,
			0.0, 0.0, 1.0, a_position.z
		};
	}

	Pose LookAt(const Vec3& a_position, const Vec3& a_target)
	{
		Vec3 forward = Unit(Sub(a_target, a_position));
		if (Norm(forward) < kEpsilon) {
			forward = Vec3{ 0.0, 0.0, -1.0 };
		}
		Vec3 right = Unit(Cross(forward, Vec3{ 0.0, 1.0, 0.0 }));
		if (Norm(right) < kEpsilon) {
			right = Vec3{ 1.0, 0.0, 0.0 };
		}
		const Vec3 up = Cross(right, forward);
		const Vec3 back = Scale(forward, -1.0);
		return Pose{
			right.x, up.x, back.x, a_position.x,
			right.y, up.y, back.y, a_position.y,
			right.z, up.z, back.z, a_position.z
		};
	}

	std::optional<Calibration> CalibrationFromPoints(const std::array<Vec3, 4>& a_points, double a_stepMeters)
	{
		if (!std::isfinite(a_stepMeters) || a_stepMeters <= 0.0) {
			return std::nullopt;
		}
		Calibration calibration;
		calibration.rest = a_points[0];
		for (int i = 0; i < 3; ++i) {
			calibration.perMeter[static_cast<std::size_t>(i)] = Scale(Sub(a_points[static_cast<std::size_t>(i + 1)], a_points[0]), 1.0 / a_stepMeters);
		}
		const double determinant = Dot(calibration.perMeter[0], Cross(calibration.perMeter[1], calibration.perMeter[2]));
		if (!std::isfinite(determinant) || std::abs(determinant) < kDetEpsilon) {
			return std::nullopt;
		}
		return calibration;
	}

	std::optional<Vec3> TrackingDelta(const Calibration& a_calibration, const Vec3& a_worldDelta)
	{
		const Vec3&  c0 = a_calibration.perMeter[0];
		const Vec3&  c1 = a_calibration.perMeter[1];
		const Vec3&  c2 = a_calibration.perMeter[2];
		const double determinant = Dot(c0, Cross(c1, c2));
		if (!std::isfinite(determinant) || std::abs(determinant) < kDetEpsilon) {
			return std::nullopt;
		}
		const double dx = Dot(a_worldDelta, Cross(c1, c2)) / determinant;
		const double dy = Dot(c0, Cross(a_worldDelta, c2)) / determinant;
		const double dz = Dot(c0, Cross(c1, a_worldDelta)) / determinant;
		return Vec3{ dx, dy, dz };
	}

	Vec3 TrackingFor(const Calibration& a_calibration, const Vec3& a_restTracking, const Vec3& a_world)
	{
		const auto delta = TrackingDelta(a_calibration, Sub(a_world, a_calibration.rest));
		if (!delta) {
			return a_restTracking;
		}
		return Add(a_restTracking, *delta);
	}

	std::optional<ReachRequest> ParseReach(const json& a_args, std::string& a_error)
	{
		a_error.clear();
		if (!a_args.is_object()) {
			a_error = "arguments must be a JSON object";
			return std::nullopt;
		}

		ReachRequest request;

		if (a_args.contains("hands") && !a_args["hands"].is_null()) {
			const json& hands = a_args["hands"];
			if (!hands.is_object()) {
				a_error = "'hands' must be an object";
				return std::nullopt;
			}
			bool anyHand = false;
			for (const char* side : { "left", "right" }) {
				if (!hands.contains(side) || hands[side].is_null()) {
					continue;
				}
				const json&       hand = hands[side];
				const std::string path = std::string("hands.") + side;
				if (!hand.is_object()) {
					a_error = "'" + path + "' must be an object";
					return std::nullopt;
				}
				if (!hand.contains("node") || !hand["node"].is_string()) {
					a_error = "'" + path + ".node' must be a non-empty string of at most 128 characters";
					return std::nullopt;
				}
				HandTarget target;
				target.node = hand["node"].get<std::string>();
				if (target.node.empty() || target.node.size() > kNodeMaxLen) {
					a_error = "'" + path + ".node' must be a non-empty string of at most 128 characters";
					return std::nullopt;
				}
				if (hand.contains("front") && !hand["front"].is_null()) {
					if (!ReadNumber(hand["front"], path + ".front", kFrontMin, kFrontMax, target.front, a_error)) {
						return std::nullopt;
					}
				}
				if (hand.contains("offset") && !hand["offset"].is_null()) {
					if (!ReadVec3(hand["offset"], path + ".offset", kOffsetLimit, target.offset, a_error)) {
						return std::nullopt;
					}
				}
				if (std::string(side) == "left") {
					request.left = target;
				} else {
					request.right = target;
				}
				anyHand = true;
			}
			if (!anyHand) {
				a_error = "'hands' must contain at least one of 'left'/'right'";
				return std::nullopt;
			}
		}

		if (a_args.contains("head") && !a_args["head"].is_null()) {
			const json& head = a_args["head"];
			if (!head.is_object()) {
				a_error = "'head' must be an object";
				return std::nullopt;
			}
			if (head.contains("lookAt") && !head["lookAt"].is_null()) {
				if (!head["lookAt"].is_string()) {
					a_error = "'head.lookAt' must be a non-empty string";
					return std::nullopt;
				}
				const std::string lookAt = head["lookAt"].get<std::string>();
				if (lookAt.empty()) {
					a_error = "'head.lookAt' must be a non-empty string";
					return std::nullopt;
				}
				if (lookAt == "hands") {
					request.head.aim = HeadTarget::Aim::kHands;
				} else {
					if (lookAt.size() > kNodeMaxLen) {
						a_error = "'head.lookAt' node name must be at most 128 characters";
						return std::nullopt;
					}
					request.head.aim = HeadTarget::Aim::kNode;
					request.head.node = lookAt;
				}
			}
			if (head.contains("back") && !head["back"].is_null()) {
				if (!ReadNumber(head["back"], "head.back", kBackMin, kBackMax, request.head.back, a_error)) {
					return std::nullopt;
				}
			}
			if (head.contains("up") && !head["up"].is_null()) {
				if (!ReadNumber(head["up"], "head.up", kUpMin, kUpMax, request.head.up, a_error)) {
					return std::nullopt;
				}
			}
			if (head.contains("pos") && !head["pos"].is_null()) {
				Vec3 position;
				if (!ReadVec3(head["pos"], "head.pos", kPosLimit, position, a_error)) {
					return std::nullopt;
				}
				request.head.position = position;
			}
			if (head.contains("eyeHeight") && !head["eyeHeight"].is_null()) {
				double eyeHeight = 0.0;
				if (!ReadNumber(head["eyeHeight"], "head.eyeHeight", 0.0, kEyeHeightMax, eyeHeight, a_error)) {
					return std::nullopt;
				}
				request.head.eyeHeight = eyeHeight;
			}
			if (request.head.aim == HeadTarget::Aim::kNone && std::ranges::any_of(std::array{ "back", "up", "pos", "eyeHeight" }, [&](const char* a_key) { return head.contains(a_key) && !head[a_key].is_null(); })) {
				a_error = "'head' needs 'lookAt' when 'back', 'up', 'pos' or 'eyeHeight' is given";
				return std::nullopt;
			}
		}

		if (a_args.contains("holdMs") && !a_args["holdMs"].is_null()) {
			long long value = 0;
			if (!ReadInteger(a_args["holdMs"], "holdMs", kHoldMsMin, kHoldMsMax, value, a_error)) {
				return std::nullopt;
			}
			request.holdMs = static_cast<long>(value);
		}

		if (a_args.contains("iterations") && !a_args["iterations"].is_null()) {
			long long value = 0;
			if (!ReadInteger(a_args["iterations"], "iterations", kIterationsMin, kIterationsMax, value, a_error)) {
				return std::nullopt;
			}
			request.iterations = static_cast<int>(value);
		}

		if (a_args.contains("toleranceUnits") && !a_args["toleranceUnits"].is_null()) {
			if (!ReadNumber(a_args["toleranceUnits"], "toleranceUnits", kToleranceMin, kToleranceMax, request.toleranceUnits, a_error)) {
				return std::nullopt;
			}
		}

		if (a_args.contains("approachUnits") && !a_args["approachUnits"].is_null()) {
			if (!ReadNumber(a_args["approachUnits"], "approachUnits", kApproachMin, kApproachMax, request.approachUnits, a_error)) {
				return std::nullopt;
			}
		}

		if (!request.left && !request.right) {
			a_error = "'hands' must contain at least one of 'left'/'right'";
			return std::nullopt;
		}
		if (request.head.aim == HeadTarget::Aim::kNode && !request.left && !request.right) {
			a_error = "'head.lookAt' node requires at least one hand target for calibration";
			return std::nullopt;
		}
		if (request.head.aim == HeadTarget::Aim::kHands && !request.left && !request.right) {
			a_error = "'head.lookAt' 'hands' requires at least one hand target";
			return std::nullopt;
		}

		return request;
	}

	json MakeFrame(long a_tMs, int a_packet, const Pose& a_hmd, const Pose& a_left, const Pose& a_right)
	{
		json frame;
		frame["originCode"] = 1;
		frame["hmd"] = DeviceJson(0, a_hmd, false, a_packet);
		frame["left"] = DeviceJson(1, a_left, true, a_packet);
		frame["right"] = DeviceJson(2, a_right, true, a_packet);
		frame["tMs"] = a_tMs;
		frame["seq"] = a_packet;
		return frame;
	}

	json CalibrationFrames(bool a_rightHand, const Pose& a_hmd, const Vec3& a_restTracking, const Vec3& a_otherHandTracking, double a_stepMeters)
	{
		json       frames = json::array();
		int        packet = 1;
		const Vec3 legs[4] = {
			Vec3{ 0.0, 0.0, 0.0 },
			Vec3{ a_stepMeters, 0.0, 0.0 },
			Vec3{ 0.0, a_stepMeters, 0.0 },
			Vec3{ 0.0, 0.0, a_stepMeters }
		};
		const Pose other = Upright(a_otherHandTracking);
		for (int leg = 0; leg < 4; ++leg) {
			const Pose moving = Upright(Add(a_restTracking, legs[leg]));
			for (int k = 0; k < 4; ++k) {
				const long  tMs = static_cast<long>(leg) * kCalibrationLegMs + static_cast<long>(k) * kFrameStepMs;
				const Pose& left = a_rightHand ? other : moving;
				const Pose& right = a_rightHand ? moving : other;
				frames.push_back(MakeFrame(tMs, packet, a_hmd, left, right));
				++packet;
			}
		}
		return frames;
	}

	long CalibrationSampleMs(int a_leg)
	{
		return static_cast<long>(a_leg) * kCalibrationLegMs + kCalibrationSettleMs;
	}

	long ApproachAndHoldDurationMs(long a_holdMs)
	{
		return kBlendDurationMs + std::max(a_holdMs / kFrameStepMs, 1L) * kFrameStepMs;
	}

	json ApproachAndHoldFrames(const Pose& a_hmd, const Vec3& a_leftStart, const Vec3& a_leftFinal, const Vec3& a_rightStart, const Vec3& a_rightFinal, long a_holdMs)
	{
		json frames = json::array();
		int  packet = 1;
		for (int k = 0; k < kBlendFrames; ++k) {
			const double t = static_cast<double>(k) / static_cast<double>(kBlendFrames - 1);
			const Vec3   left = Add(a_leftStart, Scale(Sub(a_leftFinal, a_leftStart), t));
			const Vec3   right = Add(a_rightStart, Scale(Sub(a_rightFinal, a_rightStart), t));
			const long   tMs = static_cast<long>(k) * kBlendStepMs;
			frames.push_back(MakeFrame(tMs, packet, a_hmd, Upright(left), Upright(right)));
			++packet;
		}
		long holdCount = a_holdMs / kFrameStepMs;
		if (holdCount < 1L) {
			holdCount = 1L;
		}
		const Pose leftFinal = Upright(a_leftFinal);
		const Pose rightFinal = Upright(a_rightFinal);
		for (long k = 1; k <= holdCount; ++k) {
			const long tMs = kBlendDurationMs + k * kFrameStepMs;
			frames.push_back(MakeFrame(tMs, packet, a_hmd, leftFinal, rightFinal));
			++packet;
		}
		return frames;
	}

	std::string_view PhaseName(Phase a_phase)
	{
		switch (a_phase) {
		case Phase::kQueued:
			return "queued";
		case Phase::kCalibrating:
			return "calibrating";
		case Phase::kReaching:
			return "reaching";
		case Phase::kHolding:
			return "holding";
		case Phase::kDone:
			return "done";
		case Phase::kFailed:
			return "failed";
		case Phase::kStopped:
			return "stopped";
		}
		return "queued";
	}

	json ProgressJson(const Progress& a_progress)
	{
		const auto round2 = [](double a_value) { return std::round(a_value * 100.0) / 100.0; };
		const auto vecJson = [&round2](const Vec3& a_v) {
			return json::array({ round2(a_v.x), round2(a_v.y), round2(a_v.z) });
		};
		const auto handJson = [&round2, &vecJson](const HandProgress& a_hand) {
			json result;
			result["hand"] = vecJson(a_hand.hand);
			result["node"] = vecJson(a_hand.node);
			result["error"] = vecJson(a_hand.error);
			result["errorDistance"] = round2(Norm(a_hand.error));
			result["handToNode"] = round2(Distance(a_hand.hand, a_hand.node));
			return result;
		};
		json result;
		result["phase"] = std::string(PhaseName(a_progress.phase));
		result["iteration"] = a_progress.iteration;
		if (a_progress.left) {
			result["left"] = handJson(*a_progress.left);
		}
		if (a_progress.right) {
			result["right"] = handJson(*a_progress.right);
		}
		if (a_progress.left || a_progress.right) {
			const double worst = std::max(a_progress.left ? Norm(a_progress.left->error) : 0.0, a_progress.right ? Norm(a_progress.right->error) : 0.0);
			result["worstErrorDistance"] = round2(worst);
		}
		if (!a_progress.error.empty()) {
			result["error"] = a_progress.error;
		}
		return result;
	}

	bool Converged(double a_worstErrorUnits, double a_toleranceUnits)
	{
		return a_worstErrorUnits <= a_toleranceUnits;
	}

}
