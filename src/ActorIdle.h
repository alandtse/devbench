#pragma once

#include "Json.h"

#include <cstdint>
#include <string>

namespace RE
{
	class Actor;
	class TESIdleForm;
}

namespace dvb::ActorIdle
{
	// Plays an idle on a loaded NPC, optionally keeping it playing, and returns the NPC to its default idle. All of these
	// run on the main thread. A looping idle is re-issued on a timer (the engine does not report when an idle ends), so
	// a one-shot clip repeats. Loops end when the actor unloads,
	// dies, is asked to stop, or a load / new game starts.

	/// Throws ToolError (409) for an unloaded or dead actor, the player, or an actor with no AI process.
	json Play(RE::Actor* a_actor, RE::TESIdleForm* a_idle, bool a_loop, std::int64_t a_repeatMs, bool a_freezeAi);

	/// Ends any loop on the actor and returns it to its default idle. a_freezeAi turns its AI back off once it settles.
	json Stop(RE::Actor* a_actor, bool a_freezeAi);

	/// The idles the actor can use now (its own conditions pass), optionally filtered by an EditorID substring.
	json List(RE::Actor* a_actor, const std::string& a_filter, std::size_t a_limit);

	/// The idle the actor is running now (null when none) and whether a loop is active on it.
	json Status(RE::Actor* a_actor);

	/// Main thread, at a load or new game.
	void EndSession();
}
