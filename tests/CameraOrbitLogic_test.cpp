#include "test_framework.h"

#include "CameraOrbitLogic.h"

using namespace dvb::CameraOrbit;

namespace
{
	Baseline Scene()
	{
		Baseline b;
		b.playerYaw = 1.0f;
		b.playerPitch = 0.1f;
		b.camera = { 0.2f, 0.25f, 0.3f, 0.35f, Vec3{ 5.0f, 0.0f, 6.0f }, Vec3{ 7.0f, 0.0f, 8.0f } };
		return b;
	}

	Request Front()
	{
		Request r;
		r.yawRad = 3.0f;
		return r;
	}
}

TEST_CASE("an orbit requested before a load or new game is refused")
{
	OrbitState s;
	const auto session = s.Session();
	s.EndSession();
	CHECK(s.Enable(Front(), session, false, false) == Admission::kStaleSession);
	CHECK(!s.On());
	CHECK(s.Enable(Front(), s.Session(), false, false) == Admission::kAccepted);
	CHECK(s.On());
}

TEST_CASE("an orbit is refused during a load and while the free camera is on")
{
	OrbitState s;
	CHECK(s.Enable(Front(), s.Session(), true, false) == Admission::kLoading);
	CHECK(s.Enable(Front(), s.Session(), false, true) == Admission::kFreeCamera);
	CHECK(!s.On());
	CHECK(s.Revision() == 0);
}

TEST_CASE("an orbit captures the scene once and leaves omitted parts alone")
{
	OrbitState s;
	s.Enable(Front(), s.Session(), false, false);
	CHECK(!s.Applied());
	const auto w = s.Apply(Scene());
	CHECK(w.has_value());
	CHECK(s.Applied());
	CHECK(w->playerYaw == 1.0f);
	CHECK(w->camera.targetYaw == 4.0f);
	CHECK(w->camera.currentYaw == 4.0f);
	CHECK(w->camera.freeRotationX == 3.0f);   // the same heading, relative to the facing, for free rotation
	CHECK(!w->playerPitch.has_value());       // no pitch asked for
	CHECK(w->camera.targetZoom == 0.3f);      // no zoom asked for
	CHECK(w->camera.offsetActual.x == 7.0f);  // no offset asked for

	Baseline moved = Scene();
	moved.playerYaw = 2.0f;  // the player turned after the first write; the held facing wins
	const auto again = s.Apply(moved);
	CHECK(again->playerYaw == 1.0f);
	CHECK(s.CapturedBaseline()->playerYaw == 1.0f);
}

TEST_CASE("ending an orbit writes back exactly what it overwrote")
{
	OrbitState s;
	Request    r = Front();
	r.zoom = -0.5f;
	r.pitchRad = 0.4f;
	s.Enable(r, s.Session(), false, false);
	s.Apply(Scene());

	CameraFields now = Scene().camera;
	now.targetYaw = 4.0f;
	now.targetZoom = -0.5f;
	now.offsetActual = Vec3{ 9.0f, 0.0f, 9.0f };  // moved by the game; the orbit never set an offset
	const auto restore = s.End(EndReason::kRequested, now);
	CHECK(restore.has_value());
	CHECK(restore->camera.targetYaw == 0.2f);
	CHECK(restore->camera.freeRotationX == 0.0f);
	CHECK(restore->camera.targetZoom == 0.3f);
	CHECK(restore->camera.currentZoom == 0.35f);
	CHECK(restore->camera.offsetActual.x == 9.0f);
	CHECK(restore->playerPitch.has_value());
	CHECK(*restore->playerPitch == 0.1f);
	CHECK(!s.On());
	CHECK(s.LastEnd() == EndReason::kRequested);
	CHECK(!s.End(EndReason::kRequested, now).has_value());  // already off
}

TEST_CASE("a reconfigured orbit keeps its first baseline and restores every field any configuration set")
{
	OrbitState s;
	Request    zoomed = Front();
	zoomed.zoom = 0.8f;
	s.Enable(zoomed, s.Session(), false, false);
	s.Apply(Scene());
	Request offset = Front();
	offset.offset = Vec3{ 1.0f, 0.0f, 2.0f };
	s.Enable(offset, s.Session(), false, false);
	Baseline later = Scene();
	later.camera.targetZoom = 0.8f;
	s.Apply(later);
	CHECK(s.CapturedBaseline()->camera.targetZoom == 0.3f);
	const auto restore = s.End(EndReason::kLeftThirdPerson, later.camera);
	CHECK(restore->camera.targetZoom == 0.3f);
	CHECK(restore->camera.offsetExpected.x == 5.0f);
}

TEST_CASE("an orbit that never applied restores nothing")
{
	OrbitState s;
	s.Enable(Front(), s.Session(), false, false);
	CHECK(!s.End(EndReason::kFreeCamera, Scene().camera).has_value());
	CHECK(s.LastEnd() == EndReason::kFreeCamera);
	CHECK(!s.Apply(Scene()).has_value());  // off: no writes
}

TEST_CASE("a load drops the orbit and its baseline without restoring")
{
	OrbitState s;
	s.Enable(Front(), s.Session(), false, false);
	s.Apply(Scene());
	s.EndSession();
	CHECK(!s.On());
	CHECK(!s.Applied());
	CHECK(s.LastEnd() == EndReason::kSessionEnded);
	CHECK(!s.End(EndReason::kRequested, Scene().camera).has_value());
}

TEST_CASE("every change to the orbit moves its revision")
{
	OrbitState s;
	const auto r0 = s.Revision();
	s.Enable(Front(), s.Session(), false, false);
	const auto r1 = s.Revision();
	CHECK(r1 != r0);
	s.Apply(Scene());
	CHECK(s.Revision() == r1);  // applying is not a change of configuration
	s.Enable(Front(), s.Session(), false, false);
	const auto r2 = s.Revision();
	CHECK(r2 != r1);
	s.End(EndReason::kRequested, Scene().camera);
	CHECK(s.Revision() != r2);
}
