#include "VanityCamera.h"

namespace dvb::VanityCamera
{
	namespace
	{
		// Replaces Update's idle-timeout call, so only the idle trigger is suppressed; an explicit
		// PushCameraState(kAutoVanity) never goes through here.
		struct EnterAutoVanity
		{
			static void thunk(RE::PlayerCamera*) {}
		};
	}

	void Install()
	{
		// Checked in Ghidra: the call is at +0x15E in SE 1.5.97 and +0x154 in AE 1.6.1170 and 1.7.104. VR is not mapped.
		if (REL::Module::IsVR()) {
			logs::warn("devbench: disableVanityCamera is not supported on VR");
			return;
		}
		REL::Relocation<std::uintptr_t> update{ RELOCATION_ID(49852, 50784), REL::Relocate(0x15E, 0x154) };
		auto&                           trampoline = SKSE::GetTrampoline();
		trampoline.write_call<5>(update.address(), EnterAutoVanity::thunk);
	}
}
