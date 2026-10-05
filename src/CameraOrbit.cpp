#include "CameraOrbit.h"

#include <RE/L/LoadingMenu.h>
#include <RE/P/PlayerCamera.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/ThirdPersonState.h>
#include <RE/U/UI.h>

#include <atomic>

namespace dvb::CameraOrbit
{
	namespace
	{
		OrbitState                 g_state;  // main thread
		std::atomic<std::uint64_t> g_session{ 1 };

		Vec3         FromNi(const RE::NiPoint3& a_p) { return { a_p.x, a_p.y, a_p.z }; }
		RE::NiPoint3 ToNi(const Vec3& a_v) { return { a_v.x, a_v.y, a_v.z }; }

		CameraFields Read(const RE::ThirdPersonState* a_state)
		{
			return { a_state->targetYaw, a_state->currentYaw, a_state->targetZoomOffset, a_state->currentZoomOffset,
				FromNi(a_state->posOffsetExpected), FromNi(a_state->posOffsetActual), a_state->freeRotation.x };
		}

		void WriteCamera(RE::ThirdPersonState* a_state, const CameraFields& a_fields)
		{
			a_state->targetYaw = a_fields.targetYaw;
			a_state->currentYaw = a_fields.currentYaw;
			a_state->targetZoomOffset = a_fields.targetZoom;
			a_state->currentZoomOffset = a_fields.currentZoom;
			a_state->posOffsetExpected = ToNi(a_fields.offsetExpected);
			a_state->posOffsetActual = ToNi(a_fields.offsetActual);
			a_state->freeRotation.x = a_fields.freeRotationX;
		}

		// The live third-person state, or null when the camera is in another state.
		RE::ThirdPersonState* LiveThirdPerson()
		{
			auto* cam = RE::PlayerCamera::GetSingleton();
			if (!cam || !cam->currentState || cam->currentState->id != RE::CameraState::kThirdPerson)
				return nullptr;
			return static_cast<RE::ThirdPersonState*>(cam->currentState.get());
		}

		// The player's facing is held at the baseline: with a weapon or spell drawn and no attack held, the player
		// otherwise turns to face wherever the camera looks, and a camera in front would chase the player round.
		void Apply(RE::ThirdPersonState* a_state)
		{
			auto* pc = RE::PlayerCharacter::GetSingleton();
			if (!pc)
				return;
			const auto write = g_state.Apply(Baseline{ pc->data.angle.z, pc->data.angle.x, Read(a_state) });
			if (!write)
				return;
			pc->data.angle.z = write->playerYaw;
			if (write->playerPitch)
				pc->data.angle.x = *write->playerPitch;  // the third-person tilt follows the player's look pitch
			// The game turns the camera round the player by one of two values: the free-rotation offset while free
			// rotation is on (weapon sheathed), else the camera's own heading. Writing both turns it twice.
			auto camera = write->camera;
			if (a_state->freeRotationEnabled) {
				camera.targetYaw = a_state->targetYaw;
				camera.currentYaw = a_state->currentYaw;
			} else {
				camera.freeRotationX = a_state->freeRotation.x;
			}
			WriteCamera(a_state, camera);
		}

		bool EndInto(EndReason a_reason, RE::ThirdPersonState* a_state)
		{
			if (!g_state.On())
				return false;
			const CameraFields current = a_state ? Read(a_state) : CameraFields{};
			const auto         restore = g_state.End(a_reason, current);
			if (restore) {
				if (a_state)
					WriteCamera(a_state, restore->camera);
				if (restore->playerPitch)
					if (auto* pc = RE::PlayerCharacter::GetSingleton())
						pc->data.angle.x = *restore->playerPitch;
			}
			return true;
		}

		struct Update
		{
			static void thunk(RE::ThirdPersonState* a_state, RE::BSTSmartPointer<RE::TESCameraState>& a_next)
			{
				const std::uint64_t revision = g_state.Revision();
				const bool          on = g_state.On();
				if (on)
					Apply(a_state);
				func(a_state, a_next);
				// the update itself resets the heading behind a drawn weapon; re-apply only the same configuration
				if (on && g_state.On() && g_state.Revision() == revision)
					Apply(a_state);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// Leaving the third-person state for any other (a POV switch, the free camera, ...) ends the orbit while this
		// state is still valid to restore into, so it never lies latent and comes back on a later return.
		struct EndState
		{
			static void thunk(RE::ThirdPersonState* a_state)
			{
				EndInto(EndReason::kLeftThirdPerson, a_state);
				func(a_state);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Install()
	{
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_ThirdPersonState[0] };
		Update::func = vtbl.write_vfunc(REL::Module::IsVR() ? 0x04 : 0x03, Update::thunk);
		EndState::func = vtbl.write_vfunc(0x02, EndState::thunk);
	}

	std::uint64_t CurrentSession() { return g_session.load(std::memory_order_acquire); }

	Admission Enable(const Request& a_request, std::uint64_t a_session)
	{
		// ThirdPersonState is larger on VR (0x100 against 0xE8) and its fields' VR offsets are not mapped, so the
		// orbit never writes them there.
		if (REL::Module::IsVR())
			return Admission::kUnsupportedRuntime;
		auto*      ui = RE::UI::GetSingleton();
		auto*      cam = RE::PlayerCamera::GetSingleton();
		const bool loading = !ui || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) || !RE::PlayerCharacter::GetSingleton();
		const bool freeCamera = cam && cam->IsInFreeCameraMode();
		return g_state.Enable(a_request, a_session, loading, freeCamera);
	}

	bool End(EndReason a_reason) { return EndInto(a_reason, LiveThirdPerson()); }

	void EndSession()
	{
		g_state.EndSession();
		g_session.store(g_state.Session(), std::memory_order_release);
	}

	const OrbitState& State() { return g_state; }
}
