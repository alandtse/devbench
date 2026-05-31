#include "ModalCapture.h"

#include <atomic>

namespace
{
	std::atomic<RE::MessageBoxData*> g_data{ nullptr };

	// Detour on MessageBoxMenu::QueueMessage(MessageBoxData*). Every message box — the
	// load-content-mismatch confirmation included — is enqueued through here, so we
	// record the pointer and forward. No allocation, no lock: a single relaxed store.
	struct QueueMessageHook
	{
		static void thunk(RE::MessageBoxData* a_data)
		{
			g_data.store(a_data, std::memory_order_relaxed);
			func(a_data);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
}

namespace dvb::ModalCapture
{
	void Install()
	{
		// TEMPORARILY DISABLED: the write_branch<5> detour on QueueMessage
		// (RELOCATION_ID 51422/52271) CTDs the game when a message box is queued. Under
		// investigation (prologue/id verification) — re-enable once the safe hook is
		// confirmed. Until then Current() returns nullptr and menu describe/accept
		// report no modal, but the bench is otherwise unaffected.
		logs::warn("ModalCapture: QueueMessage hook disabled pending crash fix");
	}

	RE::MessageBoxData* Current() { return g_data.load(std::memory_order_relaxed); }
	void Clear() { g_data.store(nullptr, std::memory_order_relaxed); }
}
