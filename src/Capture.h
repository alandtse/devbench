#pragma once

#include <filesystem>

#include "Json.h"
#include "ToolRegistry.h"  // ToolContext, ToolDescriptor

namespace dvb
{
	class EventBus;
	struct Config;
}

// capture: take a screenshot at a replay checkpoint (or ad hoc), and return its path plus
// correlation metadata. Devbench never scores images — that is entirely an external (Python)
// concern; this tool's only job is capture + a reliable "the file is ready" signal.
//
// Capture is deliberately PLUGIN-AGNOSTIC: devbench does not implement its own D3D11/image-codec
// capture pipeline (real graphics-engine code it doesn't own anywhere else). Instead it defines a
// capability CONTRACT any consumer plugin can implement via the existing C-ABI
// `RegisterToolExtension("capture", <key>, …)` mechanism — Open Shaders is the reference
// implementation, not a hardcoded dependency. A small `kind=native` fallback exists for when no
// provider is registered: a main-thread flag flip against vanilla's own `MenuControls::
// QueueScreenshot()` plus a directory poll — no D3D11, no image codec, deliberately lower quality
// than any registered provider (see Handle's kind-resolution rule).
namespace dvb::Capture
{
	json Handle(const json& a_args, const ToolContext& a_ctx);

	/// `inspect kind=screenshots` — list image files sitting in the vanilla screenshot
	/// directories (game root + `Screenshots/`), independent of the `capture` tool. Shares the
	/// directory-snapshot logic the native fallback needs, and lets a human/agent discover where
	/// vanilla actually writes on this install (see the GameRoot() assumption below) in one call.
	json ListScreenshots(const json& a_args);

	/// The game install directory (the running executable's parent directory). Vanilla
	/// screenshots and Open Shaders' own default capture path both resolve relative paths here,
	/// not against Documents/My Games (that's SKSE's log/save convention, a different one).
	std::filesystem::path GameRoot();

	ToolDescriptor BuildCaptureDescriptor();

	/// Wired from RegisterCoreTools/main.cpp before the tool is reachable.
	void SetEvents(EventBus* a_events);
	void SetDefaults(const Config& a_cfg);
}
