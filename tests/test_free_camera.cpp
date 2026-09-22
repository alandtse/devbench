#include "test_framework.h"

#include "MainThread.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

// Fakes MainThread::RunAndWait (the real one needs SKSE's TaskInterface) as a
// direct synchronous call.
namespace dvb::MainThread
{
	json RunAndWait(std::function<json()> a_fn, std::chrono::milliseconds, const std::atomic<bool>*)
	{
		return a_fn();
	}
}

// Compile the production controller against a small camera model. This checks
// ownership, lifecycle and mutations; it does not claim to qualify Skyrim's ABI
// or rendered stereo. Namespace substitutions isolate these fakes from other TUs.
namespace camera_test_RE
{
	template <class T>
	using BSTSmartPointer = std::shared_ptr<T>;

	struct NiPoint3
	{
		float x{}, y{}, z{};
	};
	struct NiQuaternion
	{
		float w{ 1 }, x{}, y{}, z{};
	};
	struct Point2
	{
		float x{}, y{};
	};
	enum class CameraState : std::uint32_t
	{
		kFirstPerson = 0,
		kFree = 1,
		kVR = 9,
		kVRTotal = 14
	};
	struct PlayerCamera;
	struct TESCameraState
	{
		virtual ~TESCameraState() = default;
		virtual void  GetRotation(NiQuaternion& a_out) { a_out = sourceRotation; }
		virtual void  GetTranslation(NiPoint3& a_out) { a_out = sourcePosition; }
		PlayerCamera* camera{};
		CameraState   id{};
		NiQuaternion  sourceRotation{ 1, 0.2f, 0, 0.4f };
		NiPoint3      sourcePosition{ 100, 200, 300 };
	};
	struct FreeCameraState : TESCameraState
	{
		NiPoint3     translation{};
		Point2       rotation{}, zUpDown{};
		std::int16_t verticalDirection{};
		bool         useRunSpeed{};
	};
	struct PlayerCamera
	{
		struct States
		{
			std::array<BSTSmartPointer<TESCameraState>, 14> values;
			auto&                                           operator[](CameraState a_id) { return values[static_cast<unsigned>(a_id)]; }
			const auto&                                     operator[](CameraState a_id) const { return values[static_cast<unsigned>(a_id)]; }
			auto                                            begin() const { return values.begin(); }
			auto                                            end() const { return values.end(); }
		};
		struct RuntimeData
		{
			States cameraStates;
		};
		RuntimeData                     vrData;
		RuntimeData                     flatData;
		inline static PlayerCamera*     singleton{};
		static PlayerCamera*            GetSingleton() { return singleton; }
		RuntimeData*                    GetVRRuntimeData() { return &vrData; }
		RuntimeData&                    GetRuntimeData() { return flatData; }
		BSTSmartPointer<TESCameraState> currentState;
		bool                            cameraRoot{ true };
		bool                            rejectTransition{};
		int                             transitions{};
		// Flat model of PlayerCamera::ToggleFlyCam: pushes the current state as "parent",
		// seeds the free state from it, and switches; toggling off pops the parent back.
		BSTSmartPointer<TESCameraState> flatParent;
		void                            ToggleFreeCameraMode(bool)
		{
			auto& free = flatData.cameraStates[CameraState::kFree];
			if (currentState == free) {
				SetState(flatParent.get());
				flatParent.reset();
				return;
			}
			free->sourceRotation = currentState->sourceRotation;
			free->sourcePosition = currentState->sourcePosition;
			flatParent = currentState;
			SetState(free.get());
		}
		bool IsInFreeCameraMode() const { return currentState == flatData.cameraStates[CameraState::kFree]; }
		void SetState(TESCameraState* a_state)
		{
			++transitions;
			if (rejectTransition)
				return;
			for (const auto& state : vrData.cameraStates) {
				if (state.get() == a_state) {
					currentState = state;
					return;
				}
			}
			for (const auto& state : flatData.cameraStates) {
				if (state.get() == a_state) {
					currentState = state;
					return;
				}
			}
			CHECK_MESSAGE(false, "attempted to restore an unregistered camera state");
		}
	};
	struct PlayerCharacter
	{
		inline static PlayerCharacter* singleton{};
		static PlayerCharacter*        GetSingleton() { return singleton; }
		bool                           loaded{ true };
		bool                           Get3D() const { return loaded; }
		bool                           GetParentCell() const { return loaded; }
	};
	struct PlayerControls
	{
		static PlayerControls* GetSingleton()
		{
			static PlayerControls controls;
			return &controls;
		}
	};
}

