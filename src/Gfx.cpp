#include "Gfx.h"

#include "GfxLogic.h"
#include "MainThread.h"

#include <algorithm>
#include <string>
#include <vector>

namespace dvb::Gfx
{
	namespace
	{
		constexpr int           kDefaultDepth = 2;
		constexpr int           kMaxDepth = 8;
		constexpr std::uint32_t kDefaultLimit = 500;
		constexpr int           kMaxNodes = 20000;  // display objects link back to parents, so depth alone is not a budget
		constexpr std::size_t   kMaxBatch = 64;

		using GfxLogic::Budget;

		json ToJson(const RE::GFxValue& a_value, int a_depth, std::uint32_t a_limit, Budget& a_budget)
		{
			using Type = RE::GFxValue::ValueType;
			if (a_budget.Exhausted(kMaxNodes))
				return "<node limit>";
			++a_budget.nodes;
			switch (a_value.GetType()) {
			case Type::kBoolean:
				return a_value.GetBool();
			case Type::kNumber:
				return a_value.GetNumber();
			case Type::kString:
				return a_value.GetString() ? GfxLogic::ToUtf8(a_value.GetString()) : "";
			case Type::kStringW:
				return a_value.GetStringW() ? SKSE::stl::utf16_to_utf8(a_value.GetStringW()).value_or("") : "";
			case Type::kUndefined:
				return "<undefined>";
			case Type::kArray:
				{
					const auto size = a_value.GetArraySize();
					if (a_depth <= 0) {
						a_budget.truncated = true;
						return std::format("<array {}>", size);
					}
					a_budget.truncated |= size > a_limit;
					json out = json::array();
					for (std::uint32_t i = 0; i < size && i < a_limit; ++i) {
						if (a_budget.Exhausted(kMaxNodes))
							break;
						RE::GFxValue item;
						a_value.GetElement(i, &item);
						out.push_back(ToJson(item, a_depth - 1, a_limit, a_budget));
					}
					return out;
				}
			case Type::kObject:
			case Type::kDisplayObject:
				{
					if (a_depth <= 0) {
						a_budget.truncated = true;
						return "<object>";
					}
					json out = json::object();
					// This Scaleform has no function type: methods arrive as objects and print as {}.
					a_value.VisitMembers([&](const char* a_name, const RE::GFxValue& a_member) {
						if (!a_budget.Exhausted(kMaxNodes))
							out[GfxLogic::ToUtf8(a_name)] = ToJson(a_member, a_depth - 1, a_limit, a_budget);
					});
					return out;
				}
			default:
				return nullptr;
			}
		}

		void FromJson(RE::GFxMovieView* a_movie, const json& a_value, RE::GFxValue& a_out)
		{
			if (a_value.is_boolean()) {
				a_out.SetBoolean(a_value.get<bool>());
			} else if (a_value.is_number()) {
				a_out.SetNumber(a_value.get<double>());
			} else if (a_value.is_string()) {
				a_movie->CreateString(&a_out, a_value.get_ref<const std::string&>().c_str());
			} else if (a_value.is_array()) {
				a_movie->CreateArray(&a_out);
				for (const auto& item : a_value) {
					RE::GFxValue element;
					FromJson(a_movie, item, element);
					a_out.PushBack(element);
				}
			} else if (a_value.is_object()) {
				a_movie->CreateObject(&a_out);
				for (const auto& [name, item] : a_value.items()) {
					RE::GFxValue member;
					FromJson(a_movie, item, member);
					a_out.SetMember(name.c_str(), member);
				}
			} else {
				a_out.SetNull();
			}
		}

		bool Step(const RE::GFxValue& a_from, const std::string& a_segment, RE::GFxValue& a_to)
		{
			if (a_from.IsArray() && GfxLogic::IsIndex(a_segment))
				return a_from.GetElement(static_cast<std::uint32_t>(std::stoul(a_segment)), &a_to);
			return a_from.GetMember(a_segment.c_str(), &a_to);
		}

		// Walks a_count segments. The first is a root the movie resolves itself: _root, _global, or a timeline variable.
		// GFxValue only asserts its type, so every step checks IsObject: a member lookup on a number crashes in release.
		void Resolve(RE::GFxMovieView* a_movie, const std::vector<std::string>& a_segments, std::size_t a_count, RE::GFxValue& a_out)
		{
			if (!a_movie->GetVariable(&a_out, a_segments[0].c_str()))
				throw ToolError(404, "root not found: " + a_segments[0]);
			for (std::size_t i = 1; i < a_count; ++i) {
				if (!a_out.IsObject())
					throw ToolError(400, "not an object: " + a_segments[i - 1]);
				RE::GFxValue next;
				if (!Step(a_out, a_segments[i], next))
					throw ToolError(404, "member not found: " + a_segments[i]);
				a_out = next;
			}
		}

