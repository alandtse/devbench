#include "test_framework.h"

#include "GameClock.h"
#include "TimeScaleControl.h"

#include <cmath>
#include <limits>

using dvb::GameClock::Integrate;
using dvb::GameClock::Integrator;
using dvb::TimeScaleControl::Reconciler;
using dvb::TimeScaleControl::Validate;

namespace
{
	constexpr double kClamp = dvb::GameClock::kMaxFrameDeltaMs;

	double Advance(double a_fromGameMs, double a_wallMs, double a_multiplier)
	{
		return Integrate(Integrator{ a_fromGameMs }, a_wallMs, a_multiplier, kClamp).gameMs;
	}
}

TEST_CASE("game time advances by the engine's effective multiplier")
{
	CHECK(Advance(0.0, 16.0, 1.0) == 16.0);
	CHECK(Advance(0.0, 16.0, 3.0) == 48.0);
	CHECK(Advance(0.0, 16.0, 0.5) == 8.0);
	CHECK(Advance(100.0, 10.0, 2.0) == 120.0);
	CHECK(Advance(0.0, 0.0, 3.0) == 0.0);
}

TEST_CASE("a mid-run scale change repaces the rest of the run without restarting it")
{
	// 100 game ms of play at 1x, then the same 100 wall ms at 3x: the run's clock keeps its origin.
	double gameMs = 0.0;
	for (int i = 0; i < 10; ++i)
		gameMs = Advance(gameMs, 10.0, 1.0);
	CHECK(gameMs == 100.0);
	for (int i = 0; i < 10; ++i)
		gameMs = Advance(gameMs, 10.0, 3.0);
	CHECK(gameMs == 400.0);
	for (int i = 0; i < 10; ++i)
		gameMs = Advance(gameMs, 10.0, 1.0);
	CHECK(gameMs == 500.0);
}

TEST_CASE("a ramping multiplier is integrated per frame, not by its target")
{
	// The engine walks toward a new multiplier over several frames; each frame counts what it
	// actually ran at, so the total is between the old- and new-speed extremes over the same
	// five-frame shape (a single oversized call would hit the anti-hitch clamp instead).
	double gameMs = 0.0;
	for (const double multiplier : { 1.2, 1.6, 2.2, 2.7, 3.0 })
		gameMs = Advance(gameMs, 100.0, multiplier);
	CHECK(std::fabs(gameMs - 1070.0) < 0.01);

	double constantLow = 0.0;
	for (int i = 0; i < 5; ++i)
		constantLow = Advance(constantLow, 100.0, 1.0);
	double constantHigh = 0.0;
	for (int i = 0; i < 5; ++i)
		constantHigh = Advance(constantHigh, 100.0, 3.0);
	CHECK(gameMs > constantLow);
	CHECK(gameMs < constantHigh);
}

TEST_CASE("one hitch cannot jump the clock")
{
	CHECK(Advance(0.0, kClamp, 1.0) == kClamp);
	CHECK(Advance(0.0, kClamp * 4.0, 1.0) == kClamp);
	CHECK(Advance(0.0, 5000.0, 3.0) == kClamp * 3.0);
	// A negative or non-finite wall delta contributes nothing rather than rewinding.
	CHECK(Advance(50.0, -100.0, 1.0) == 50.0);
	CHECK(Advance(50.0, std::numeric_limits<double>::quiet_NaN(), 1.0) == 50.0);
}

TEST_CASE("a non-finite or non-positive multiplier freezes rather than corrupting the clock")
{
	CHECK(Advance(10.0, 100.0, 0.0) == 10.0);
	CHECK(Advance(10.0, 100.0, -1.0) == 10.0);
	CHECK(Advance(10.0, 100.0, std::numeric_limits<double>::quiet_NaN()) == 10.0);
	CHECK(Advance(10.0, 100.0, std::numeric_limits<double>::infinity()) == 10.0);
}

TEST_CASE("scale validation clamps to the documented range")
{
	const auto normal = Validate(1.0, false, false);
	CHECK(normal.accepted);
	CHECK(normal.value == 1.0F);

	const auto slowest = Validate(0.1, false, false);
	CHECK(slowest.accepted);
	CHECK(slowest.value == 0.1F);

	const auto fastest = Validate(3.0, false, false);
	CHECK(fastest.accepted);
	CHECK(fastest.value == 3.0F);
}

TEST_CASE("scale validation rejects the values it cannot honor")
{
	CHECK(!Validate(std::numeric_limits<double>::quiet_NaN(), false, false).accepted);
	CHECK(!Validate(std::numeric_limits<double>::infinity(), false, false).accepted);
	CHECK(!Validate(-std::numeric_limits<double>::infinity(), false, false).accepted);
	CHECK(!Validate(-1.0, false, false).accepted);
	CHECK(!Validate(0.0, false, false).accepted);   // a freeze needs its own flag
	CHECK(!Validate(0.05, false, false).accepted);  // under the slowest supported scale
	CHECK(!Validate(3.5, false, false).accepted);   // over the normal ceiling without allowHigh
	CHECK(!Validate(10.5, false, true).accepted);   // over the hard ceiling even with allowHigh
	CHECK(!Validate(0.5, true, false).accepted);    // freeze only means scale 0
}

