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
	json Handle(const json& a_args, EventBus& a_events);
}
