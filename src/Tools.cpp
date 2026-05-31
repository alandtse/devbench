#include "Tools.h"

#include "ConsoleLogCapture.h"
#include "Json.h"
#include "MainThread.h"
#include "ToolRegistry.h"
#include "Version.h"

namespace dvb
{
	namespace
	{
		bool Truthy(const json& a_v)
		{
			if (a_v.is_boolean())
				return a_v.get<bool>();
			if (a_v.is_string()) {
				const auto s = a_v.get<std::string>();
				return s == "true" || s == "1";
			}
			if (a_v.is_number())
				return a_v.get<double>() != 0.0;
			return false;
		}

		// console: run a Skyrim console command; optionally fence + capture its output.
		json ConsoleHandler(const json& a_args, const ToolContext&)
		{
			const std::string action = a_args.value("action", std::string("exec"));

			if (action == "read") {
				const auto lines = ConsoleLogCapture::Snapshot(200);
				const bool markersFound = ConsoleLogCapture::SawBegin() && ConsoleLogCapture::SawEnd();
				json arr = json::array();
				for (const auto& l : lines)
					arr.push_back(json{ { "seq", l.seq }, { "frame", l.frame }, { "text", l.text } });
				json out{
					{ "markersFound", markersFound },
					{ "sawBegin", ConsoleLogCapture::SawBegin() },
					{ "sawEnd", ConsoleLogCapture::SawEnd() },
					{ "count", arr.size() },
					{ "headSeq", ConsoleLogCapture::HeadSeq() },
					{ "lines", std::move(arr) },
				};
				ConsoleLogCapture::EndCapture();  // close the window opened by exec(capture)
				return out;
			}
			if (action != "exec")
				throw ToolError(400, std::format("unknown action '{}'", action));

			const std::string command = a_args.value("command", std::string{});
			if (command.empty())
				throw ToolError(400, "missing required parameter 'command'");

			auto* task = SKSE::GetTaskInterface();
			if (!task)
				throw ToolError(500, "SKSE TaskInterface unavailable");

			const bool capture = a_args.contains("capture") && Truthy(a_args["capture"]);
			if (capture)
				ConsoleLogCapture::BeginCapture();

			// ExecuteCommand is deferred (GFx console drains queued commands on a later
			// tick). Fence the real command between two invalid marker commands; the
			// detour records only what lands between their echoed tokens. Capture
			// `command` by value so it outlives this lambda.
			task->AddTask([command, capture]() {
				if (capture)
					RE::Console::ExecuteCommand(ConsoleLogCapture::kMarkerBegin);
				RE::Console::ExecuteCommand(command.c_str());
				if (capture)
					RE::Console::ExecuteCommand(ConsoleLogCapture::kMarkerEnd);
			});

			return json{ { "queued", true }, { "command", command }, { "capturing", capture } };
		}

		// inspect: read live game state. Demonstrates the value-returning primitive —
		// the read runs on the main thread and its result is returned synchronously.
		json InspectHandler(const json& a_args, const ToolContext&)
		{
			const std::string kind = a_args.value("kind", std::string("state"));
			if (kind != "state")
				throw ToolError(400, std::format("unknown kind '{}'", kind));

			return MainThread::RunAndWait([]() -> json {
				auto* pc = RE::PlayerCharacter::GetSingleton();
				const bool loaded = pc && pc->Get3D() != nullptr;
				return json{
					{ "plugin", "devbench" },
					{ "version", DEVBENCH_VERSION_STRING },
					{ "vr", REL::Module::IsVR() },
					{ "playerLoaded", loaded },
				};
			});
		}
	}

	void RegisterCoreTools(ToolRegistry& a_registry)
	{
		ToolDescriptor console;
		console.name = "console";
		console.description =
			"Run a Skyrim console command. Fire-and-forget by default (queued onto the "
			"main thread, runs next tick). With capture=true the command is fenced between "
			"marker commands so action='read' returns ONLY its output (markersFound=true), "
			"surviving heavy ConsoleLog spam. Useful for printing commands (getav, getgs, help).";
		console.inputSchema = json{
			{ "type", "object" },
			{ "properties", json{
									 { "action", json{ { "type", "string" }, { "enum", json::array({ "exec", "read" }) }, { "description", "'exec' (default) runs `command`; 'read' returns the fenced output and closes the window" } } },
									 { "command", json{ { "type", "string" }, { "description", "the console command, exactly as typed after ~ (required for exec)" } } },
									 { "capture", json{ { "type", "boolean" }, { "description", "exec: fence and capture this command's output for the next read" } } },
								 } },
		};
		a_registry.Register(std::move(console), &ConsoleHandler);

		ToolDescriptor inspect;
		inspect.name = "inspect";
		inspect.description =
			"Read live game/plugin state. Runs on the main thread and returns the value "
			"synchronously (times out if the game is mid-load / not pumping tasks).";
		inspect.readOnly = true;
		inspect.inputSchema = json{
			{ "type", "object" },
			{ "properties", json{
									 { "kind", json{ { "type", "string" }, { "enum", json::array({ "state" }) }, { "description", "'state' → { plugin, version, vr, playerLoaded }" } } },
								 } },
		};
		a_registry.Register(std::move(inspect), &InspectHandler);
	}
}
