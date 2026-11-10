#include "test_framework.h"

#include "MapLogic.h"

using dvb::json;
using namespace dvb::MapLogic;

namespace
{
	std::string ErrorOf(const json& a_args)
	{
		const auto parsed = Parse(a_args);
		return parsed.request ? std::string{} : parsed.error;
	}
}

TEST_CASE("a map call must name a known action")
{
	CHECK(ErrorOf(json::object()).find("action is required") != std::string::npos);
	CHECK(ErrorOf(json{ { "action", 3 } }).find("action is required") != std::string::npos);
	CHECK(ErrorOf(json{ { "action", "teleport" } }).find("unknown action 'teleport'") != std::string::npos);
}

TEST_CASE("get and clearWaypoint need no target")
{
	CHECK(Parse(json{ { "action", "get" } }).request->action == Action::kGet);
	CHECK(Parse(json{ { "action", "clearWaypoint" } }).request->action == Action::kClearWaypoint);
}

TEST_CASE("locate needs exactly a location")
{
	const auto ok = Parse(json{ { "action", "locate" }, { "location", "WhiterunLocation" } });
	CHECK(ok.request.has_value());
	CHECK(ok.request->location == "WhiterunLocation");
	CHECK(!ok.request->position.has_value());
	CHECK(!ErrorOf(json{ { "action", "locate" } }).empty());
	CHECK(!ErrorOf(json{ { "action", "locate" }, { "location", "X" }, { "x", 1 } }).empty());
	CHECK(!ErrorOf(json{ { "action", "locate" }, { "location", "X" }, { "worldspace", "Tamriel" } }).empty());
	CHECK(!ErrorOf(json{ { "action", "locate" }, { "location", "" } }).empty());
}

TEST_CASE("setWaypoint takes a location or an x and y, never both")
{
	const auto byLocation = Parse(json{ { "action", "setWaypoint" }, { "location", "0x18DEB" } });
	CHECK(byLocation.request.has_value());
	CHECK(byLocation.request->location == "0x18DEB");

	const auto byPoint = Parse(json{ { "action", "setWaypoint" }, { "x", 1.5 }, { "y", -2 } });
	CHECK(byPoint.request.has_value());
	CHECK((byPoint.request->position.value() == std::array<float, 3>{ 1.5f, -2.0f, 0.0f }));
	CHECK(byPoint.request->worldspace.empty());

	const auto withWorld = Parse(json{ { "action", "setWaypoint" }, { "x", 1 }, { "y", 2 }, { "z", 3 }, { "worldspace", "Tamriel" } });
	CHECK(withWorld.request.has_value());
	CHECK((withWorld.request->position.value() == std::array<float, 3>{ 1.0f, 2.0f, 3.0f }));
	CHECK(withWorld.request->worldspace == "Tamriel");

	CHECK(!ErrorOf(json{ { "action", "setWaypoint" } }).empty());
	CHECK(!ErrorOf(json{ { "action", "setWaypoint" }, { "x", 1 } }).empty());
	CHECK(!ErrorOf(json{ { "action", "setWaypoint" }, { "location", "X" }, { "x", 1 }, { "y", 2 } }).empty());
	CHECK(!ErrorOf(json{ { "action", "setWaypoint" }, { "location", "X" }, { "worldspace", "Tamriel" } }).empty());
}

TEST_CASE("setWaypoint rejects coordinates that are not finite numbers")
{
	CHECK(ErrorOf(json{ { "action", "setWaypoint" }, { "x", "1" }, { "y", 2 } }).find("x must be a finite number") != std::string::npos);
	CHECK(ErrorOf(json{ { "action", "setWaypoint" }, { "x", 1 }, { "y", 1e300 } }).find("y must be a finite number") != std::string::npos);
	CHECK(ErrorOf(json{ { "action", "setWaypoint" }, { "x", 1 }, { "y", 2 }, { "z", true } }).find("z must be a finite number") != std::string::npos);
}
