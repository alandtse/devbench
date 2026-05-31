#include "HostApi.h"

#include "DevBenchAPI.h"
#include "EventBus.h"
#include "Json.h"
#include "ToolRegistry.h"
#include "Version.h"

namespace dvb::HostApi
{
	namespace
	{
		ToolRegistry* g_registry = nullptr;
		EventBus*     g_events = nullptr;

		// Concrete implementation of the published C-ABI interface, wired to the
		// host's registry + bus.
		struct Interface : DevBenchAPI::IDevBenchInterface001
		{
			unsigned int GetBuildNumber() override
			{
				return DEVBENCH_VERSION_MAJOR * 10000u + DEVBENCH_VERSION_MINOR * 100u + DEVBENCH_VERSION_PATCH;
			}

			bool RegisterTool(const char* a_name, const char* a_descriptorJson,
				DevBenchAPI::ToolFn a_handler, void* a_ctx) override
			{
				if (!g_registry || !a_name || !a_handler)
					return false;

				json desc = json::object();
				if (a_descriptorJson) {
					try {
						desc = json::parse(a_descriptorJson);
					} catch (...) {
					}
				}
				ToolDescriptor d;
				d.name = a_name;
				d.description = desc.value("description", std::string{});
				d.inputSchema = desc.value("inputSchema", json::object());
				d.readOnly = desc.value("readOnly", false);

				const auto  handler = a_handler;
				void* const ctx = a_ctx;
				// Wrap the C callback as a ToolHandler: pass args as a JSON string, collect
				// the result the consumer writes via our sink. Nothing C++ crosses the DLL
				// boundary — only const char* and function pointers.
				return g_registry->Register(std::move(d),
					[handler, ctx](const json& a_args, const ToolContext&) -> json {
						std::string          result;
						DevBenchAPI::WriteFn write = +[](void* a_sink, const char* a_json) {
							*static_cast<std::string*>(a_sink) = a_json ? a_json : "";
						};
						handler(ctx, a_args.dump().c_str(), &result, write);
						if (result.empty())
							return json::object();
						try {
							return json::parse(result);
						} catch (...) {
							return json{ { "raw", result } };
						}
					});
			}

			void EmitEvent(const char* a_topic, const char* a_payloadJson) override
			{
				if (!g_events || !a_topic)
					return;
				json payload = json::object();
				if (a_payloadJson) {
					try {
						payload = json::parse(a_payloadJson);
					} catch (...) {
					}
				}
				g_events->Publish(a_topic, std::move(payload));
			}
		};

		Interface g_interface;

		void* GetApi(unsigned int a_revision)
		{
			// Only revision 1 exists; future revisions return a derived interface.
			return a_revision >= 1 ? static_cast<DevBenchAPI::IDevBenchInterface001*>(&g_interface) : nullptr;
		}

		// Self-test: register a trivial tool THROUGH the public interface, proving the
		// C-callback + JSON round-trip path end to end without a separate consumer.
		void RegisterSelfTest()
		{
			static constexpr const char* desc =
				R"({"description":"devbench C-ABI self-test; echoes its args.","inputSchema":{"type":"object"},"readOnly":true})";
			g_interface.RegisterTool("ping", desc, +[](void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write) {
					const std::string args = (a_argsJson && *a_argsJson) ? a_argsJson : "{}";
					const std::string out = R"({"pong":true,"echo":)" + args + "}";
					a_write(a_sink, out.c_str()); }, nullptr);
		}
	}

	void Init(ToolRegistry& a_registry, EventBus& a_events)
	{
		g_registry = &a_registry;
		g_events = &a_events;
		RegisterSelfTest();
	}

	void OnInterfaceRequest(SKSE::MessagingInterface::Message* a_message)
	{
		if (a_message && a_message->type == DevBenchAPI::DevBenchMessage::kMessage_GetInterface && a_message->data) {
			static_cast<DevBenchAPI::DevBenchMessage*>(a_message->data)->GetApiFunction = GetApi;
			logs::info("devbench: provided plugin interface to {}", a_message->sender ? a_message->sender : "<?>");
		}
	}
}
