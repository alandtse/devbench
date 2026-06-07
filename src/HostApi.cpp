#include "HostApi.h"

#include "DevBenchAPI.h"
#include "EventBus.h"
#include "Json.h"
#include "ToolExtensions.h"
#include "ToolRegistry.h"
#include "Version.h"

namespace dvb::HostApi
{
	namespace
	{
		ToolRegistry* g_registry = nullptr;
		EventBus*     g_events = nullptr;

		// Wrap a consumer's C callback (fn + ctx) as a ToolHandler: args in as a JSON string, result
		// collected via our host-owned sink. Nothing C++ crosses the DLL boundary — only const char*
		// and function pointers. Shared by RegisterTool and RegisterMenuHandler.
		ToolHandler MakeHandler(DevBenchAPI::ToolFn a_handler, void* a_ctx)
		{
			return [a_handler, a_ctx](const json& a_args, const ToolContext&) -> json {
				std::string          result;
				DevBenchAPI::WriteFn write = +[](void* a_sink, const char* a_json) {
					*static_cast<std::string*>(a_sink) = a_json ? a_json : "";
				};
				a_handler(a_ctx, a_args.dump().c_str(), &result, write);
				if (result.empty())
					return json::object();
				try {
					return json::parse(result);
				} catch (...) {
					return json{ { "raw", result } };
				}
			};
		}

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

				return g_registry->Register(std::move(d), MakeHandler(a_handler, a_ctx));
			}

			bool RegisterMenuHandler(const char* a_menuName, const char* a_descriptorJson,
				DevBenchAPI::ToolFn a_handler, void* a_ctx) override
			{
				// Menus are just the first base tool that accepts extensions — delegate.
				return RegisterToolExtension("menu", a_menuName, a_descriptorJson, a_handler, a_ctx);
			}

			bool RegisterToolExtension(const char* a_baseTool, const char* a_key, const char* a_descriptorJson,
				DevBenchAPI::ToolFn a_handler, void* a_ctx) override
			{
				if (!a_baseTool || !*a_baseTool || !a_key || !*a_key || !a_handler)
					return false;
				json desc = json::object();
				if (a_descriptorJson) {
					try {
						desc = json::parse(a_descriptorJson);
					} catch (...) {
					}
				}
				if (!desc.is_object())  // a scalar/array descriptor would break the descriptor object contract
					desc = json::object();
				return ToolExtensions::Register(a_baseTool, a_key, std::move(desc), MakeHandler(a_handler, a_ctx));
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

		// Shared echo handler for the extension self-tests (the ping pattern, one level down).
		void EchoExtension(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
		{
			const std::string args = (a_argsJson && *a_argsJson) ? a_argsJson : "{}";
			const std::string out = R"({"invoked":true,"echo":)" + args + "}";
			a_write(a_sink, out.c_str());
		}

		// Self-test the extension path THROUGH the public interface under two base tools — `menu`
		// (via the RegisterMenuHandler alias) and `inspect` (via the general RegisterToolExtension) —
		// so `menu invoke name=devbench.selftest` and `inspect kind=devbench.selftest` round-trip the
		// C-callback without a separate consumer mod, proving the mechanism generalizes.
		void RegisterExtensionSelfTests()
		{
			g_interface.RegisterMenuHandler("devbench.selftest",
				R"({"description":"devbench menu-extension self-test; echoes its args."})", &EchoExtension, nullptr);
			g_interface.RegisterToolExtension("inspect", "devbench.selftest",
				R"({"description":"devbench inspect-extension self-test; echoes its args."})", &EchoExtension, nullptr);
		}
	}

	void Init(ToolRegistry& a_registry, EventBus& a_events)
	{
		g_registry = &a_registry;
		g_events = &a_events;
		RegisterSelfTest();
		RegisterExtensionSelfTests();
	}

	void OnInterfaceRequest(SKSE::MessagingInterface::Message* a_message)
	{
		if (a_message && a_message->type == DevBenchAPI::DevBenchMessage::kMessage_GetInterface && a_message->data) {
			static_cast<DevBenchAPI::DevBenchMessage*>(a_message->data)->GetApiFunction = GetApi;
			logs::info("devbench: provided plugin interface to {}", a_message->sender ? a_message->sender : "<?>");
		}
	}
}