namespace camera_test_REL
{
	struct Module
	{
		inline static bool vr = true;
		static bool        IsVR() { return vr; }
	};
	struct VariantID
	{
		VariantID(int, int, std::uintptr_t a_rva) { CHECK(a_rva == 0x873B50); }
	};
	template <class T>
	struct Relocation
	{
		Relocation(VariantID) {}
		void operator()(camera_test_RE::FreeCameraState* a_state, const camera_test_RE::NiQuaternion* a_rotation) const
		{
			a_state->rotation = { a_rotation->x, a_rotation->z };
		}
	};
	template <class T>
	void RelocateVirtual(int a_flatSlot, int a_vrSlot, camera_test_RE::TESCameraState* a_state, camera_test_RE::NiQuaternion& a_out)
	{
		CHECK(a_flatSlot == 4 && a_vrSlot == 5);
		a_state->GetRotation(a_out);
	}
	template <class T>
	void RelocateVirtual(int a_flatSlot, int a_vrSlot, camera_test_RE::TESCameraState* a_state, camera_test_RE::NiPoint3& a_out)
	{
		CHECK(a_flatSlot == 5 && a_vrSlot == 6);
		a_state->GetTranslation(a_out);
	}
}

#define RE camera_test_RE
#define REL camera_test_REL
#include "../src/FreeCamera.cpp"
#undef REL
#undef RE

namespace
{
	namespace Camera = dvb::FreeCamera;
	namespace Model = camera_test_RE;

	struct Scene
	{
		Model::PlayerCamera                     camera;
		Model::PlayerCharacter                  player;
		std::shared_ptr<Model::TESCameraState>  prior = std::make_shared<Model::TESCameraState>();
		std::shared_ptr<Model::TESCameraState>  other = std::make_shared<Model::TESCameraState>();
		std::shared_ptr<Model::FreeCameraState> free = std::make_shared<Model::FreeCameraState>();
		Scene()
		{
			Model::PlayerCamera::singleton = &camera;
			Model::PlayerCharacter::singleton = &player;
			prior->camera = other->camera = free->camera = &camera;
			prior->id = Model::CameraState::kVR;
			other->id = Model::CameraState::kFirstPerson;
			free->id = Model::CameraState::kFree;
			camera.vrData.cameraStates[prior->id] = prior;
			camera.vrData.cameraStates[other->id] = other;
			camera.vrData.cameraStates[free->id] = free;
			camera.currentState = prior;
			Camera::EndLoad();
		}
		~Scene()
		{
			camera.currentState = prior;
			Camera::EndLoad();
			Model::PlayerCamera::singleton = nullptr;
			Model::PlayerCharacter::singleton = nullptr;
		}
		void Enable(bool a_on = true) { Camera::SetEnabled(a_on, Camera::CurrentSession()); }
		void Drive() { Camera::Drive(10, 20, 30, 0.3f, 0.5f, Camera::CurrentSession()); }
	};

	template <class F>
	void ExpectError(int a_code, F a_fn)
	{
		try {
			a_fn();
			CHECK_MESSAGE(false, "expected ToolError");
		} catch (const dvb::ToolError& error) {
			CHECK(error.code == a_code);
		}
	}
}