TEST_CASE("scale validation takes freeze and allowHigh as the only ways past the limits")
{
	const auto frozen = Validate(0.0, true, false);
	CHECK(frozen.accepted);
	CHECK(frozen.value == 0.0F);

	const auto high = Validate(4.0, false, true);
	CHECK(high.accepted);
	CHECK(high.value == 4.0F);

	const auto atHardCeiling = Validate(10.0, false, true);
	CHECK(atHardCeiling.accepted);
	CHECK(atHardCeiling.value == 10.0F);
}

TEST_CASE("a rejected scale explains itself instead of silently clamping")
{
	CHECK(!Validate(15.0, false, true).error.empty());
	CHECK(!Validate(0.0, false, false).error.empty());
	CHECK(Validate(1.0, false, false).error.empty());
}

TEST_CASE("a lease applies its value once and restores the captured baseline on expiry")
{
	Reconciler reconciler;
	CHECK(reconciler.Effective() == 1.0F);

	reconciler.Request(2.0F, "run", 1000);
	CHECK(reconciler.Requested() == 2.0F);
	CHECK(reconciler.Leased(500));
	CHECK(reconciler.LeaseRemainingMs(500) == 500);

	const auto applied = reconciler.Reconcile(500);
	CHECK(applied.has_value());
	CHECK(*applied == 2.0F);
	CHECK(reconciler.Effective() == 2.0F);

	// Already at the requested value: the engine must not be written again (that restarts a ramp).
	CHECK(!reconciler.Reconcile(600).has_value());

	// Expiry restores the value captured when the lease was taken, not a hardcoded 1.0.
	const auto restored = reconciler.Reconcile(1000);
	CHECK(restored.has_value());
	CHECK(*restored == 1.0F);
	CHECK(!reconciler.Leased(1000));
	CHECK(reconciler.LeaseRemainingMs(1000) == 0);
	CHECK(!reconciler.Reconcile(1001).has_value());
}

TEST_CASE("a lease taken while another is live keeps the original baseline, not the leased value")
{
	Reconciler reconciler;
	reconciler.Request(0.5F, "first", 1000);
	CHECK(*reconciler.Reconcile(1) == 0.5F);
	reconciler.Request(2.0F, "second", 2000);
	CHECK(*reconciler.Reconcile(1) == 2.0F);
	CHECK(*reconciler.Reconcile(2000) == 1.0F);  // back to normal, not stuck at the first lease
}

TEST_CASE("renewing a lease keeps a long run from expiring mid-flight")
{
	Reconciler reconciler;
	reconciler.Request(3.0F, "run", 1000);
	CHECK(*reconciler.Reconcile(1) == 3.0F);
	reconciler.RenewLease("run", 5000);
	CHECK(!reconciler.Reconcile(1000).has_value());  // would have expired without the renewal
	CHECK(reconciler.Leased(4999));
	CHECK(*reconciler.Reconcile(5000) == 1.0F);
}

TEST_CASE("a displaced holder cannot renew or release a lease it no longer owns")
{
	Reconciler reconciler;
	reconciler.Request(3.0F, "run", 1000);
	reconciler.Request(2.0F, "ad-hoc", 2000);
	CHECK(*reconciler.Reconcile(1) == 2.0F);
	reconciler.RenewLease("run", 9000);
	CHECK(*reconciler.Reconcile(2000) == 1.0F);  // the ad-hoc lease expired on time
	reconciler.Release("run");
	CHECK(reconciler.Requested() == 1.0F);

	Reconciler other;
	other.Request(2.0F, "run", 5000);
	other.Release("someone-else");
	CHECK(other.Leased(1));  // untouched by an unrelated holder
	other.Release();
	CHECK(!other.Leased(1));  // an ownerless release restores normal speed
}

TEST_CASE("a release still needs one more push even though nothing is leased anymore")
{
	// Release() must still be followed by one more Pending()-gated pump, not just Leased().
	Reconciler reconciler;
	reconciler.Request(3.0F, "run", 1000);
	CHECK(*reconciler.Reconcile(1) == 3.0F);
	reconciler.Release("run");
	CHECK(!reconciler.Leased(2));
	CHECK(reconciler.Pending());  // the engine still reports 3.0; Leased() alone would miss this
	CHECK(*reconciler.Reconcile(2) == 1.0F);
	CHECK(!reconciler.Pending());
}
