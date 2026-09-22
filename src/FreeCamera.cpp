#include "FreeCamera.h"

#include "MainThread.h"
#include "ToolRegistry.h"

#include <atomic>
#include <chrono>

namespace dvb::FreeCamera
{
	namespace
	{
		// VR's own ToggleFreeCameraMode never installs cameraStates[kFree] as currentState, so
		// activation/restoration is hand-rolled here instead. Flat's own toggle works correctly
		// (see SetEnabledFlat) and is used directly.
		RE::BSTSmartPointer<RE::TESCameraState> g_vrPreviousState;
		RE::BSTSmartPointer<RE::TESCameraState> g_vrFreeState;
		RE::PlayerCamera*                       g_vrOwner = nullptr;
		bool                                    g_vrLoadRecoveryPending = false;

		// Flat: no cached state pointer to go stale across a load -- the engine's own toggle
		// owns push/pop, so ownership is just "did devbench toggle this on", read live.
		RE::PlayerCamera* g_flatOwner = nullptr;

		std::atomic<SessionToken> g_session{ 0 };
		std::atomic<bool>         g_loading{ false };

		void ReleaseVR()
		{
			g_vrPreviousState.reset();
			g_vrFreeState.reset();
			g_vrOwner = nullptr;
		}

		bool RegisteredVR(RE::PlayerCamera* a_camera, RE::TESCameraState* a_state)
		{
			if (!a_state || a_state->camera != a_camera)
				return false;
			for (const auto& state : a_camera->GetVRRuntimeData()->cameraStates) {
				if (state.get() == a_state)
					return true;
			}
			return false;
		}

		void ValidateSession(SessionToken a_session)
		{
			if (g_loading.load() || a_session != g_session.load())
				throw ToolError(409, "camera request belongs to a loading or previous scene; read camera state and retry after loading");
		}

		RE::FreeCameraState* GetFreeState(RE::PlayerCamera* a_camera)
		{
			if (!a_camera)
				throw ToolError(422, "player camera is unavailable");
			// Same enum value on both runtimes (kFree sits before VR's kVR insertion point),
			// only the backing array differs.
			auto* state = REL::Module::IsVR() ?
			                  static_cast<RE::FreeCameraState*>(a_camera->GetVRRuntimeData()->cameraStates[RE::CameraState::kFree].get()) :
			                  static_cast<RE::FreeCameraState*>(a_camera->GetRuntimeData().cameraStates[RE::CameraState::kFree].get());
			if (!state || state->camera != a_camera || state->id != RE::CameraState::kFree)
				throw ToolError(422, "free-camera state is unavailable");
			return state;
		}

		void ReconcileVRLoadRecovery(RE::PlayerCamera* a_camera)
		{
			if (a_camera && a_camera->currentState && a_camera->currentState->id != RE::CameraState::kFree)
				g_vrLoadRecoveryPending = false;
		}

		bool RecoverVRAfterLoad()
		{
			if (!g_vrLoadRecoveryPending)
				return true;
			auto* camera = RE::PlayerCamera::GetSingleton();
			ReconcileVRLoadRecovery(camera);
			if (!g_vrLoadRecoveryPending)
				return true;
			auto* data = camera ? camera->GetVRRuntimeData() : nullptr;
			if (!data || !camera->currentState)
				return false;

			// Reacquire the loaded scene's normal VR state; no pre-load pointers survive.
			const auto freeState = data->cameraStates[RE::CameraState::kFree];
			const auto returnState = data->cameraStates[RE::CameraState::kVR];
			auto*      player = RE::PlayerCharacter::GetSingleton();
			if (camera->currentState != freeState || !RegisteredVR(camera, freeState.get()) ||
				!RegisteredVR(camera, returnState.get()) || returnState->id != RE::CameraState::kVR ||
				!camera->cameraRoot || !player || !player->Get3D() || !player->GetParentCell() ||
				!RE::PlayerControls::GetSingleton())
				return false;
			camera->SetState(returnState.get());
			g_vrLoadRecoveryPending = camera->currentState == freeState;
			return camera->currentState == returnState;
		}

		bool IsOwnedVR()
		{
			auto* camera = RE::PlayerCamera::GetSingleton();
			ReconcileVRLoadRecovery(camera);
			auto* data = camera ? camera->GetVRRuntimeData() : nullptr;
			if (!data || camera != g_vrOwner || !g_vrFreeState || camera->currentState != g_vrFreeState ||
				data->cameraStates[RE::CameraState::kFree] != g_vrFreeState || !RegisteredVR(camera, g_vrPreviousState.get())) {
				ReleaseVR();
				return false;
			}
			// A different mod leaving and reentering this exact singleton state between
			// observations is unobservable without an engine hook. Do not mix owners.
			return true;
		}

		bool IsOwnedFlat()
		{
			auto* camera = RE::PlayerCamera::GetSingleton();
			if (!camera || camera != g_flatOwner || !camera->IsInFreeCameraMode()) {
				g_flatOwner = nullptr;
				return false;
			}
			return true;
		}

		void SetEnabledVR(bool a_enabled)
		{
			if (!RecoverVRAfterLoad())
				throw ToolError(500, "VR camera recovery after loading failed; retry freecam off after the scene is ready");
			auto*      camera = RE::PlayerCamera::GetSingleton();
			auto*      freeState = GetFreeState(camera);
			const bool owned = IsOwnedVR();
			const bool active = camera->currentState.get() == freeState;
			if (active && !owned)
				throw ToolError(409, "VR free camera was not activated by devbench; preserve its existing owner");
			if (active == a_enabled)
				return;

			if (!a_enabled) {
				camera->SetState(g_vrPreviousState.get());
				if (camera->currentState != g_vrPreviousState)
					throw ToolError(500, "VR camera state restoration failed");
				ReleaseVR();
				return;
			}

			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!RegisteredVR(camera, camera->currentState.get()) || !camera->cameraRoot || !player || !player->Get3D() ||
				!player->GetParentCell() || !RE::PlayerControls::GetSingleton())
				throw ToolError(422, "VR free camera requires a loaded scene, registered camera state, and player controls");

