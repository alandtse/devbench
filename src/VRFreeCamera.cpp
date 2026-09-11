#include "VRFreeCamera.h"

#include "ToolRegistry.h"

#include <atomic>

namespace dvb::VRFreeCamera
{
	namespace
	{
		RE::BSTSmartPointer<RE::TESCameraState> g_previousState;
		RE::BSTSmartPointer<RE::TESCameraState> g_freeState;
		RE::PlayerCamera*                       g_owner = nullptr;
		std::atomic<SessionToken>               g_session{ 0 };
		std::atomic<bool>                       g_loading{ false };
		bool                                    g_loadRecoveryPending = false;

		void Release()
		{
			g_previousState.reset();
			g_freeState.reset();
			g_owner = nullptr;
		}

		bool Registered(RE::PlayerCamera* a_camera, RE::TESCameraState* a_state)
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
				throw ToolError(409, "VR camera request belongs to a loading or previous scene; read camera state and retry after loading");
		}

		RE::FreeCameraState* GetFreeState(RE::PlayerCamera* a_camera)
		{
			auto* data = a_camera ? a_camera->GetVRRuntimeData() : nullptr;
			if (!data)
				throw ToolError(422, "VR player camera is unavailable");
			auto* state = static_cast<RE::FreeCameraState*>(data->cameraStates[RE::CameraState::kFree].get());
			if (!state || state->camera != a_camera || state->id != RE::CameraState::kFree)
				throw ToolError(422, "VR free-camera state is unavailable");
			return state;
		}

		void ReconcileLoadRecovery(RE::PlayerCamera* a_camera)
		{
			if (a_camera && a_camera->currentState && a_camera->currentState->id != RE::CameraState::kFree)
				g_loadRecoveryPending = false;
		}

		bool RecoverAfterLoad()
		{
			if (!g_loadRecoveryPending)
				return true;
			auto* camera = RE::PlayerCamera::GetSingleton();
			ReconcileLoadRecovery(camera);
			if (!g_loadRecoveryPending)
				return true;
			auto* data = camera ? camera->GetVRRuntimeData() : nullptr;
			if (!data || !camera->currentState)
				return false;

			// Reacquire the loaded scene's normal VR state; no pre-load pointers survive.
			const auto freeState = data->cameraStates[RE::CameraState::kFree];
			const auto returnState = data->cameraStates[RE::CameraState::kVR];
			auto*      player = RE::PlayerCharacter::GetSingleton();
			if (camera->currentState != freeState || !Registered(camera, freeState.get()) ||
				!Registered(camera, returnState.get()) || returnState->id != RE::CameraState::kVR ||
				!camera->cameraRoot || !player || !player->Get3D() || !player->GetParentCell() ||
				!RE::PlayerControls::GetSingleton())
				return false;
			camera->SetState(returnState.get());
			g_loadRecoveryPending = camera->currentState == freeState;
			return camera->currentState == returnState;
		}
	}

	SessionToken CurrentSession()
	{
		return g_session.load();
	}

	bool IsOwned()
	{
		if (!REL::Module::IsVR())
			return false;
		auto* camera = RE::PlayerCamera::GetSingleton();
		ReconcileLoadRecovery(camera);
		auto* data = camera ? camera->GetVRRuntimeData() : nullptr;
		if (!data || camera != g_owner || !g_freeState || camera->currentState != g_freeState ||
			data->cameraStates[RE::CameraState::kFree] != g_freeState || !Registered(camera, g_previousState.get())) {
			Release();
			return false;
		}
		// A different mod leaving and reentering this exact singleton state between
		// observations is unobservable without an engine hook. Do not mix owners.
		return true;
	}

	void SetEnabled(bool a_enabled, SessionToken a_session)
	{
		ValidateSession(a_session);
		if (!RecoverAfterLoad())
			throw ToolError(500, "VR camera recovery after loading failed; retry freecam off after the scene is ready");
		auto*      camera = RE::PlayerCamera::GetSingleton();
		auto*      freeState = GetFreeState(camera);
		const bool owned = IsOwned();
		const bool active = camera->currentState.get() == freeState;
		if (active && !owned)
			throw ToolError(409, "VR free camera was not activated by devbench; preserve its existing owner");
		if (active == a_enabled)
			return;

		if (!a_enabled) {
			camera->SetState(g_previousState.get());
			if (camera->currentState != g_previousState)
				throw ToolError(500, "VR camera state restoration failed");
			Release();
			return;
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!Registered(camera, camera->currentState.get()) || !camera->cameraRoot || !player || !player->Get3D() ||
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
		g_previousState = camera->currentState;
		g_freeState = camera->GetVRRuntimeData()->cameraStates[RE::CameraState::kFree];
		g_owner = camera;
		camera->SetState(freeState);
		if (camera->currentState != g_freeState) {
			Release();
			throw ToolError(500, "VR free-camera activation failed");
		}
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
		if (!REL::Module::IsVR())
			return;
		g_loading.store(true);
		g_session.fetch_add(1);
		if (IsOwned()) {
			g_owner->SetState(g_previousState.get());
			g_loadRecoveryPending = g_owner->currentState == g_freeState;
			if (g_owner->currentState != g_previousState)
				logs::warn("devbench: VR camera pre-load restoration failed; recovery will use the loaded scene's camera states");
		}
		Release();
	}

	void EndLoad()
	{
		if (!REL::Module::IsVR())
			return;
		Release();
		g_session.fetch_add(1);
		if (!RecoverAfterLoad())
			logs::warn("devbench: VR camera post-load recovery is pending; retry freecam off after the scene is ready");
		g_loading.store(false);
	}
}