TEST_CASE("VR camera repeated owned round trips retain the prior state and clear input latches")
{
	Scene scene;
	for (int cycle = 0; cycle < 3; ++cycle) {
		scene.free->zUpDown = { 1, -1 };
		scene.free->verticalDirection = 1;
		scene.free->useRunSpeed = true;
		scene.Enable();
		CHECK(Camera::IsOwned());
		CHECK(scene.free->translation.x == scene.prior->sourcePosition.x);
		CHECK(scene.free->translation.y == scene.prior->sourcePosition.y);
		CHECK(scene.free->translation.z == scene.prior->sourcePosition.z);
		CHECK(scene.free->rotation.x == scene.prior->sourceRotation.x);
		CHECK(scene.free->rotation.y == scene.prior->sourceRotation.z);
		CHECK(scene.free->zUpDown.x == 0 && scene.free->zUpDown.y == 0);
		CHECK(scene.free->verticalDirection == 0 && !scene.free->useRunSpeed);
		const auto entries = scene.camera.transitions;
		scene.Enable();
		CHECK(scene.camera.transitions == entries);
		scene.Drive();
		CHECK(scene.free->translation.x == 10 && scene.free->translation.y == 20 && scene.free->translation.z == 30);
		CHECK(scene.free->rotation.x == 0.3f && scene.free->rotation.y == 0.5f);
		scene.Enable(false);
		CHECK(scene.camera.currentState == scene.prior);
		CHECK(!Camera::IsOwned());
		scene.Enable(false);
		CHECK(scene.camera.transitions == entries + 1);
	}
}

TEST_CASE("VR camera rejects every mutation of an externally activated free camera")
{
	Scene scene;
	scene.camera.currentState = scene.free;
	scene.free->translation = { 90, 80, 70 };
	ExpectError(409, [&] { scene.Enable(); });
	ExpectError(409, [&] { scene.Enable(false); });
	ExpectError(409, [&] { scene.Drive(); });
	CHECK(scene.camera.transitions == 0);
	CHECK(scene.free->translation.x == 90);
	CHECK(!Camera::IsOwned());
}

TEST_CASE("VR camera observation releases ownership after an external state change")
{
	Scene scene;
	scene.Enable();
	scene.camera.currentState = scene.other;
	CHECK(!Camera::IsOwned());
	CHECK(scene.prior.use_count() == 2);  // fixture + registry; controller released it
	scene.camera.currentState = scene.free;
	ExpectError(409, [&] { scene.Enable(false); });
	scene.camera.currentState = scene.other;
	scene.Enable();
	scene.Enable(false);
	CHECK(scene.camera.currentState == scene.other);
}

TEST_CASE("VR camera refuses replaced free and return states")
{
	Scene scene;
	scene.Enable();
	scene.camera.vrData.cameraStates[scene.prior->id] = scene.other;
	ExpectError(409, [&] { scene.Enable(false); });
	CHECK(scene.camera.currentState == scene.free);
	CHECK(!Camera::IsOwned());
	scene.camera.currentState = scene.other;
	scene.Enable();
	auto replacement = std::make_shared<Model::FreeCameraState>();
	replacement->camera = &scene.camera;
	replacement->id = Model::CameraState::kFree;
	scene.camera.vrData.cameraStates[Model::CameraState::kFree] = replacement;
	ExpectError(409, [&] { scene.Drive(); });
	CHECK(!Camera::IsOwned());
	CHECK(replacement->translation.x == 0);
}

TEST_CASE("VR camera invalidates mutations at both save-load boundaries")
{
	Scene      scene;
	const auto oldSession = Camera::CurrentSession();
	scene.Enable();
	Camera::BeginLoad();
	CHECK(scene.camera.currentState == scene.prior);
	CHECK(!Camera::IsOwned());
	const auto loadingSession = Camera::CurrentSession();
	ExpectError(409, [&] { Camera::SetEnabled(true, oldSession); });
	ExpectError(409, [&] { Camera::SetEnabled(true, loadingSession); });
	ExpectError(409, [&] { scene.Drive(); });
	Camera::EndLoad();
	ExpectError(409, [&] { Camera::SetEnabled(true, oldSession); });
	ExpectError(409, [&] { Camera::SetEnabled(true, loadingSession); });
	scene.Enable();
	ExpectError(409, [&] { Camera::Drive(999, 999, 999, 1, 1, oldSession); });
	CHECK(scene.free->translation.x == scene.prior->sourcePosition.x);
	// NewGame uses EndLoad without a preceding PreLoad notification.
	const auto beforeNewGame = Camera::CurrentSession();
	scene.camera.currentState = scene.other;
	Camera::EndLoad();
	ExpectError(409, [&] { Camera::SetEnabled(true, beforeNewGame); });
	CHECK(!Camera::IsOwned());
}

