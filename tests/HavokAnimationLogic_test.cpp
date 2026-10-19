#include "test_framework.h"

#include "HavokAnimationLogic.h"

#include <cstring>
#include <string>
#include <vector>

using namespace dvb::HavokAnimationLogic;

namespace
{
	void PutU32(std::vector<std::uint8_t>& a_bytes, std::size_t a_at, std::uint32_t a_value)
	{
		std::memcpy(a_bytes.data() + a_at, &a_value, sizeof(a_value));
	}

	void PutFloat(std::vector<std::uint8_t>& a_bytes, std::size_t a_at, float a_value)
	{
		std::memcpy(a_bytes.data() + a_at, &a_value, sizeof(a_value));
	}

	void PutName(std::vector<std::uint8_t>& a_bytes, std::size_t a_at, const std::string& a_name)
	{
		std::memcpy(a_bytes.data() + a_at, a_name.c_str(), a_name.size() + 1);
	}

	// A packfile with a class-name section, a data section holding one animation object, and the virtual fixup that names it.
	std::vector<std::uint8_t> MakePackfile(const std::string& a_class, float a_duration, int a_pointerSize = 8, std::size_t a_tableStart = 0x40)
	{
		constexpr std::size_t     classStart = 0x100, dataStart = 0x200, objectOffset = 0x10, fixupOffset = 0x80, exportsOffset = 0x90;
		std::vector<std::uint8_t> bytes(0x400, 0);
		PutU32(bytes, 0, 0x57E0E057);
		PutU32(bytes, 4, 0x10C0C010);
		bytes[16] = static_cast<std::uint8_t>(a_pointerSize);
		bytes[17] = 1;
		PutU32(bytes, 20, 3);

		auto section = [&](std::size_t a_index, const char* a_name, std::uint32_t a_start, std::uint32_t a_virtual, std::uint32_t a_exports) {
			const auto base = a_tableStart + a_index * 48;
			PutName(bytes, base, a_name);
			bytes[base + 19] = 0xFF;
			PutU32(bytes, base + 20, a_start);
			PutU32(bytes, base + 32, a_virtual);
			PutU32(bytes, base + 36, a_exports);
		};
		section(0, "__classnames__", classStart, 0, 0);
		section(1, "__types__", dataStart, 0, 0);
		section(2, "__data__", dataStart, fixupOffset, exportsOffset);

		PutName(bytes, classStart + 5, a_class);
		PutU32(bytes, dataStart + fixupOffset, objectOffset);
		PutU32(bytes, dataStart + fixupOffset + 4, 0);
		PutU32(bytes, dataStart + fixupOffset + 8, 5);
		PutU32(bytes, dataStart + fixupOffset + 12, 0xFFFFFFFF);

		const auto fields = dataStart + objectOffset + (a_pointerSize == 8 ? 0x10 : 0x08);
		PutFloat(bytes, fields + 4, a_duration);
		PutU32(bytes, fields + 8, 52);
		PutU32(bytes, fields + 12, 1);
		return bytes;
	}
}

TEST_CASE("an animation's class, duration and track counts are read from a 64-bit packfile")
{
	const auto bytes = MakePackfile("hkaSplineCompressedAnimation", 2.5f);
	const auto info = ParseAnimation(bytes);
	CHECK(info.has_value());
	if (!info)
		return;
	CHECK(info->className == "hkaSplineCompressedAnimation");
	CHECK(info->durationSeconds == 2.5f);
	CHECK(info->transformTracks == 52);
	CHECK(info->floatTracks == 1);
	CHECK(info->pointerSize == 8);
}

TEST_CASE("a 32-bit packfile and the longer section table are read too")
{
	const auto narrow = ParseAnimation(MakePackfile("hkaInterleavedUncompressedAnimation", 1.25f, 4));
	CHECK(narrow.has_value());
	if (!narrow)
		return;
	CHECK(narrow->durationSeconds == 1.25f);
	CHECK(narrow->pointerSize == 4);

	const auto shifted = ParseAnimation(MakePackfile("hkaSplineCompressedAnimation", 4.0f, 8, 0x50));
	CHECK(shifted.has_value());
	if (!shifted)
		return;
	CHECK(shifted->durationSeconds == 4.0f);
}

TEST_CASE("bytes that are not an animation packfile are refused")
{
	CHECK(!ParseAnimation({}).has_value());
	CHECK(!ParseAnimation(std::vector<std::uint8_t>(0x400, 0)).has_value());
	CHECK(!ParseAnimation(MakePackfile("hkaAnimationBinding", 2.0f)).has_value());
	CHECK(!ParseAnimation(MakePackfile("hkaSplineCompressedAnimation", 0.0f)).has_value());
	CHECK(!ParseAnimation(MakePackfile("hkaSplineCompressedAnimation", 1.0e9f)).has_value());
}

