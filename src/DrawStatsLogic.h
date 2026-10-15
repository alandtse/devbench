#pragma once

#include "Json.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dvb::DrawStatsLogic
{
	/// Where an object is drawn: inside one kind of shadow-casting light's Render, or anywhere else.
	enum class Pass : std::uint8_t
	{
		kMain,
		kSun,
		kSpot,
		kPoint,
	};
	inline constexpr std::size_t kPassCount = 4;

	/// The JSON name of a pass: "main", "sunShadows", "spotShadows", "pointShadows".
	std::string_view PassName(Pass a_pass);

	/// The nine BSShader types, in the order their SetupGeometry hooks index them.
	inline constexpr std::size_t                                kShaderCount = 9;
	inline constexpr std::array<std::string_view, kShaderCount> kShaderNames{ "Lighting", "Utility", "Effect", "Grass", "Sky", "Water", "Particle",
		"BloodSplatter", "DistantTree" };

	/// A shadow caster's size on screen is its bound radius over its distance from the camera; the bins are split at these
	/// edges, so bin 0 is below the first edge and the last bin is at or above the last one.
	inline constexpr std::array<float, 6> kCasterSizeEdges{ 0.001f, 0.002f, 0.005f, 0.01f, 0.02f, 0.05f };
	inline constexpr std::size_t          kCasterSizeBins = kCasterSizeEdges.size() + 1;

	/// The bin of a caster with this bound radius at this distance; a distance under 1 counts as 1.
	std::size_t CasterSizeBin(float a_radius, float a_distance);

	/// A render target as the main pass binds it: the depth view when there is one, else the first color view.
	struct Target
	{
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::uint32_t format = 0;  // DXGI_FORMAT
		std::uint32_t arraySize = 0;
		bool          depth = false;

		bool operator==(const Target&) const = default;
	};

	/// "screen", "half screen" or "square map" when the size says so, else "<width>x<height>"; a depth target ends in " depth".
	std::string TargetLabel(const Target& a_target, std::uint32_t a_screenWidth, std::uint32_t a_screenHeight);

	/// A counter's change over a window, per frame; 0 when the window has no frames.
	double PerFrame(std::uint64_t a_from, std::uint64_t a_to, std::uint64_t a_frames);

	inline constexpr std::size_t kMaxTargets = 32;
	inline constexpr std::size_t kMaxCascades = 8;

	/// Every counter at one moment. CPU times are QPC ticks, GPU times nanoseconds; per-target arrays follow the target list.
	struct Counts
	{
		std::uint64_t                                                      at = 0;
		std::uint64_t                                                      frames = 0;
		std::uint64_t                                                      frameTicks = 0;
		std::array<std::uint64_t, kPassCount>                              renders{};
		std::array<std::uint64_t, kPassCount>                              renderTicks{};
		std::array<std::uint64_t, kPassCount>                              accumulateTicks{};
		std::array<std::uint64_t, kPassCount>                              casters{};
		std::array<std::array<std::uint64_t, kShaderCount>, kPassCount>    objects{};
		std::array<std::array<std::uint64_t, kCasterSizeBins>, kPassCount> casterSizes{};
		std::array<std::uint64_t, kMaxCascades>                            cascadeObjects{};
		std::array<std::array<std::uint64_t, kShaderCount>, kMaxTargets>   targetObjects{};
		std::array<std::uint64_t, kPassCount>                              gpuNs{};
		std::array<std::uint64_t, kMaxTargets>                             targetGpuNs{};
		std::uint64_t                                                      gpuFrameNs = 0;
		std::uint64_t                                                      gpuFrames = 0;
		std::uint64_t                                                      gpuLost = 0;
	};

	/// The `inspect kind=draws` reading of the frames between two counts, per frame. Targets come highest GPU time first,
	/// then most objects. `a_targetsDropped`: the collector refused a target because the list was full.
	json Reading(const Counts& a_from, const Counts& a_to, const std::vector<Target>& a_targets, bool a_targetsDropped,
		std::uint32_t a_screenWidth, std::uint32_t a_screenHeight, bool a_gpuTimed, double a_ticksPerSecond);

	/// The `source` of a reading taken by devbench's own hooks.
	inline constexpr std::string_view kOwnSource = "devbench";

	/// A provider's `source`: its descriptor's non-empty "source" string, else "extension".
	std::string ProviderSource(const json& a_descriptor);

	/// The reading with "source" set (a provider's own "source" key is overwritten); a reading that is not an object is
	/// kept under "reading".
	json WithSource(json a_reading, std::string_view a_source);
}