TEST_CASE("VR camera transition failures retain safe restoration and recovery retries")
{
	Scene scene;
	scene.camera.rejectTransition = true;
	ExpectError(500, [&] { scene.Enable(); });
	CHECK(!Camera::IsOwned());
	scene.camera.rejectTransition = false;
	scene.Enable();
	scene.camera.rejectTransition = true;
	ExpectError(500, [&] { scene.Enable(false); });
	CHECK(Camera::IsOwned());  // retry still has the valid return state
	Camera::BeginLoad();
	CHECK(!Camera::IsOwned());
	Camera::EndLoad();
	ExpectError(500, [&] { scene.Enable(false); });
	scene.camera.rejectTransition = false;
	scene.Enable(false);
	CHECK(scene.camera.currentState == scene.prior);
	CHECK(!Camera::IsOwned());
}

TEST_CASE("VR camera requires a loaded player and a registered source state")
{
	Scene scene;
	scene.player.loaded = false;
	ExpectError(422, [&] { scene.Enable(); });
	scene.player.loaded = true;
	scene.camera.vrData.cameraStates[scene.prior->id].reset();
	ExpectError(422, [&] { scene.Enable(); });
	CHECK(scene.camera.transitions == 0);
	CHECK(!Camera::IsOwned());
}

TEST_CASE("VR camera load recovery reacquires replacement scene states")
{
	Scene scene;
	scene.Enable();
	scene.camera.rejectTransition = true;
	Camera::BeginLoad();
	CHECK(!Camera::IsOwned());
	CHECK(scene.prior.use_count() == 2);
	CHECK(scene.free.use_count() == 3);
	const std::weak_ptr<Model::TESCameraState>  oldPrior = scene.prior;
	const std::weak_ptr<Model::FreeCameraState> oldFree = scene.free;
	scene.prior = std::make_shared<Model::TESCameraState>();
	scene.free = std::make_shared<Model::FreeCameraState>();
	scene.prior->camera = scene.free->camera = &scene.camera;
	scene.prior->id = Model::CameraState::kVR;
	scene.free->id = Model::CameraState::kFree;
	scene.camera.vrData.cameraStates[Model::CameraState::kVR] = scene.prior;
	scene.camera.vrData.cameraStates[Model::CameraState::kFree] = scene.free;
	scene.camera.currentState = scene.free;
	CHECK(oldPrior.expired() && oldFree.expired());
	scene.camera.rejectTransition = false;
	Camera::EndLoad();
	CHECK(scene.camera.currentState == scene.prior);
	CHECK(!Camera::IsOwned());
	scene.Enable();
	scene.Enable(false);
	CHECK(scene.camera.currentState == scene.prior);
}

TEST_CASE("VR camera load recovery waits for an available camera")
{
	Scene scene;
	scene.Enable();
	scene.camera.rejectTransition = true;
	Camera::BeginLoad();
	Model::PlayerCamera::singleton = nullptr;
	Camera::EndLoad();
	ExpectError(500, [&] { scene.Enable(false); });
	Model::PlayerCamera::singleton = &scene.camera;
	scene.camera.rejectTransition = false;
	scene.Enable(false);
	CHECK(scene.camera.currentState == scene.prior);
}

TEST_CASE("VR camera load recovery validates the current return-state registration")
{
	Scene scene;
	scene.Enable();
	scene.camera.rejectTransition = true;
	Camera::BeginLoad();
	scene.camera.rejectTransition = false;
	scene.camera.vrData.cameraStates[Model::CameraState::kVR].reset();
	const auto transitions = scene.camera.transitions;
	Camera::EndLoad();
	ExpectError(500, [&] { scene.Enable(false); });
	scene.camera.vrData.cameraStates[Model::CameraState::kVR] = scene.other;
	ExpectError(500, [&] { scene.Enable(false); });
	scene.camera.vrData.cameraStates[Model::CameraState::kVR] = scene.prior;
	scene.prior->camera = nullptr;
	ExpectError(500, [&] { scene.Enable(false); });
	CHECK(scene.camera.transitions == transitions);
	scene.prior->camera = &scene.camera;
	scene.Enable(false);
	CHECK(scene.camera.currentState == scene.prior);
}

