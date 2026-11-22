#include "test_framework.h"

#include "NodeRecordingLogic.h"

using namespace dvb;
using namespace dvb::NodeRecordingLogic;

TEST_CASE("node names accept a list of distinct names")
{
	std::string error;
	const auto  names = ParseNodeNames(json::array({ "NPC Pelvis [Pelv]", "3BA" }), error);
	CHECK(names.has_value());
	if (!names)
		return;
	CHECK(names->size() == 2);
	CHECK((*names)[1] == "3BA");
}

TEST_CASE("node names refuse an empty, oversized or malformed list")
{
	std::string error;
	CHECK(!ParseNodeNames(json::array(), error).has_value());
	CHECK(!ParseNodeNames(json("3BA"), error).has_value());
	CHECK(!ParseNodeNames(json::array({ "" }), error).has_value());
	CHECK(!ParseNodeNames(json::array({ 5 }), error).has_value());
	CHECK(!ParseNodeNames(json::array({ "a", "a" }), error).has_value());
	CHECK(!ParseNodeNames(json::array({ std::string(kMaxNameLength + 1, 'x') }), error).has_value());
	json many = json::array();
	for (std::size_t i = 0; i <= kMaxNodes; ++i)
		many.push_back("n" + std::to_string(i));
	CHECK(!ParseNodeNames(many, error).has_value());
	CHECK(!error.empty());
}
