#include "test_framework.h"

#include "FormSearchLogic.h"

using namespace dvb::FormSearchLogic;

TEST_CASE("a substring match ignores case and an empty needle matches everything")
{
	CHECK(ContainsCI("meshes\\xSummer\\Lena\\wig.nif", "lena"));
	CHECK(ContainsCI("meshes\\xSummer\\Lena\\wig.nif", "XSUMMER"));
	CHECK(ContainsCI("anything", ""));
	CHECK(!ContainsCI("meshes\\hair.nif", "wig"));
}

TEST_CASE("head part types parse from their names in any case")
{
	CHECK(ParseHeadPartType("Hair") == 3);
	CHECK(ParseHeadPartType("facialhair") == 4);
	CHECK(ParseHeadPartType("MISC") == 0);
	CHECK(!ParseHeadPartType("Wig").has_value());
	CHECK(HeadPartTypeName(3) == "Hair");
	CHECK(HeadPartTypeName(7).empty());
	CHECK(HeadPartTypeName(-1).empty());
}

TEST_CASE("slot numbers come from a mask and back")
{
	CHECK(SlotsInMask(0).empty());
	const std::vector<int> hair{ 31 };
	const std::vector<int> several{ 30, 31, 41 };
	CHECK(SlotsInMask(1u << 1) == hair);
	CHECK(SlotsInMask((1u << 0) | (1u << 1) | (1u << 11)) == several);
	CHECK(MaskForSlot(41) == (1u << 11));
	CHECK(MaskForSlot(30) == 1u);
	CHECK(MaskForSlot(61) == (1u << 31));
	CHECK(!MaskForSlot(29).has_value());
	CHECK(!MaskForSlot(62).has_value());
}
