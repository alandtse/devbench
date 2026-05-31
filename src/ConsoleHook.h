#pragma once

namespace dvb
{
	class EventBus;
}

// Console command observer. devbench is otherwise hook-free (it reads ConsoleLog's buffer rather
// than detouring the SEH-laden VPrint — see ConsoleLogCapture.h). This is the one deliberate
// exception: a trampoline on the console's command executor so EVERY command — typed into the
// in-game console by the user or issued programmatically — is observed. Each is published as a
// "console.command" event and, while a recording is active, captured into it (so a user setting
// values mid-record is reproduced on replay). The executor is a normal function (no SEH
// prologue), so the entry hook is safe, unlike VPrint.
namespace dvb::ConsoleHook
{
	/// Install the trampoline once. Idempotent. `a_events` must outlive the process (it is the
	/// server's EventBus). Safe to call after the server starts; the executor isn't reached until
	/// the player is in-game.
	void Install(EventBus& a_events);
}
