#pragma once

#include "Json.h"

#include <filesystem>
#include <string>
#include <vector>

// Per-frame world transforms of named scene-graph nodes under one reference, sampled on the main
// thread once per presented frame while a `record` is active and written beside the recording as
// newline-delimited JSON, so a plugin's own per-frame data can be diffed against it by frame number.
namespace dvb::NodeRecording
{
	/// Starts sampling a_names under a_formId (the player when empty). Main thread must not be the caller
	/// (it resolves the reference through MainThread::RunAndWait). Throws ToolError for a missing reference
	/// or a sampler that is already running.
	void Start(const std::string& a_formId, std::vector<std::string> a_names);

	/// True while sampling.
	bool IsActive();

	/// Stops sampling without writing anything. No-op when idle.
	void Cancel();

	/// Stops sampling and writes the frames to a_path (header line, then one line per frame). Returns
	/// { path, frames, nodes, truncated }. Throws std::runtime_error when the file cannot be written.
	json StopAndWrite(const std::filesystem::path& a_path);

	/// { active, frames, nodes, truncated } for `record status`.
	json Status();
}