TEST_CASE("VR camera load recovery waits for a loaded scene and blocks stale requests")
{
	Scene scene;
	scene.Enable();
	scene.camera.rejectTransition = true;
	Camera::BeginLoad();
	const auto loadingSession = Camera::CurrentSession();
	scene.camera.rejectTransition = false;
	scene.player.loaded = false;
	const auto transitions = scene.camera.transitions;
	Camera::EndLoad();
	ExpectError(500, [&] { scene.Enable(false); });
	ExpectError(409, [&] { scene.Drive(); });
	scene.player.loaded = true;
	ExpectError(409, [&] { Camera::SetEnabled(false, loadingSession); });
	CHECK(scene.camera.transitions == transitions);
	scene.Enable(false);
	CHECK(scene.camera.currentState == scene.prior);
}

TEST_CASE("VR camera load recovery preserves a camera already changed by loading")
{
	Scene scene;
	scene.Enable();
	scene.camera.rejectTransition = true;
	Camera::BeginLoad();
	scene.camera.rejectTransition = false;
	scene.camera.currentState = scene.other;
	const auto transitions = scene.camera.transitions;
	Camera::EndLoad();
	CHECK(scene.camera.currentState == scene.other);
	CHECK(scene.camera.transitions == transitions);
	scene.camera.currentState = scene.free;
	ExpectError(409, [&] { scene.Enable(false); });
	CHECK(scene.camera.transitions == transitions);
}

TEST_CASE("VR camera load recovery never starts for an externally owned free camera")
{
	Scene scene;
	scene.camera.currentState = scene.free;
	Camera::BeginLoad();
	Camera::EndLoad();
	ExpectError(409, [&] { scene.Enable(false); });
	CHECK(scene.camera.currentState == scene.free);
	CHECK(scene.camera.transitions == 0);
}

TEST_CASE("VR camera observation cancels pending recovery before a foreign free-camera entry")
{
	Scene scene;
	scene.Enable();
	scene.camera.rejectTransition = true;
	Camera::BeginLoad();
	Camera::EndLoad();
	scene.camera.currentState = scene.other;
	CHECK(!Camera::IsOwned());
	scene.camera.currentState = scene.free;
	scene.camera.rejectTransition = false;
	const auto transitions = scene.camera.transitions;
	ExpectError(409, [&] { scene.Enable(false); });
	CHECK(scene.camera.currentState == scene.free);
	CHECK(scene.camera.transitions == transitions);
}

TEST_CASE("a replay camera hold activates the free camera and restores on destruction")
{
	Scene scene;
	{
		Camera::ReplayHold hold;
		hold.Activate();
		CHECK(scene.camera.currentState == scene.free);
	}
	CHECK(scene.camera.currentState == scene.prior);
}

TEST_CASE("a replay camera hold that never activated restores nothing")
{
	Scene scene;
	{
		Camera::ReplayHold hold;
	}
	CHECK(scene.camera.currentState == scene.prior);
	CHECK(scene.camera.transitions == 0);
}

TEST_CASE("activating a replay camera hold twice is a no-op")
{
	Scene              scene;
	Camera::ReplayHold hold;
	hold.Activate();
	const auto transitions = scene.camera.transitions;
	hold.Activate();
	CHECK(scene.camera.transitions == transitions);
}

TEST_CASE("a replay camera hold that failed to activate does not restore on destruction")
{
	Scene scene;
	scene.camera.currentState = scene.free;  // owned elsewhere before the hold ever runs
	{
		Camera::ReplayHold hold;
		ExpectError(409, [&] { hold.Activate(); });
		CHECK(scene.camera.currentState == scene.free);
	}
	// Destruction must not have tried to restore a state this hold never captured.
	CHECK(scene.camera.currentState == scene.free);
}

