// SPDX-License-Identifier: MIT
//
// This interface header and its companion DevBenchAPI.cpp are MIT-licensed (see
// DevBenchAPI.LICENSE.txt) so ANY SKSE plugin — including proprietary/closed-source —
// may vendor them to talk to devbench, independent of the devbench plugin's GPL-3.0.
// They are self-contained: drop both files into your plugin (or consume the
// devbench-api vcpkg port) and build — no other devbench source is needed.
#pragma once

#include <cstdint>

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

// devbench cross-plugin API — lets another SKSE plugin register MCP/REST tools and
// emit events into the running devbench host. Usage: after SKSE sends your plugin
// kPostLoad, request the interface via an SKSE messaging dispatch, then call through
// the versioned abstract interface below.
//
// The call direction is reversed from a typical query API: you hand devbench a handler
// that it calls back when a tool is invoked. So payloads are JSON strings and the
// handler is a plain C function pointer + void* ctx — never std::function or other C++
// objects across the DLL boundary.
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

	// Message used to fetch the interface (a fixed, randomly chosen id).
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

		// Register a handler for a custom menu, dispatched through the base `menu` tool as
		// action='invoke', name='<menuName>'. Thin alias for RegisterToolExtension("menu", …) — kept
		// for the 1.4.0 ABI. Same handler contract as RegisterTool. Returns false if it replaced an
		// existing handler. ABI: vtable slot exists only on hosts that ship it — call only when
		// GetBuildNumber() >= 10400 (devbench 1.4.0).
		virtual bool RegisterMenuHandler(const char* a_menuName, const char* a_descriptorJson,
			ToolFn a_handler, void* a_ctx) = 0;

		// Register a handler that EXTENDS a built-in base tool, keyed by a string, so a mod adds a
		// sub-capability WITHOUT a top-level tool (the agent-facing surface stays small). The base
		// tool routes to it: `menu invoke name='<key>'`, `inspect kind='<key>'`. Same handler
		// contract as RegisterTool (args JSON in — the full base-tool args — result JSON out, runs on
		// the listener thread; marshal to the main thread yourself). a_descriptorJson
		// { "description", … } is surfaced by the base tool's discovery path (`menu describe`,
		// `inspect kind=extensions`). Opted-in base tools: `menu`, `inspect`. Returns false if it
		// replaced an existing (baseTool, key) entry.
		//
		// ABI: appended after RegisterMenuHandler — call only when GetBuildNumber() >= 10500
		// (devbench 1.5.0).
		virtual bool RegisterToolExtension(const char* a_baseTool, const char* a_key,
			const char* a_descriptorJson, ToolFn a_handler, void* a_ctx) = 0;

		// Set the game's global time scale — the same control as `game setTimeScale`, so a consumer
		// can speed up or slow down a run (e.g. 3.0 to finish a benchmark sooner). a_scale is
		// 0.1..3.0; a_leaseMs is how long it stays in effect before the previous scale is restored
		// (0 = 60000), so an interrupted consumer still leaves the game at normal speed.
		// NON-BLOCKING and callable from ANY thread (including the main thread): the change is
		// applied on a later main-thread frame; there is no RunAndWait. Returns false when the scale
		// is out of range, or when a recording/capture is in flight and would be made incomparable.
		// a_owner identifies the lease for `game getTimeScale` (nullptr = an anonymous owner).
		//
		// ABI: appended after RegisterToolExtension — call only when GetBuildNumber() >= 12000
		// (devbench 1.20.0).
		virtual bool SetTimeScale(float a_scale, std::uint32_t a_leaseMs, const char* a_owner) = 0;

		// The global time multiplier the game is running at right now (what the engine has actually
		// reached, not a value a request left pending). 1.0 when nothing has changed it.
		//
		// ABI: appended after SetTimeScale — call only when GetBuildNumber() >= 12000
		// (devbench 1.20.0).
		virtual float GetTimeScale() = 0;
	};
}

extern DevBenchAPI::IDevBenchInterface001* g_devBenchInterface;
