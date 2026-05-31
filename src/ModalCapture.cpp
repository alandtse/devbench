#include "ModalCapture.h"

#include <atomic>
#include <mutex>

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
		static std::once_flag once;
		std::call_once(once, []() {
			// MessageBoxMenu::QueueMessage — RelocationID covers SE/AE and maps to VR via
			// the address library (the same id CommonLibSSE-NG uses for this function).
			REL::Relocation<std::uintptr_t> target{ REL::RelocationID(51422, 52271) };
			auto& trampoline = SKSE::GetTrampoline();
			QueueMessageHook::func = trampoline.write_branch<5>(target.address(), &QueueMessageHook::thunk);
			logs::info("ModalCapture: hooked MessageBoxMenu::QueueMessage");
		});
	}

	RE::MessageBoxData* Current() { return g_data.load(std::memory_order_relaxed); }
	void Clear() { g_data.store(nullptr, std::memory_order_relaxed); }
}
