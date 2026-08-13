#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "Json.h"

// Pure image-comparison primitives — decode, crop, SSIM, golden scoring. Zero dependency on
// RE/SKSE (only stb_image + std::filesystem + json), so this is unit-testable in the
// host-independent `devbench-tests` target (see tests/Ssim_test.cpp) with no game required.
// `Capture.cpp` is the only real caller — it resolves a relative golden path against GameRoot()
// BEFORE calling in here, so this module never needs to know anything about "the game" at all.
namespace dvb::Ssim
{
	struct GrayImage
	{
		std::vector<float> px;  // row-major, 0..255 range as float (SSIM math needs float)
		int                width = 0;
		int                height = 0;
	};

	/// Decode a PNG/BMP file into a single-channel grayscale buffer, or nullopt if the file
	/// doesn't exist, isn't readable, or isn't a supported format.
	std::optional<GrayImage> DecodeGray(const std::filesystem::path& a_path);

	/// Crop a 0..1 UV rect {x,y,w,h} — the SAME convention the `capture` tool's own `subrect`
	/// arg and tests/http/visual.py's regions both use, so one rect definition works everywhere.
	GrayImage CropUv(const GrayImage& a_img, const json& a_uv);

	/// Windowed SSIM (Wang et al.), OVERLAPPING windows (stride shorter than the window) — see
	/// Ssim.cpp for why overlap is load-bearing, not stylistic: a first non-overlapping-block
	/// version failed a basic sanity check (a fully value-inverted image scored 0.54, not
	/// strongly negative) because content flat within each block has zero intra-block variance,
	/// blinding the contrast term to inversion regardless of sign.
	double ComputeSsim(const GrayImage& a_a, const GrayImage& a_b);

	struct RegionScore
	{
		std::string name;
		double      score = 0.0;
		double      threshold = 0.0;
		bool        passed = false;
	};

	// Everything a `golden` result needs, whether it succeeded or not — a decode failure
	// (missing golden, corrupt file, dimension mismatch) is reported via `error`, not thrown: a
	// checkpoint that can't be scored still has a real capture worth returning, it just can't
	// carry a verdict.
	struct GoldenScore
	{
		bool                     ok = false;
		std::string              error;
		double                   score = 0.0;
		double                   threshold = 0.0;
		bool                     passed = false;
		std::vector<RegionScore> regions;
	};

	/// Score `a_capturedPath` against `a_goldenPath` — BOTH must already be resolved, absolute
	/// paths; this module does no path resolution of its own. `a_goldenCfg`'s optional
	/// "threshold" (default 0.98) and "regions" ([{name,x,y,w,h,threshold?}]) control scoring.
	/// Never throws.
	GoldenScore ScoreAgainstGolden(const std::filesystem::path& a_capturedPath,
		const std::filesystem::path& a_goldenPath, const json& a_goldenCfg);
}
