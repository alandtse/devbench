#include "Ssim.h"

// Decode-only: two already-captured images -> pixel buffers. STBI_ONLY_* keeps the codec
// surface to exactly the formats devbench's own captures can actually be (provider captures are
// PNG per the provider contract; the native fallback can also land BMP depending on the user's
// vanilla screenshot .ini setting) -- not a general-purpose image loader.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace dvb::Ssim
{
	namespace
	{
		namespace fs = std::filesystem;
	}

	std::optional<GrayImage> DecodeGray(const fs::path& a_path)
	{
		int            w = 0, h = 0, channels = 0;
		const auto     pathStr = a_path.string();
		unsigned char* data = stbi_load(pathStr.c_str(), &w, &h, &channels, 1);  // force 1-channel grayscale
		if (!data)
			return std::nullopt;
		GrayImage img;
		img.width = w;
		img.height = h;
		img.px.resize(static_cast<size_t>(w) * h);
		for (size_t i = 0; i < img.px.size(); ++i)
			img.px[i] = static_cast<float>(data[i]);
		stbi_image_free(data);
		return img;
	}

	GrayImage CropUv(const GrayImage& a_img, const json& a_uv)
	{
		const double x0d = a_uv.value("x", 0.0), y0d = a_uv.value("y", 0.0);
		const double wd = a_uv.value("w", 1.0), hd = a_uv.value("h", 1.0);
		const int    x0 = std::clamp(static_cast<int>(std::lround(x0d * a_img.width)), 0, a_img.width);
		const int    y0 = std::clamp(static_cast<int>(std::lround(y0d * a_img.height)), 0, a_img.height);
		const int    x1 = std::clamp(static_cast<int>(std::lround((x0d + wd) * a_img.width)), x0, a_img.width);
		const int    y1 = std::clamp(static_cast<int>(std::lround((y0d + hd) * a_img.height)), y0, a_img.height);

		GrayImage out;
		out.width = x1 - x0;
		out.height = y1 - y0;
		out.px.resize(static_cast<size_t>(out.width) * out.height);
		for (int y = 0; y < out.height; ++y)
			for (int x = 0; x < out.width; ++x)
				out.px[static_cast<size_t>(y) * out.width + x] = a_img.px[static_cast<size_t>(y0 + y) * a_img.width + (x0 + x)];
		return out;
	}

	double ComputeSsim(const GrayImage& a_a, const GrayImage& a_b)
	{
		constexpr double kC1 = (0.01 * 255.0) * (0.01 * 255.0);
		constexpr double kC2 = (0.03 * 255.0) * (0.03 * 255.0);
		constexpr int    kWindow = 8;
		constexpr int    kStride = 3;  // < kWindow so windows overlap and straddle any block-periodic content

		const int w = a_a.width, h = a_a.height;
		if (w <= 0 || h <= 0)
			return 0.0;
		const int win = std::min({ kWindow, w, h });
		if (win <= 1)
			return 0.0;

		double sum = 0.0;
		int    windows = 0;
		for (int by = 0; by <= h - win; by += kStride) {
			for (int bx = 0; bx <= w - win; bx += kStride) {
				const int n = win * win;

				double meanA = 0.0, meanB = 0.0;
				for (int y = 0; y < win; ++y)
					for (int x = 0; x < win; ++x) {
						const size_t idx = static_cast<size_t>(by + y) * w + (bx + x);
						meanA += a_a.px[idx];
						meanB += a_b.px[idx];
					}
				meanA /= n;
				meanB /= n;

				double varA = 0.0, varB = 0.0, covAB = 0.0;
				for (int y = 0; y < win; ++y)
					for (int x = 0; x < win; ++x) {
						const size_t idx = static_cast<size_t>(by + y) * w + (bx + x);
						const double da = a_a.px[idx] - meanA;
						const double db = a_b.px[idx] - meanB;
						varA += da * da;
						varB += db * db;
						covAB += da * db;
					}
				varA /= (n - 1);
				varB /= (n - 1);
				covAB /= (n - 1);

				const double numerator = (2 * meanA * meanB + kC1) * (2 * covAB + kC2);
				const double denominator = (meanA * meanA + meanB * meanB + kC1) * (varA + varB + kC2);
				sum += denominator > 0.0 ? (numerator / denominator) : 1.0;
				++windows;
			}
		}
		// win > w or win > h (a crop smaller than the window) — fall back to one whole-image window.
		if (windows == 0) {
			double       meanA = 0.0, meanB = 0.0;
			const size_t n = static_cast<size_t>(w) * h;
			for (size_t i = 0; i < n; ++i) {
				meanA += a_a.px[i];
				meanB += a_b.px[i];
			}
			meanA /= n;
			meanB /= n;
			double varA = 0.0, varB = 0.0, covAB = 0.0;
			for (size_t i = 0; i < n; ++i) {
				const double da = a_a.px[i] - meanA;
				const double db = a_b.px[i] - meanB;
				varA += da * da;
				varB += db * db;
				covAB += da * db;
			}
			varA /= (n - 1);
			varB /= (n - 1);
			covAB /= (n - 1);
			const double numerator = (2 * meanA * meanB + kC1) * (2 * covAB + kC2);
			const double denominator = (meanA * meanA + meanB * meanB + kC1) * (varA + varB + kC2);
			return denominator > 0.0 ? (numerator / denominator) : 1.0;
		}
		return sum / windows;
	}

	GoldenScore ScoreAgainstGolden(const fs::path& a_capturedPath, const fs::path& a_goldenPath, const json& a_goldenCfg)
	{
		GoldenScore result;
		result.threshold = a_goldenCfg.value("threshold", 0.98);

		if (!fs::exists(a_goldenPath)) {
			result.error = std::format("no golden at {}", a_goldenPath.generic_string());
			return result;
		}
		const auto candidate = DecodeGray(a_capturedPath);
		const auto golden = DecodeGray(a_goldenPath);
		if (!candidate || !golden) {
			result.error = "failed to decode candidate or golden image";
			return result;
		}
		if (candidate->width != golden->width || candidate->height != golden->height) {
			result.error = std::format("shape mismatch: candidate {}x{} vs golden {}x{}",
				candidate->width, candidate->height, golden->width, golden->height);
			return result;
		}

		const json regionCfgs = a_goldenCfg.value("regions", json::array());
		if (!regionCfgs.empty()) {
			double worst = 1.0;
			bool   allPassed = true;
			for (const auto& r : regionCfgs) {
				const GrayImage rc = CropUv(*candidate, r);
				const GrayImage rg = CropUv(*golden, r);
				const double    score = ComputeSsim(rc, rg);
				const double    threshold = r.value("threshold", result.threshold);
				const bool      passed = score >= threshold;
				result.regions.push_back(RegionScore{ r.value("name", std::string("?")), score, threshold, passed });
				worst = std::min(worst, score);
				allPassed = allPassed && passed;
			}
			result.ok = true;
			result.score = worst;
			result.passed = allPassed;
			return result;
		}

		result.ok = true;
		result.score = ComputeSsim(*candidate, *golden);
		result.passed = result.score >= result.threshold;
		return result;
	}
}
