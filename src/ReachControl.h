#pragma once

#include "Json.h"
#include "ToolRegistry.h"

#include <string>

namespace dvb::ReachControl
{
	/// Starts an asynchronous reach run and returns {queued, runId, owner, contract}.
	json Start(const json& a_args, const std::string& a_owner, const ToolContext& a_ctx);
	/// Reports progress for a run (the latest run when 'runId' is omitted).
	json Status(const json& a_args);
	/// Requests the active run to stop; the owner must match.
	json Stop(const std::string& a_owner);
	/// True while a run is queued, calibrating, reaching or holding.
	bool Active();
	/// Stops any active run; called on game load and new game.
	void EndSession();
}
