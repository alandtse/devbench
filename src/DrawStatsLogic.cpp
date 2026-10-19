#include "DrawStatsLogic.h"

#include <algorithm>
#include <format>
#include <utility>

namespace dvb::DrawStatsLogic
{
	std::string_view PassName(Pass a_pass)
	{
		switch (a_pass) {
		case Pass::kSun:
			return "sunShadows";
		case Pass::kSpot:
			return "spotShadows";
		case Pass::kPoint:
			return "pointShadows";
		default:
			return "main";
		}
	}

	std::size_t CasterSizeBin(float a_radius, float a_distance)
	{
		const float size = a_radius / std::max(a_distance, 1.0f);
		std::size_t bin = 0;
		while (bin < kCasterSizeEdges.size() && size >= kCasterSizeEdges[bin])
			++bin;
		return bin;
	}

	std::string TargetLabel(const Target& a_target, std::uint32_t a_screenWidth, std::uint32_t a_screenHeight)
	{
		std::string label;
		if (a_screenWidth && a_target.width == a_screenWidth && a_target.height == a_screenHeight)
			label = "screen";
		else if (a_screenWidth && a_target.width == a_screenWidth / 2 && a_target.height == a_screenHeight / 2)
			label = "half screen";
		else if (a_target.width && a_target.width == a_target.height)
			label = std::format("square map {}", a_target.width);
		else
			label = std::format("{}x{}", a_target.width, a_target.height);
		if (a_target.depth)
			label += " depth";
		return label;
	}

	double PerFrame(std::uint64_t a_from, std::uint64_t a_to, std::uint64_t a_frames)
	{
		if (a_frames == 0 || a_to < a_from)
			return 0.0;
		return static_cast<double>(a_to - a_from) / static_cast<double>(a_frames);
	}

	namespace
	{
		json ByShader(const std::array<std::uint64_t, kShaderCount>& a_from, const std::array<std::uint64_t, kShaderCount>& a_to,
			std::uint64_t a_frames, double& a_total)
		{
			json out = json::object();
			a_total = 0.0;
			for (std::size_t s = 0; s < kShaderCount; ++s) {
				const double v = PerFrame(a_from[s], a_to[s], a_frames);
				a_total += v;
				if (v > 0.0)
					out[std::string(kShaderNames[s])] = v;
			}
			return out;
		}

		double NsToMs(std::uint64_t a_from, std::uint64_t a_to, std::uint64_t a_frames) { return PerFrame(a_from, a_to, a_frames) / 1.0e6; }
	}

