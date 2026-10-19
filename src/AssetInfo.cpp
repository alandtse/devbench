#include "AssetInfo.h"

#include "FormSearchLogic.h"
#include "HavokAnimationLogic.h"
#include "PluginHeaderLogic.h"
#include "ToolRegistry.h"

#include <RE/B/BSResourceNiBinaryStream.h>
#include <RE/T/TESDataHandler.h>
#include <RE/T/TESFile.h>

#include <algorithm>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace dvb::AssetInfo
{
	namespace
	{
		constexpr std::size_t kMaxHeaderBytes = 4u << 20;
		constexpr std::size_t kMaxFileBytes = 32u << 20;
		constexpr std::size_t kReadChunk = 64u << 10;
		constexpr std::size_t kMaxBehaviorsSearched = 64;

		struct Behavior
		{
			std::vector<HavokAnimationLogic::Clip> clips;
			std::vector<std::string>               references;
			bool                                   readable{ false };
		};

		std::mutex                      g_cacheLock;
		std::map<std::string, json>     g_pluginCache;  // by lower-case file name; plugin headers do not change while the game runs
		std::map<std::string, Behavior> g_behaviors;    // by lower-case path; game files do not change while the game runs

		std::string Lower(std::string_view a_text)
		{
			std::string out(a_text);
			std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return out;
		}

		std::string WithBackslashes(std::string a_path)
		{
			std::ranges::replace(a_path, '/', '\\');
			return a_path;
		}

		bool IsPlainFileName(std::string_view a_name)
		{
			return !a_name.empty() && a_name.find_first_of("\\/:") == std::string_view::npos && a_name != "." && a_name != "..";
		}

		bool IsSafeGamePath(std::string_view a_path)
		{
			return !a_path.empty() && a_path.find("..") == std::string_view::npos && a_path.find(':') == std::string_view::npos && a_path.front() != '\\' && a_path.front() != '/';
		}

		std::vector<std::uint8_t> ReadFilePrefix(const std::string& a_path, std::size_t a_count)
		{
			std::ifstream             file(a_path, std::ios::binary);
			std::vector<std::uint8_t> bytes(a_count);
			file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(a_count));
			bytes.resize(static_cast<std::size_t>(std::max<std::streamsize>(file.gcount(), 0)));
			return bytes;
		}

		json ReadPlugin(const std::string& a_name)
		{
			const auto path = "Data\\" + a_name;
			auto       bytes = ReadFilePrefix(path, 24);
			const auto size = PluginHeaderLogic::RecordSize(bytes, kMaxHeaderBytes);
			if (!size)
				return nullptr;
			bytes = ReadFilePrefix(path, *size);
			const auto header = PluginHeaderLogic::Parse(bytes);
			if (!header)
				return nullptr;
			return json{
				{ "name", a_name },
				{ "author", header->author },
				{ "description", header->description },
				{ "version", header->version },
				{ "recordCount", header->recordCount },
				{ "masters", header->masters },
				{ "master", header->master },
				{ "light", header->light },
			};
		}

		// The stream does not report its size and a short read reports only failure, so the bytes a failed chunk did deliver
		// are counted from how far the stream advanced.
		bool ReadStream(const std::string& a_path, std::vector<std::uint8_t>& a_out)
		{
			RE::BSResourceNiBinaryStream stream(a_path);
			if (!stream.good())
				return false;
			std::vector<std::uint8_t> chunk(kReadChunk);
			while (a_out.size() < kMaxFileBytes) {
				const auto before = stream.tell();
				const bool whole = stream.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::uint32_t>(chunk.size()));
				const auto got = whole ? chunk.size() : std::min<std::size_t>(stream.tell() - before, chunk.size());
				a_out.insert(a_out.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(got));
				if (!whole)
					break;
			}
			return !a_out.empty();
		}

		bool ReadGameFile(const std::string& a_path, std::vector<std::uint8_t>& a_bytes, std::string& a_resolved)
		{
			for (const auto& candidate : { "Meshes\\" + a_path, a_path }) {
				a_bytes.clear();
				if (ReadStream(candidate, a_bytes)) {
					a_resolved = candidate;
					return true;
				}
			}
			return false;
		}

		json DescribeFile(const std::string& a_path, std::vector<std::uint8_t>& a_bytes)
		{
			json        out{ { "path", a_path }, { "found", false } };
			std::string resolved;
			if (!IsSafeGamePath(a_path) || !ReadGameFile(WithBackslashes(a_path), a_bytes, resolved))
				return out;
			out["found"] = true;
			out["resolvedPath"] = resolved;
			out["bytes"] = a_bytes.size();
			if (const auto info = HavokAnimationLogic::ParseAnimation(a_bytes)) {
				out["className"] = info->className;
				out["durationSeconds"] = info->durationSeconds;
				out["transformTracks"] = info->transformTracks;
				out["floatTracks"] = info->floatTracks;
			}
			return out;
		}

		std::string ProjectDirectory(const std::string& a_behaviorPath)
		{
			const auto file = a_behaviorPath.rfind('\\');
			if (file == std::string::npos || file == 0)
				return {};
			const auto folder = a_behaviorPath.rfind('\\', file - 1);
			return folder == std::string::npos ? std::string{} : a_behaviorPath.substr(0, folder + 1);
		}

		const Behavior& LoadBehavior(const std::string& a_path)
		{
			const auto            key = Lower(a_path);
			const std::lock_guard guard{ g_cacheLock };
			if (const auto it = g_behaviors.find(key); it != g_behaviors.end())
				return it->second;

			Behavior                  behavior;
			std::vector<std::uint8_t> bytes;
			std::string               resolved;
			if (ReadGameFile(a_path, bytes, resolved)) {
				behavior.clips = HavokAnimationLogic::ParseClips(bytes);
				behavior.references = HavokAnimationLogic::ParseBehaviorReferences(bytes);
				behavior.readable = true;
			}
			return g_behaviors.emplace(key, std::move(behavior)).first->second;
		}
	}

	json Plugin(std::string_view a_name)
	{
		if (!IsPlainFileName(a_name))
			return nullptr;
		const auto key = Lower(a_name);
		{
			const std::lock_guard guard{ g_cacheLock };
			if (const auto it = g_pluginCache.find(key); it != g_pluginCache.end())
				return it->second;
		}
		auto                  info = ReadPlugin(std::string(a_name));
		const std::lock_guard guard{ g_cacheLock };
		return g_pluginCache.emplace(key, std::move(info)).first->second;
	}

	json Plugins(const std::string& a_filter, std::size_t a_limit, std::size_t a_offset)
	{
		auto* dh = RE::TESDataHandler::GetSingleton();
		if (!dh)
			throw ToolError(503, "TESDataHandler unavailable (no data loaded?)");

		json        out = json::array();
		std::size_t matched = 0;
		auto        matches = [&](const json& a_info) {
			return a_filter.empty() || FormSearchLogic::ContainsCI(a_info.value("name", std::string{}), a_filter) || FormSearchLogic::ContainsCI(a_info.value("author", std::string{}), a_filter) || FormSearchLogic::ContainsCI(a_info.value("description", std::string{}), a_filter);
		};
		auto visit = [&](const RE::TESFile* const* a_files, std::size_t a_count, bool a_light) {
			for (std::size_t i = 0; i < a_count; ++i) {
				const auto* file = a_files[i];
				if (!file)
					continue;
				json info = Plugin(file->GetFilename());
				if (!info.is_object())
					info = json{ { "name", std::string(file->GetFilename()) } };
				if (!matches(info))
					continue;
				if (matched >= a_offset && out.size() < a_limit) {
					info["index"] = a_light ? file->GetSmallFileCompileIndex() : file->GetCompileIndex();
					info["lightIndex"] = a_light;
					out.push_back(std::move(info));
				}
				++matched;
			}
		};
		visit(dh->GetLoadedMods(), dh->GetLoadedModCount(), false);
		visit(dh->GetLoadedLightMods(), dh->GetLoadedLightModCount(), true);

		return json{ { "total", matched }, { "offset", a_offset }, { "returned", out.size() }, { "truncated", a_offset + out.size() < matched }, { "plugins", std::move(out) } };
	}

	json Animation(const std::string& a_path)
	{
		std::vector<std::uint8_t> bytes;
		return DescribeFile(a_path, bytes);
	}

	json IdleAnimation(const std::string& a_file, const std::string& a_event)
	{
		std::vector<std::uint8_t> bytes;
		json                      file = DescribeFile(a_file, bytes);
		if (!file["found"].get<bool>() || file.contains("className")) {
			file["source"] = "file";
			return file;
		}

		json out{ { "source", "behavior" }, { "behavior", a_file }, { "event", a_event }, { "found", false } };
		if (HavokAnimationLogic::ParseClips(bytes).empty() && HavokAnimationLogic::ParseBehaviorReferences(bytes).empty()) {
			out["note"] = "the idle's file is neither an animation nor a behavior graph this reader understands";
			return out;
		}

		const auto              project = ProjectDirectory(WithBackslashes(a_file));
		std::deque<std::string> pending{ WithBackslashes(a_file) };
		std::set<std::string>   seen{ Lower(pending.front()) };
		std::size_t             searched = 0;
		while (!pending.empty() && searched < kMaxBehaviorsSearched) {
			const auto path = pending.front();
			pending.pop_front();
			++searched;
			const auto& behavior = LoadBehavior(path);
			if (const auto match = HavokAnimationLogic::FindClipForEvent(behavior.clips, a_event)) {
				json animation = Animation(project + WithBackslashes(match->clip.animation));
				animation["source"] = "behavior";
				animation["behavior"] = path;
				animation["event"] = a_event;
				animation["clip"] = match->clip.name;
				animation["animationName"] = match->clip.animation;
				animation["match"] = std::string(HavokAnimationLogic::MatchKindName(match->kind));
				animation["candidates"] = match->candidates;
				return animation;
			}
			for (const auto& reference : behavior.references) {
				const auto next = project + WithBackslashes(reference);
				if (seen.insert(Lower(next)).second)
					pending.push_back(next);
			}
		}
		out["note"] = "no clip named for the event in the behavior graph or the behavior files it references (" + std::to_string(searched) + " searched); the event may reach its clip through the graph's transitions";
		return out;
	}
}