		json RunOne(RE::GFxMovieView* a_movie, const json& a_args)
		{
			const auto op = a_args.value("op", std::string{});
			const auto segments = GfxLogic::SplitPath(a_args.value("path", std::string{}));
			if (op == "batch")
				throw ToolError(400, "batch cannot be nested");
			if (op != "get" && op != "set" && op != "call")
				throw ToolError(400, "op must be get, set, call or batch");
			if (segments.empty())
				throw ToolError(400, "path is required");
			Budget budget;

			if (op == "get") {
				RE::GFxValue value;
				Resolve(a_movie, segments, segments.size(), value);
				const int  depth = std::clamp(a_args.value("depth", kDefaultDepth), 0, kMaxDepth);
				const auto limit = GfxLogic::ReadLimit(a_args, kDefaultLimit);
				if (!limit)
					throw ToolError(400, "limit must be an integer from 0 to 4294967295");
				json result = { { "ok", true }, { "value", ToJson(value, depth, *limit, budget) } };
				result["nodes"] = budget.nodes;
				result["truncated"] = budget.truncated;
				return result;
			}

			if (segments.size() < 2)
				throw ToolError(400, "set and call need a path with at least two segments");
			RE::GFxValue parent;
			Resolve(a_movie, segments, segments.size() - 1, parent);
			if (!parent.IsObject())
				throw ToolError(400, "not an object: " + segments[segments.size() - 2]);
			const auto& leaf = segments.back();

			if (op == "set") {
				if (!a_args.contains("value"))
					throw ToolError(400, "set needs a value");
				RE::GFxValue value;
				FromJson(a_movie, a_args["value"], value);
				const bool done = parent.IsArray() && GfxLogic::IsIndex(leaf) ?
				                      parent.SetElement(static_cast<std::uint32_t>(std::stoul(leaf)), value) :
				                      parent.SetMember(leaf.c_str(), value);
				if (!done)
					throw ToolError(422, "set failed: " + leaf);
				return { { "ok", true } };
			}

			const auto argsIt = a_args.find("args");
			if (argsIt != a_args.end() && !argsIt->is_array())
				throw ToolError(400, "args must be an array");
			std::vector<RE::GFxValue> args;
			if (argsIt != a_args.end())
				for (const auto& arg : *argsIt)
					FromJson(a_movie, arg, args.emplace_back());
			RE::GFxValue result;
			if (!parent.Invoke(leaf.c_str(), &result, args.data(), args.size()))
				throw ToolError(422, "call failed: " + leaf);
			return { { "ok", true }, { "value", ToJson(result, kDefaultDepth, kDefaultLimit, budget) } };
		}

		json Run(const json& a_args)
		{
			const auto menuName = a_args.value("menu", std::string{});
			if (menuName.empty())
				throw ToolError(400, "menu is required");
			auto* ui = RE::UI::GetSingleton();
			if (!ui)
				throw ToolError(409, "UI not ready");
			const auto menu = ui->GetMenu(menuName);
			if (!menu || !menu->uiMovie)
				throw ToolError(404, "menu not open: " + menuName);
			auto* movie = menu->uiMovie.get();

			if (a_args.value("op", std::string{}) != "batch")
				return RunOne(movie, a_args);

			// One UI task, so the steps cannot interleave with a frame. Stops at the first failure.
			const auto ops = a_args.value("ops", json::array());
			if (!ops.is_array() || ops.empty() || ops.size() > kMaxBatch)
				throw ToolError(400, std::format("batch needs an ops array of 1 to {} steps", kMaxBatch));
			json results = json::array();
			for (const auto& op : ops) {
				try {
					results.push_back(RunOne(movie, op));
				} catch (const std::exception& e) {
					// Earlier steps already acted, so the reply keeps their results; `scenario` reads ok=false as a failure.
					return { { "ok", false }, { "error", e.what() }, { "failedIndex", results.size() }, { "results", results } };
				}
			}
			return { { "ok", true }, { "results", results } };
		}
	}

	ToolDescriptor BuildDescriptor()
	{
		ToolDescriptor gfx;
		gfx.name = "gfx";
		gfx.description =
			"Read, set or call anything inside an open Scaleform (Flash) menu, such as RaceMenu, the HUD or a mod's menu, "
			"by a dotted path from _root or _global. Numbers in a path index arrays. op='get' reads a value "
			"(`truncated` is true when the limit, the depth or the node budget cut the result; methods read as {}), 'set' writes `value`, 'call' invokes a "
			"function with `args`. 'batch' runs `ops` in one UI task and stops at the first failure, returning `failedIndex`. "
			"set and call act on the live menu with no safety net: a native function given a wrong argument type can crash the game. Open menu names come from menu action='list'.";
		gfx.inputSchema = json{
			{ "type", "object" },
			{ "required", json::array({ "op", "menu" }) },
			{ "properties", json{
								{ "op", json{ { "type", "string" }, { "enum", json::array({ "get", "set", "call", "batch" }) } } },
								{ "menu", json{ { "type", "string" }, { "description", "Open menu name, for example 'RaceSex Menu'" } } },
								{ "path", json{ { "type", "string" }, { "description", "Dotted path from _root or _global" } } },
								{ "depth", json{ { "type", "integer" }, { "description", "get: levels to expand; default 2, maximum 8" } } },
								{ "limit", json{ { "type", "integer" }, { "description", "get: array items, 0 to 4294967295; default 500" } } },
								{ "value", json{ { "description", "set: the new value" } } },
								{ "args", json{ { "type", "array" }, { "description", "call: arguments; objects allowed" } } },
								{ "ops", json{ { "type", "array" }, { "items", json{ { "type", "object" } } }, { "description", "batch: up to 64 steps of op, path, value, args, depth, limit" } } },
							} },
		};
		return gfx;
	}

	json Handle(const json& a_args, const ToolContext&)
	{
		// The task owns its args: one that overruns the timeout finishes after this call returns.
		return MainThread::RunAndWait([args = a_args]() { return Run(args); }, std::chrono::milliseconds(5000), nullptr, MainThread::Queue::kUi);
	}
}
