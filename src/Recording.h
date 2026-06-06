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

	/// Record a console command observed mid-recording (from the console hook), stamped with the
	/// current frame so BuildScenario replays it at the point in the trajectory it was issued.
	/// No-op unless a recording is active. Called on the main thread (the hook runs there).
	/// coc/cow are skipped here — cell transitions are captured via NoteCellChange instead.
	void NoteConsoleCommand(const std::string& a_command);

	/// Record a mid-recording cell transition (from the cell-load event sink). `a_command` is the
	/// pre-built reproducible command the caller derived from the destination: `coc <interior>`
	/// (unique editor id) or `cow <worldspace> <gx> <gy>` for exteriors (whose editor ids aren't
	/// unique across worldspaces, so coc is ambiguous). The trajectory's setpos refines the spot.
	/// Single source of truth for transitions (door, coc, fast-travel). No-op unless recording.
	void NoteCellChange(const std::string& a_command);

	/// Mark whether devbench is currently replaying (teleporting the player). While true, the
	/// pose sampler skips ticks — the replay's own setpos commands (captured via the console
	/// hook) are the trajectory — so a recording that plays back a recipe embeds it cleanly.
	void SetReplaying(bool a_replaying);

	/// Default settle delay (ms) inserted after a restore-load before the trajectory, so the
	/// game settles before the player is teleported. Local/per-machine (set from config);
	/// a replay call's settleMs arg overrides it.
	void SetLoadSettleMs(int a_ms);

	/// Default sample interval (ms) for record `start` when no intervalMs arg is given (e.g. the
	/// hotkey). Local/per-machine via config (recordIntervalMs). Clamped to the 10ms floor.
	void SetDefaultIntervalMs(int a_ms);

	/// Scene-coupling defaults from config: the age thresholds (ms) that map a recipe's
	/// entryPoint.ageMs to the anchored/cell/worldspace tier, and the clean-transition policy
	/// (bounce a coc/cow restore through a neutral cell to force a loading-screen teardown).
	/// A recipe's meta.coupling block and a replay call's args override these.
	void SetCoupling(int a_anchorMs, int a_cellMs, bool a_cleanTransition, const std::string& a_transitionCell);

	/// Show a corner HUD message (marshaled to the main thread). Used for hotkey feedback
	/// (record start/stop, replay) since devbench is otherwise headless. No-op if no task interface.
	void Notify(const std::string& a_msg);

	/// Build the replay plan for action=replay from a recording file (a_args.path). Returns
	/// { steps, coupling } — `steps` runs through the scenario tool; `coupling` reports the
	/// effective tier. With a_args.restoreScene=true, the steps prepend the entry point (load
	/// the save / coc the cell) + waitUntil playerLoaded so the trajectory runs in the recorded
	/// scene. The recipe's tier is the producer's signal; a_args.coupling overrides it (run
	/// looser) and a_args.force turns the scene mismatch from an abort into a reported warning.
	/// Throws ToolError on a missing/invalid file or an invalid coupling override.
	json BuildReplaySteps(const json& a_args);
}
