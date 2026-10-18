#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "Json.h"

namespace dvb::ScenarioPolicy
{
	inline constexpr int kEmbeddedToolFallbackErrorCode = 422;

	/// True for a step that teleports the player along a trajectory: a `pose` row, or a
	/// `player.setpos` / `player.setangle` console step (what a pose row expands to).
	[[nodiscard]] inline bool IsTrajectoryStep(const json& a_step)
	{
		if (!a_step.is_object())
			return false;
		if (a_step.contains("pose"))
			return true;
		const auto tool = a_step.find("tool");
		if (tool == a_step.end() || !tool->is_string() || tool->get_ref<const std::string&>() != "console")
			return false;
		const auto args = a_step.find("args");
		if (args == a_step.end() || !args->is_object())
			return false;
		const auto command = args->find("command");
		if (command == args->end() || !command->is_string())
			return false;
		std::string_view text = command->get_ref<const std::string&>();
		while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
			text.remove_prefix(1);
		const auto startsWithCommand = [text](std::string_view a_name) {
			return text.size() > a_name.size() &&
			       std::isspace(static_cast<unsigned char>(text[a_name.size()])) &&
			       std::equal(a_name.begin(), a_name.end(), text.begin(), [](char a_want, char a_have) {
					   return a_want == static_cast<char>(std::tolower(static_cast<unsigned char>(a_have)));
				   });
		};
		return startsWithCommand("player.setpos") || startsWithCommand("player.setangle");
	}

	/// True for a step inside a trajectory span that keeps the replay mark: a trajectory step or a bare
	/// wait. Any other step there releases it, so its input is captured.
	[[nodiscard]] inline bool HoldsReplayMark(const json& a_step)
	{
		return IsTrajectoryStep(a_step) || (a_step.is_object() && a_step.contains("wait") && !a_step.contains("pose"));
	}

	/// First and last index (inclusive) of a run of scenario steps.
	struct StepSpan
	{
		std::size_t first = 0;
		std::size_t last = 0;
	};

	/// The steps a scenario run holds the recorder's replay mark over: its first trajectory step
	/// through its last, plus the bare waits right after it, since the pose sampler would capture
	/// the teleported pose during those waits. Empty when the run has no trajectory step.
	[[nodiscard]] inline std::optional<StepSpan> TrajectorySpan(const json& a_steps)
	{
		std::optional<StepSpan> span;
		if (!a_steps.is_array())
			return span;
		for (std::size_t i = 0; i < a_steps.size(); ++i) {
			if (!IsTrajectoryStep(a_steps[i]))
				continue;
			if (span)
				span->last = i;
			else
				span = StepSpan{ i, i };
		}
		if (!span)
			return span;
		while (span->last + 1 < a_steps.size() && HoldsReplayMark(a_steps[span->last + 1]))
			++span->last;
		return span;
	}

	/// Extension tools report domain failures in their JSON payload. A scenario
	/// must treat those as failed steps even when transport dispatch succeeded.
	[[nodiscard]] inline bool IsEmbeddedToolFailure(const json& a_value)
	{
		if (!a_value.is_object())
			return false;
		if (const auto error = a_value.find("error");
			error != a_value.end() && !error->is_null()) {
			if (!error->is_string() || !error->get_ref<const std::string&>().empty())
				return true;
		}
		if (const auto ok = a_value.find("ok");
			ok != a_value.end() && ok->is_boolean()) {
			return !ok->get<bool>();
		}
		return false;
	}

	/// Return the receipt's code or the scenario fallback failure code.
	[[nodiscard]] inline json EmbeddedToolErrorCode(const json& a_value)
	{
		if (a_value.is_object()) {
			if (const auto code = a_value.find("errorCode");
				code != a_value.end() && !code->is_null()) {
				return *code;
			}
		}
		return kEmbeddedToolFallbackErrorCode;
	}

	/// Return the receipt's message or a stable ok:false fallback.
	[[nodiscard]] inline std::string EmbeddedToolErrorMessage(const json& a_value)
	{
		if (a_value.is_object()) {
			if (const auto error = a_value.find("error");
				error != a_value.end() && !error->is_null()) {
				if (!error->is_string())
					return error->dump();
				if (!error->get_ref<const std::string&>().empty())
					return error->get<std::string>();
			}
		}
		return "tool returned ok:false";
	}
}
