#pragma once

#include "EventBus.h"
#include "Json.h"

// record: capture a manual play-through as a replayable scenario. `start` samples the
// player pose every intervalMs on a background thread (marshaling the read to the main
// thread) and captures a one-time scene manifest — the worldspace/cell, time of day, and
// weather a shader benchmark must reproduce. `stop` serializes the trajectory to a
// scenario file (consumable by the scenario runner) and returns its path. `status` reports
// progress. Emits record.started / record.stopped on the EventBus as scenario markers.
namespace dvb::Recording
{
	/// record tool handler. action = start | stop | status. Needs the EventBus to emit
	/// the start/stop marker events; bound with a capturing lambda at registration.
	/// (action=replay is handled at the registration site, which has the registry to run
	/// the assembled scenario — see BuildReplaySteps.)
	json Handle(const json& a_args, EventBus& a_events);

	/// Note how the player got into the current scene, so a recording started afterward can
	/// stamp a reproducible entry point into its manifest. Called by the game/console tools
	/// when they broker a load or a `coc`/`cow`. Last one wins.
	void NoteLoadEntry(const std::string& a_saveName);
	void NoteCocEntry(const std::string& a_cellId);

	/// Build the step list for action=replay from a recording file (a_args.path). With
	/// a_args.restoreScene=true, prepend the entry point (load the save / coc the cell) +
	/// waitUntil playerLoaded so the trajectory runs in the recorded scene. Throws ToolError
	/// on a missing/invalid file. The caller runs the result through the scenario tool.
	json BuildReplaySteps(const json& a_args);
}