	json Reading(const Counts& a, const Counts& b, const std::vector<Target>& a_targets, bool a_targetsDropped,
		std::uint32_t a_screenWidth, std::uint32_t a_screenHeight, bool a_gpuTimed, double a_ticksPerSecond)
	{
		const std::uint64_t frames = b.frames - a.frames;
		const std::uint64_t gpuFrames = b.gpuFrames - a.gpuFrames;
		const auto          ms = [&](std::uint64_t a_ticks) { return a_ticksPerSecond > 0.0 ? static_cast<double>(a_ticks) * 1000.0 / a_ticksPerSecond : 0.0; };
		const auto          msPerFrame = [&](std::uint64_t a_from, std::uint64_t a_to) {
			return frames && a_to >= a_from ? ms(a_to - a_from) / static_cast<double>(frames) : 0.0;
		};
		const double seconds = b.at >= a.at ? ms(b.at - a.at) / 1000.0 : 0.0;
		json         out{
			{ "frames", frames },
			{ "seconds", seconds },
			{ "fps", seconds > 0.0 ? static_cast<double>(frames) / seconds : 0.0 },
			{ "frameMs", msPerFrame(a.frameTicks, b.frameTicks) },
			{ "gpu", json{
						 { "timed", a_gpuTimed },
						 { "framesTimed", gpuFrames },
						 { "framesLost", b.gpuLost - a.gpuLost },
						 { "frameMs", NsToMs(a.gpuFrameNs, b.gpuFrameNs, gpuFrames) },
					 } },
		};

		json passes = json::array();
		for (std::size_t p = 0; p < kPassCount; ++p) {
			double objects = 0.0;
			json   pass{
				{ "pass", std::string(PassName(static_cast<Pass>(p))) },
				{ "objectsByShader", ByShader(a.objects[p], b.objects[p], frames, objects) },
			};
			pass["objects"] = objects;
			if (static_cast<Pass>(p) != Pass::kMain) {
				pass["renders"] = PerFrame(a.renders[p], b.renders[p], frames);
				pass["renderCpuMs"] = msPerFrame(a.renderTicks[p], b.renderTicks[p]);
				pass["accumulateCpuMs"] = msPerFrame(a.accumulateTicks[p], b.accumulateTicks[p]);
				pass["gpuMs"] = NsToMs(a.gpuNs[p], b.gpuNs[p], gpuFrames);
				pass["casters"] = PerFrame(a.casters[p], b.casters[p], frames);
				json sizes = json::array();
				for (std::size_t bin = 0; bin < kCasterSizeBins; ++bin) {
					json row{ { "count", PerFrame(a.casterSizes[p][bin], b.casterSizes[p][bin], frames) } };
					if (bin > 0)
						row["atLeast"] = kCasterSizeEdges[bin - 1];
					if (bin < kCasterSizeEdges.size())
						row["below"] = kCasterSizeEdges[bin];
					sizes.push_back(std::move(row));
				}
				pass["casterSizes"] = std::move(sizes);
			}
			if (static_cast<Pass>(p) == Pass::kSun) {
				json cascades = json::array();
				for (std::size_t i = 0; i < kMaxCascades; ++i)
					if (const double v = PerFrame(a.cascadeObjects[i], b.cascadeObjects[i], frames); v > 0.0)
						cascades.push_back(v);
				pass["objectsPerDepthTarget"] = std::move(cascades);
			}
			passes.push_back(std::move(pass));
		}
		out["passes"] = std::move(passes);

		struct Row
		{
			double gpuMs = 0.0;
			double objects = 0.0;
			json   body;
		};
		std::vector<Row>  rows;
		const std::size_t count = std::min(a_targets.size(), kMaxTargets);
		for (std::size_t t = 0; t < count; ++t) {
			Row row;
			row.gpuMs = NsToMs(a.targetGpuNs[t], b.targetGpuNs[t], gpuFrames);
			const auto& info = a_targets[t];
			row.body = json{
				{ "label", TargetLabel(info, a_screenWidth, a_screenHeight) },
				{ "width", info.width },
				{ "height", info.height },
				{ "format", info.format },
				{ "arraySize", info.arraySize },
				{ "depth", info.depth },
				{ "objectsByShader", ByShader(a.targetObjects[t], b.targetObjects[t], frames, row.objects) },
				{ "gpuMs", row.gpuMs },
			};
			row.body["objects"] = row.objects;
			rows.push_back(std::move(row));
		}
		std::ranges::stable_sort(rows, [](const Row& x, const Row& y) { return std::pair(x.gpuMs, x.objects) > std::pair(y.gpuMs, y.objects); });
		json targets = json::array();
		for (auto& row : rows)
			targets.push_back(std::move(row.body));
		out["mainPassTargets"] = std::move(targets);
		out["mainPassTargetsCapped"] = a_targetsDropped;
		return out;
	}

	std::string ProviderSource(const json& a_descriptor)
	{
		if (a_descriptor.is_object()) {
			const auto it = a_descriptor.find("source");
			if (it != a_descriptor.end() && it->is_string() && !it->get_ref<const std::string&>().empty())
				return it->get<std::string>();
		}
		return "extension";
	}

	json WithSource(json a_reading, std::string_view a_source)
	{
		if (!a_reading.is_object())
			a_reading = json{ { "reading", std::move(a_reading) } };
		a_reading["source"] = std::string(a_source);
		return a_reading;
	}
}