namespace
{
	// Flat (SE/AE) path: the engine's own ToggleFreeCameraMode pushes/pops the prior state
	// itself, so these cases drive that toggle rather than the hand-rolled VR state juggling
	// above. Module::vr is a process-global fake, so the fixture restores it on destruction.
	struct FlatScene
	{
		Model::PlayerCamera                     camera;
		Model::PlayerCharacter                  player;
		std::shared_ptr<Model::TESCameraState>  other = std::make_shared<Model::TESCameraState>();
		std::shared_ptr<Model::FreeCameraState> free = std::make_shared<Model::FreeCameraState>();
		FlatScene()
		{
			Model::PlayerCamera::singleton = &camera;
			Model::PlayerCharacter::singleton = &player;
			other->camera = free->camera = &camera;
			other->id = Model::CameraState::kFirstPerson;
			free->id = Model::CameraState::kFree;
			camera.flatData.cameraStates[other->id] = other;
			camera.flatData.cameraStates[free->id] = free;
			camera.currentState = other;
			camera_test_REL::Module::vr = false;
			Camera::EndLoad();
		}
		~FlatScene()
		{
			camera_test_REL::Module::vr = true;
			Model::PlayerCamera::singleton = nullptr;
			Model::PlayerCharacter::singleton = nullptr;
		}
		void Enable(bool a_on = true) { Camera::SetEnabled(a_on, Camera::CurrentSession()); }
		void Drive() { Camera::Drive(10, 20, 30, 0.3f, 0.5f, Camera::CurrentSession()); }
	};
}

TEST_CASE("flat camera enable/drive/disable round trip restores the exact prior state")
{
	FlatScene scene;
	scene.Enable();
	CHECK(Camera::IsOwned());
	CHECK(scene.camera.IsInFreeCameraMode());
	scene.Drive();
	CHECK(scene.free->translation.x == 10 && scene.free->translation.y == 20 && scene.free->translation.z == 30);
	CHECK(scene.free->rotation.x == 0.3f && scene.free->rotation.y == 0.5f);
	scene.Enable(false);
	CHECK(scene.camera.currentState == scene.other);
	CHECK(!Camera::IsOwned());
}

TEST_CASE("flat camera rejects every mutation of an externally activated free camera")
{
	FlatScene scene;
	scene.camera.currentState = scene.free;
	scene.free->translation = { 90, 80, 70 };
	ExpectError(409, [&] { scene.Enable(); });
	ExpectError(409, [&] { scene.Enable(false); });
	ExpectError(409, [&] { scene.Drive(); });
	CHECK(scene.camera.transitions == 0);
	CHECK(scene.free->translation.x == 90);
	CHECK(!Camera::IsOwned());
}

TEST_CASE("flat camera observation releases ownership after an external state change")
{
	FlatScene scene;
	scene.Enable();
	scene.camera.currentState = scene.other;
	CHECK(!Camera::IsOwned());
	ExpectError(409, [&] { scene.Drive(); });
	scene.Enable();
	scene.Enable(false);
	CHECK(scene.camera.currentState == scene.other);
}

TEST_CASE("flat camera requires a loaded player")
{
	FlatScene scene;
	scene.player.loaded = false;
	ExpectError(422, [&] { scene.Enable(); });
	CHECK(scene.camera.transitions == 0);
	CHECK(!Camera::IsOwned());
}

TEST_CASE("flat camera BeginLoad/EndLoad invalidate an in-flight session without touching engine state")
{
	FlatScene scene;
	scene.Enable();
	const auto session = Camera::CurrentSession();
	CHECK(scene.camera.IsInFreeCameraMode());
	const auto transitions = scene.camera.transitions;
	Camera::BeginLoad();
	Camera::EndLoad();
	CHECK(scene.camera.transitions == transitions);
	CHECK(scene.camera.IsInFreeCameraMode());
	CHECK(!Camera::IsOwned());
	ExpectError(409, [&] { Camera::SetEnabled(true, session); });
}
