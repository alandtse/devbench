// SPDX-License-Identifier: MIT
//
// This interface header and its companion DevBenchAPI.cpp are licensed MIT (see
// DevBenchAPI.LICENSE.txt) so ANY SKSE plugin — including proprietary/non-GPL mods —
// may vendor them to talk to devbench. This is independent of the devbench *plugin*,
// which is GPL-3.0. Matches the MergeMapper / SkyrimVRESL convention.
#pragma once

#include <cstdint>

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

// devbench cross-plugin API — lets another SKSE plugin register MCP/REST tools and
// emit events into the running devbench host. Mechanism follows MergeMapper /
// SkyrimVRESL (HIGGS-derived): after SKSE sends your plugin kPostLoad, request the
// interface via a messaging dispatch, then call through a versioned abstract
// interface. Copy this header AND DevBenchAPI.cpp into your plugin.
//
// Why the shape differs from MergeMapper's fixed getters: tools are dynamic and the
// call direction is reversed — you hand devbench a handler it calls back. So the
// payloads are JSON strings and the handler is a plain C function pointer + void* ctx
// (never std::function / C++ objects across the DLL boundary).
namespace DevBenchAPI
{
	constexpr const auto DevBenchPluginName = "devbench";

	// Host-owned writer a handler uses to return its JSON result. Call exactly once
	// before returning; the host owns the buffer (so result-string ownership never
	// crosses the DLL boundary).
	using WriteFn = void (*)(void* a_sink, const char* a_resultJson);

	// A tool handler. Invoked on devbench's server (listener) thread — marshal to the
	// main game thread yourself if you touch game state. `a_argsJson` is the tool's
	// arguments object as a JSON string; return your result via a_write(a_sink, json).
	// MUST be a plain C function / captureless lambda.
	using ToolFn = void (*)(void* a_ctx, const char* a_argsJson, void* a_sink, WriteFn a_write);

	// Message used to fetch the interface (randomly chosen, collision-free with
	// MergeMapper 0xe6cb8b59 / SkyrimVRESL 0xeacb2bef).
	struct DevBenchMessage
	{
		enum : std::uint32_t
		{
			kMessage_GetInterface = 0x9a3f1c08
		};
		void* (*GetApiFunction)(unsigned int a_revisionNumber) = nullptr;
	};

	struct IDevBenchInterface001;
	// Call only after SKSE sends kPostLoad. Returns nullptr if devbench is absent.
	IDevBenchInterface001* GetDevBenchInterface001();

	struct IDevBenchInterface001
	{
		// devbench build number: VERSION_MAJOR*10000 + MINOR*100 + PATCH.
		virtual unsigned int GetBuildNumber() = 0;

		// Register a tool, exposed over both MCP (/mcp) and REST (/api/tool/<name>).
		// a_descriptorJson: { "description": str, "inputSchema": obj, "readOnly": bool }.
		// Returns false if a tool of this name already existed (it is still replaced).
		virtual bool RegisterTool(const char* a_name, const char* a_descriptorJson,
			ToolFn a_handler, void* a_ctx) = 0;

		// Publish an event (MCP notification + REST /api/events?since=N).
		virtual void EmitEvent(const char* a_topic, const char* a_payloadJson) = 0;
	};
}

extern DevBenchAPI::IDevBenchInterface001* g_devBenchInterface;
