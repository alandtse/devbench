#include "HavokAnimationLogic.h"

#include "FormSearchLogic.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace dvb::HavokAnimationLogic
{
	namespace
	{
		constexpr std::uint32_t    kMagic0 = 0x57E0E057;
		constexpr std::uint32_t    kMagic1 = 0x10C0C010;
		constexpr std::size_t      kHeaderSize = 0x40;
		constexpr std::size_t      kSectionHeaderSize = 48;
		constexpr std::size_t      kSectionNameSize = 19;
		constexpr std::size_t      kVirtualFixupSize = 12;
		constexpr std::size_t      kLocalFixupSize = 8;
		constexpr std::uint32_t    kFixupEnd = 0xFFFFFFFF;
		constexpr std::size_t      kMaxSections = 8;
		constexpr float            kMaxDurationSeconds = 36000.0f;
		constexpr std::size_t      kAnimationFieldsAt64 = 0x10;  // after the vtable, size and reference count
		constexpr std::size_t      kAnimationFieldsAt32 = 0x08;
		constexpr std::size_t      kMaxStringBytes = 1024;
		constexpr std::string_view kIdlePrefix = "Idle";
		constexpr std::size_t      kMinContainsLength = 5;  // shorter stems (Stop, Wave) would match unrelated clips

		constexpr std::array<std::string_view, 5> kAnimationClasses{
			"hkaSplineCompressedAnimation",
			"hkaInterleavedUncompressedAnimation",
			"hkaDeltaCompressedAnimation",
			"hkaWaveletCompressedAnimation",
			"hkaQuantizedAnimation",
		};

		struct Section
		{
			std::string_view name;
			std::uint32_t    start{ 0 };
			std::uint32_t    localFixups{ 0 };
			std::uint32_t    globalFixups{ 0 };
			std::uint32_t    virtualFixups{ 0 };
			std::uint32_t    exports{ 0 };
		};

		struct Object
		{
			std::uint32_t    offset{ 0 };  // within the data section
			std::string_view className;
		};

		struct Packfile
		{
			std::span<const std::uint8_t> bytes;
			int                           pointerSize{ 0 };
			Section                       classNames;
			Section                       data;
		};

		bool ReadU32(std::span<const std::uint8_t> a_bytes, std::size_t a_offset, std::uint32_t& a_out)
		{
			if (a_offset > a_bytes.size() || a_bytes.size() - a_offset < sizeof(a_out))
				return false;
			std::memcpy(&a_out, a_bytes.data() + a_offset, sizeof(a_out));
			return true;
		}

		bool ReadFloat(std::span<const std::uint8_t> a_bytes, std::size_t a_offset, float& a_out)
		{
			if (a_offset > a_bytes.size() || a_bytes.size() - a_offset < sizeof(a_out))
				return false;
			std::memcpy(&a_out, a_bytes.data() + a_offset, sizeof(a_out));
			return true;
		}

		std::string_view CString(std::span<const std::uint8_t> a_bytes, std::size_t a_offset, std::size_t a_max = kMaxStringBytes)
		{
			if (a_offset >= a_bytes.size())
				return {};
			const auto* begin = reinterpret_cast<const char*>(a_bytes.data()) + a_offset;
			const auto  room = std::min(a_bytes.size() - a_offset, a_max);
			const auto  length = ::strnlen(begin, room);
			return length == room ? std::string_view{} : std::string_view{ begin, length };
		}

		// The section table follows the 0x40-byte header, or a further 0x10 bytes on to it in the newer packfile version.
		bool ReadSections(std::span<const std::uint8_t> a_bytes, std::uint32_t a_count, std::array<Section, kMaxSections>& a_out)
		{
			for (const std::size_t tableStart : { kHeaderSize, kHeaderSize + 0x10 }) {
				bool ok = true;
				for (std::uint32_t i = 0; i < a_count && ok; ++i) {
					const auto base = tableStart + i * kSectionHeaderSize;
					if (base > a_bytes.size() || a_bytes.size() - base < kSectionHeaderSize) {
						ok = false;
						break;
					}
					a_out[i].name = CString(a_bytes.subspan(base, kSectionNameSize + 1), 0);
					ok = ReadU32(a_bytes, base + 20, a_out[i].start) && ReadU32(a_bytes, base + 24, a_out[i].localFixups) && ReadU32(a_bytes, base + 28, a_out[i].globalFixups) && ReadU32(a_bytes, base + 32, a_out[i].virtualFixups) && ReadU32(a_bytes, base + 36, a_out[i].exports) && !a_out[i].name.empty();
				}
				if (ok)
					return true;
			}
			return false;
		}

		std::optional<Packfile> Open(std::span<const std::uint8_t> a_bytes)
		{
			std::uint32_t magic0 = 0, magic1 = 0, sectionCount = 0;
			if (!ReadU32(a_bytes, 0, magic0) || !ReadU32(a_bytes, 4, magic1) || magic0 != kMagic0 || magic1 != kMagic1)
				return std::nullopt;
			if (a_bytes.size() < kHeaderSize || (a_bytes[16] != 8 && a_bytes[16] != 4) || a_bytes[17] != 1)
				return std::nullopt;
			if (!ReadU32(a_bytes, 20, sectionCount) || sectionCount == 0 || sectionCount > kMaxSections)
				return std::nullopt;

			std::array<Section, kMaxSections> sections{};
			if (!ReadSections(a_bytes, sectionCount, sections))
				return std::nullopt;

			std::optional<std::size_t> classNames, data;
			for (std::size_t i = 0; i < sectionCount; ++i) {
				if (sections[i].name == "__classnames__")
					classNames = i;
				else if (sections[i].name == "__data__")
					data = i;
			}
			if (!classNames || !data)
				return std::nullopt;
			return Packfile{ a_bytes, a_bytes[16], sections[*classNames], sections[*data] };
		}

		std::vector<Object> Objects(const Packfile& a_file)
		{
			std::vector<Object> objects;
			const auto          begin = static_cast<std::size_t>(a_file.data.start) + a_file.data.virtualFixups;
			const auto          end = static_cast<std::size_t>(a_file.data.start) + a_file.data.exports;
			for (auto at = begin; end >= at && end - at >= kVirtualFixupSize; at += kVirtualFixupSize) {
				std::uint32_t source = 0, targetSection = 0, nameOffset = 0;
				if (!ReadU32(a_file.bytes, at, source) || source == kFixupEnd)
					break;
				if (!ReadU32(a_file.bytes, at + 4, targetSection) || !ReadU32(a_file.bytes, at + 8, nameOffset))
					continue;
				const auto name = CString(a_file.bytes, static_cast<std::size_t>(a_file.classNames.start) + nameOffset);
				if (!name.empty())
					objects.push_back({ source, name });
			}
			std::ranges::sort(objects, {}, &Object::offset);
			return objects;
		}

		struct LocalFixup
		{
			std::uint32_t source{ 0 };
			std::uint32_t target{ 0 };
		};

		std::vector<LocalFixup> LocalFixups(const Packfile& a_file)
		{
			std::vector<LocalFixup> fixups;
			const auto              begin = static_cast<std::size_t>(a_file.data.start) + a_file.data.localFixups;
			const auto              end = static_cast<std::size_t>(a_file.data.start) + a_file.data.globalFixups;
			for (auto at = begin; end >= at && end - at >= kLocalFixupSize; at += kLocalFixupSize) {
				LocalFixup fixup;
				if (!ReadU32(a_file.bytes, at, fixup.source) || fixup.source == kFixupEnd || !ReadU32(a_file.bytes, at + 4, fixup.target))
					break;
				fixups.push_back(fixup);
			}
			std::ranges::sort(fixups, {}, &LocalFixup::source);
			return fixups;
		}

		bool IsPrintable(std::string_view a_text)
		{
			return !a_text.empty() && std::ranges::all_of(a_text, [](unsigned char c) { return c >= 0x20 && c < 0x7F; });
		}

		std::vector<std::string_view> StringsOf(const Packfile& a_file, const std::vector<Object>& a_objects, std::size_t a_index, const std::vector<LocalFixup>& a_fixups)
		{
			const auto                    first = a_objects[a_index].offset;
			const auto                    last = a_index + 1 < a_objects.size() ? a_objects[a_index + 1].offset : a_file.data.localFixups;
			auto                          it = std::ranges::lower_bound(a_fixups, first, {}, &LocalFixup::source);
			std::vector<std::string_view> strings;
			for (; it != a_fixups.end() && it->source < last; ++it) {
				const auto text = CString(a_file.bytes, static_cast<std::size_t>(a_file.data.start) + it->target);
				if (IsPrintable(text))
					strings.push_back(text);
			}
			return strings;
		}

		bool StartsAWord(std::string_view a_name, std::string_view a_text)
		{
			for (std::size_t at = 0; at + a_text.size() <= a_name.size(); ++at)
				if ((at == 0 || a_name[at - 1] == '_') && FormSearchLogic::EqualsCI(a_name.substr(at, a_text.size()), a_text))
					return true;
			return false;
		}

		bool EndsWithHkx(std::string_view a_text)
		{
			constexpr std::string_view kExtension = ".hkx";
			return a_text.size() >= kExtension.size() && FormSearchLogic::EqualsCI(a_text.substr(a_text.size() - kExtension.size()), kExtension);
		}
	}

	std::optional<AnimationInfo> ParseAnimation(std::span<const std::uint8_t> a_bytes)
	{
		const auto file = Open(a_bytes);
		if (!file)
			return std::nullopt;

		for (const auto& object : Objects(*file)) {
			if (std::ranges::find(kAnimationClasses, object.className) == kAnimationClasses.end())
				continue;

			const auto    start = static_cast<std::size_t>(file->data.start) + object.offset;
			const auto    fields = start + (file->pointerSize == 8 ? kAnimationFieldsAt64 : kAnimationFieldsAt32);
			float         duration = 0.0f;
			std::uint32_t transformTracks = 0, floatTracks = 0;
			if (!ReadFloat(a_bytes, fields + 4, duration) || !ReadU32(a_bytes, fields + 8, transformTracks) || !ReadU32(a_bytes, fields + 12, floatTracks))
				return std::nullopt;
			if (!std::isfinite(duration) || duration <= 0.0f || duration > kMaxDurationSeconds)
				return std::nullopt;

			return AnimationInfo{ std::string(object.className), duration, static_cast<int>(transformTracks), static_cast<int>(floatTracks), file->pointerSize };
		}
		return std::nullopt;
	}

	std::vector<Clip> ParseClips(std::span<const std::uint8_t> a_bytes)
	{
		std::vector<Clip> clips;
		const auto        file = Open(a_bytes);
		if (!file)
			return clips;
		const auto objects = Objects(*file);
		const auto fixups = LocalFixups(*file);
		for (std::size_t i = 0; i < objects.size(); ++i) {
			if (objects[i].className != "hkbClipGenerator")
				continue;
			const auto strings = StringsOf(*file, objects, i, fixups);
			const auto animation = std::ranges::find_if(strings, EndsWithHkx);
			if (strings.empty() || animation == strings.end())
				continue;
			clips.push_back({ std::string(strings.front()), std::string(*animation) });
		}
		return clips;
	}

	std::vector<std::string> ParseBehaviorReferences(std::span<const std::uint8_t> a_bytes)
	{
		std::vector<std::string> references;
		const auto               file = Open(a_bytes);
		if (!file)
			return references;
		const auto objects = Objects(*file);
		const auto fixups = LocalFixups(*file);
		for (std::size_t i = 0; i < objects.size(); ++i) {
			if (objects[i].className != "hkbBehaviorReferenceGenerator")
				continue;
			for (const auto text : StringsOf(*file, objects, i, fixups))
				if (EndsWithHkx(text))
					references.emplace_back(text);
		}
		return references;
	}

	std::string_view MatchKindName(MatchKind a_kind)
	{
		switch (a_kind) {
		case MatchKind::kExact:
			return "exact";
		case MatchKind::kNoPrefix:
			return "noIdlePrefix";
		case MatchKind::kSuffix:
			return "suffix";
		case MatchKind::kContains:
			return "contains";
		}
		return {};
	}

	std::optional<ClipMatch> FindClipForEvent(const std::vector<Clip>& a_clips, std::string_view a_event)
	{
		if (a_event.empty())
			return std::nullopt;
		const bool        hasPrefix = a_event.size() > kIdlePrefix.size() && FormSearchLogic::EqualsCI(a_event.substr(0, kIdlePrefix.size()), kIdlePrefix);
		const auto        stem = hasPrefix ? a_event.substr(kIdlePrefix.size()) : a_event;
		const std::string underscored = "_" + std::string(stem);

		auto kindOf = [&](std::string_view a_name) -> std::optional<MatchKind> {
			if (FormSearchLogic::EqualsCI(a_name, a_event))
				return MatchKind::kExact;
			if (hasPrefix && FormSearchLogic::EqualsCI(a_name, stem))
				return MatchKind::kNoPrefix;
			if (a_name.size() > underscored.size() && FormSearchLogic::EqualsCI(a_name.substr(a_name.size() - underscored.size()), underscored))
				return MatchKind::kSuffix;
			if (stem.size() >= kMinContainsLength && StartsAWord(a_name, stem))
				return MatchKind::kContains;
			return std::nullopt;
		};

		std::optional<ClipMatch> best;
		for (const auto& clip : a_clips) {
			const auto kind = kindOf(clip.name);
			if (!kind)
				continue;
			if (!best || *kind < best->kind) {
				best = ClipMatch{ clip, *kind, 1 };
			} else if (*kind == best->kind) {
				++best->candidates;
				if (clip.name.size() < best->clip.name.size())
					best->clip = clip;
			}
		}
		return best;
	}
}