TEST_CASE("a truncated packfile is refused without reading past it")
{
	const auto whole = MakePackfile("hkaSplineCompressedAnimation", 2.5f);
	for (const std::size_t length : { std::size_t{ 8 }, std::size_t{ 0x40 }, std::size_t{ 0x100 }, std::size_t{ 0x230 } })
		CHECK(!ParseAnimation(std::span<const std::uint8_t>(whole.data(), length)).has_value());
}

namespace
{
	struct BehaviorClip
	{
		std::string name;
		std::string animation;
	};

	// A behavior packfile of clip generators (class at classnames offset 5) and reference generators (offset 40), each object
	// 0x30 bytes with its string pointers at +0x10 and +0x20, strings after the objects, then the local and virtual fixups.
	std::vector<std::uint8_t> MakeBehavior(const std::vector<BehaviorClip>& a_clips, const std::vector<std::string>& a_references, bool a_junkPointer = false)
	{
		constexpr std::size_t     classStart = 0x100, dataStart = 0x200, objectSize = 0x30, stringsAt = 0x100, localAt = 0x300, virtualAt = 0x380, exportsAt = 0x3F0;
		std::vector<std::uint8_t> bytes(0x800, 0);
		PutU32(bytes, 0, 0x57E0E057);
		PutU32(bytes, 4, 0x10C0C010);
		bytes[16] = 8;
		bytes[17] = 1;
		PutU32(bytes, 20, 3);
		auto section = [&](std::size_t a_index, const char* a_name, std::uint32_t a_start, std::uint32_t a_local, std::uint32_t a_global, std::uint32_t a_virtual, std::uint32_t a_exports) {
			const auto base = 0x40 + a_index * 48;
			PutName(bytes, base, a_name);
			bytes[base + 19] = 0xFF;
			PutU32(bytes, base + 20, a_start);
			PutU32(bytes, base + 24, a_local);
			PutU32(bytes, base + 28, a_global);
			PutU32(bytes, base + 32, a_virtual);
			PutU32(bytes, base + 36, a_exports);
		};
		section(0, "__classnames__", classStart, 0, 0, 0, 0);
		section(1, "__types__", dataStart, 0, 0, 0, 0);
		section(2, "__data__", dataStart, localAt, virtualAt, virtualAt, exportsAt);
		PutName(bytes, classStart + 5, "hkbClipGenerator");
		PutName(bytes, classStart + 40, "hkbBehaviorReferenceGenerator");

		std::size_t object = 0, text = stringsAt, local = localAt, virt = virtualAt;
		auto        addString = [&](const std::string& a_text) {
			const auto at = text;
			PutName(bytes, dataStart + text, a_text);
			text += a_text.size() + 1;
			return static_cast<std::uint32_t>(at);
		};
		auto addFixup = [&](std::size_t a_source, std::uint32_t a_target) {
			PutU32(bytes, dataStart + local, static_cast<std::uint32_t>(a_source));
			PutU32(bytes, dataStart + local + 4, a_target);
			local += 8;
		};
		auto addObject = [&](std::uint32_t a_classOffset, const std::string& a_first, const std::string& a_second) {
			PutU32(bytes, dataStart + virt, static_cast<std::uint32_t>(object));
			PutU32(bytes, dataStart + virt + 4, 0);
			PutU32(bytes, dataStart + virt + 8, a_classOffset);
			virt += 12;
			addFixup(object + 0x10, addString(a_first));
			addFixup(object + 0x20, addString(a_second));
			object += objectSize;
		};
		for (const auto& clip : a_clips) {
			addObject(5, clip.name, clip.animation);
			if (a_junkPointer) {
				bytes[dataStart + text] = 0x01;
				addFixup(object - objectSize + 0x28, static_cast<std::uint32_t>(text));
				text += 2;
			}
		}
		for (const auto& reference : a_references)
			addObject(40, "Reference", reference);
		PutU32(bytes, dataStart + virt, 0xFFFFFFFF);
		PutU32(bytes, dataStart + local, 0xFFFFFFFF);
		return bytes;
	}
}

TEST_CASE("clip generators give each clip's name and animation file")
{
	const auto clips = ParseClips(MakeBehavior({ { "CiceroDance1", "Animations\\Special_CiceroDance1.HKX" }, { "IdleWave", "Animations\\IdleWave.hkx" } }, {}));
	CHECK(clips.size() == 2);
	if (clips.size() != 2)
		return;
	CHECK(clips[0].name == "CiceroDance1");
	CHECK(clips[0].animation == "Animations\\Special_CiceroDance1.HKX");
	CHECK(clips[1].name == "IdleWave");
}