			RE::NiQuaternion rotation{};
			RE::NiPoint3     translation{};
			// VR inserts a virtual slot before Update; universal builds must dispatch explicitly.
			REL::RelocateVirtual<decltype(&RE::TESCameraState::GetRotation)>(0x04, 0x05, camera->currentState.get(), rotation);
			REL::RelocateVirtual<decltype(&RE::TESCameraState::GetTranslation)>(0x05, 0x06, camera->currentState.get(), translation);
			freeState->translation = translation;

			// Preserve the native free-camera pitch/yaw convention instead of generic XYZ Euler angles.
			using SetRotation = void (*)(RE::FreeCameraState*, const RE::NiQuaternion*);
			static const REL::Relocation<SetRotation> setRotation{ REL::VariantID(0, 0, 0x873B50) };
			setRotation(freeState, &rotation);

			// Begin/End do not clear these button latches. A release while the handler
			// is inactive would otherwise leave the next activation moving by itself.
			freeState->zUpDown = {};
			freeState->verticalDirection = 0;
			freeState->useRunSpeed = false;

			// VR's native toggle dereferences a null state and never performs this transition.
			g_vrPreviousState = camera->currentState;
			g_vrFreeState = camera->GetVRRuntimeData()->cameraStates[RE::CameraState::kFree];
			g_vrOwner = camera;
			camera->SetState(freeState);
			if (camera->currentState != g_vrFreeState) {
				ReleaseVR();
				throw ToolError(500, "VR free-camera activation failed");
			}
		}

		void SetEnabledFlat(bool a_enabled)
		{
			auto*      camera = RE::PlayerCamera::GetSingleton();
			const bool active = camera && camera->IsInFreeCameraMode();
			if (active && !IsOwnedFlat())
				throw ToolError(409, "free camera was not activated by devbench; preserve its existing owner");
			if (active == a_enabled)
				return;

			auto* player = RE::PlayerCharacter::GetSingleton();
			if (a_enabled && (!camera || !camera->currentState || !camera->cameraRoot || !player || !player->Get3D() ||
								 !player->GetParentCell() || !RE::PlayerControls::GetSingleton()))
				throw ToolError(422, "free camera requires a loaded scene, registered camera state, and player controls");

			// The engine's own toggle pushes/pops the prior state itself, so there is no state
			// pointer to cache or restore here.
			camera->ToggleFreeCameraMode(false);
			if (camera->IsInFreeCameraMode() != a_enabled) {
				g_flatOwner = nullptr;
				throw ToolError(500, a_enabled ? "free-camera activation failed" : "free-camera restoration failed");
			}
			g_flatOwner = a_enabled ? camera : nullptr;
		}
	}

	SessionToken CurrentSession()
	{
		return g_session.load();
	}

	bool IsOwned()
	{
		if (!RE::PlayerCamera::GetSingleton())
			return false;
		return REL::Module::IsVR() ? IsOwnedVR() : IsOwnedFlat();
	}

	void SetEnabled(bool a_enabled, SessionToken a_session)
	{
		ValidateSession(a_session);
		if (REL::Module::IsVR())
			SetEnabledVR(a_enabled);
		else
			SetEnabledFlat(a_enabled);
	}

	void Drive(float a_x, float a_y, float a_z, float a_pitch, float a_yaw, SessionToken a_session)
	{
		ValidateSession(a_session);
		if (!IsOwned())
			throw ToolError(409, "camera drive requires a free camera activated by devbench in this scene");
		auto* state = GetFreeState(RE::PlayerCamera::GetSingleton());
		state->translation = RE::NiPoint3{ a_x, a_y, a_z };
		state->rotation.x = a_pitch;
		state->rotation.y = a_yaw;
	}

	void BeginLoad()
	{
		g_loading.store(true);
		g_session.fetch_add(1);
		if (!REL::Module::IsVR())
			return;
		if (IsOwnedVR()) {
			g_vrOwner->SetState(g_vrPreviousState.get());
			g_vrLoadRecoveryPending = g_vrOwner->currentState == g_vrFreeState;
			if (g_vrOwner->currentState != g_vrPreviousState)
				logs::warn("devbench: VR camera pre-load restoration failed; recovery will use the loaded scene's camera states");
		}
		ReleaseVR();
	}

	void EndLoad()
	{
		g_flatOwner = nullptr;
		g_session.fetch_add(1);
		if (REL::Module::IsVR()) {
			ReleaseVR();
			if (!RecoverVRAfterLoad())
				logs::warn("devbench: VR camera post-load recovery is pending; retry freecam off after the scene is ready");
		}
		g_loading.store(false);
	}

	void ReplayHold::Activate()
	{
		if (m_active)
			return;
		m_session = CurrentSession();
		MainThread::RunAndWait([this]() -> json {
			SetEnabled(true, m_session);
			return json{};
		});
		m_active = true;
	}

	ReplayHold::~ReplayHold()
	{
		if (!m_active)
			return;
		try {
			MainThread::RunAndWait([this]() -> json {
				SetEnabled(false, m_session);
				return json{};
			});
		} catch (const std::exception& e) {
			logs::warn("devbench: replay free-camera restore failed: {}", e.what());
		}
	}
}