TEST_CASE("a pointer to something other than text does not become a clip name or animation")
{
	const auto clips = ParseClips(MakeBehavior({ { "Wave", "Animations\\Wave.hkx" } }, {}, true));
	CHECK(clips.size() == 1);
	if (clips.empty())
		return;
	CHECK(clips[0].name == "Wave");
	CHECK(clips[0].animation == "Animations\\Wave.hkx");
}

TEST_CASE("behavior references name the files a graph hands control to")
{
	const auto references = ParseBehaviorReferences(MakeBehavior({ { "Wave", "Animations\\Wave.hkx" } }, { "Behaviors\\MT_Behavior.hkx", "Behaviors\\IdleBehavior.hkx" }));
	CHECK(references.size() == 2);
	if (references.size() != 2)
		return;
	CHECK(references[0] == "Behaviors\\MT_Behavior.hkx");
	CHECK(references[1] == "Behaviors\\IdleBehavior.hkx");
}

TEST_CASE("behavior parsing refuses bytes that are not a packfile and a truncated one")
{
	CHECK(ParseClips({}).empty());
	CHECK(ParseBehaviorReferences(std::vector<std::uint8_t>(0x400, 0)).empty());
	const auto whole = MakeBehavior({ { "Wave", "Animations\\Wave.hkx" } }, { "Behaviors\\MT_Behavior.hkx" });
	for (const std::size_t length : { std::size_t{ 0x40 }, std::size_t{ 0x210 }, std::size_t{ 0x2C0 }, std::size_t{ 0x330 } }) {
		const std::span<const std::uint8_t> cut(whole.data(), length);
		CHECK(ParseClips(cut).size() <= 1);
		CHECK(ParseBehaviorReferences(cut).size() <= 1);
	}
}

TEST_CASE("an event finds the clip named for it, with or without the Idle prefix, and prefers the exact name")
{
	const std::vector<Clip> clips{ { "CiceroDance1", "a.hkx" }, { "IdleCiceroDance1", "b.hkx" }, { "IdleWave", "c.hkx" }, { "Salute", "d.hkx" } };
	CHECK(FindClipForEvent(clips, "IdleCiceroDance1")->clip.animation == "b.hkx");
	CHECK(FindClipForEvent(clips, "IdleCiceroDance1")->kind == MatchKind::kExact);
	CHECK(FindClipForEvent(clips, "idlewave")->clip.animation == "c.hkx");
	CHECK(FindClipForEvent(clips, "IdleSalute")->clip.animation == "d.hkx");
	CHECK(FindClipForEvent(clips, "IdleSalute")->kind == MatchKind::kNoPrefix);
	CHECK(!FindClipForEvent(clips, "IdleDance").has_value());
	CHECK(!FindClipForEvent(clips, "Idle").has_value());
	CHECK(!FindClipForEvent(clips, "").has_value());
}

TEST_CASE("a clip with a prefix or with the name inside it is matched, weaker than an exact one")
{
	const std::vector<Clip> clips{ { "Tor_LightSconce", "a.hkx" }, { "MT_Wounded_0100", "b.hkx" }, { "MT_PointFar_01", "c.hkx" }, { "LongPointFar_01Variant", "d.hkx" } };
	const auto              suffix = FindClipForEvent(clips, "IdleLightSconce");
	CHECK(suffix.has_value());
	if (!suffix)
		return;
	CHECK(suffix->clip.animation == "a.hkx");
	CHECK(suffix->kind == MatchKind::kSuffix);

	const auto contains = FindClipForEvent(clips, "IdleWounded_01");
	CHECK(contains.has_value());
	if (!contains)
		return;
	CHECK(contains->clip.animation == "b.hkx");
	CHECK(contains->kind == MatchKind::kContains);

	const auto both = FindClipForEvent(clips, "IdlePointFar_01");
	CHECK(both.has_value());
	if (!both)
		return;
	CHECK(both->kind == MatchKind::kSuffix);
	CHECK(both->clip.animation == "c.hkx");
}

TEST_CASE("several clips matched alike are counted and the shortest name is chosen; short stems do not match inside names")
{
	const std::vector<Clip> clips{ { "MT_Sit_Long_Chair", "a.hkx" }, { "Sit_Chair", "b.hkx" } };
	const auto              match = FindClipForEvent(clips, "IdleSit_Chair");
	CHECK(match.has_value());
	if (!match)
		return;
	CHECK(match->clip.animation == "b.hkx");
	const std::vector<Clip> unrelated{ { "Stopwatch", "x.hkx" } };
	CHECK(!FindClipForEvent(unrelated, "IdleStop").has_value());
	CHECK(MatchKindName(MatchKind::kContains) == "contains");
}
