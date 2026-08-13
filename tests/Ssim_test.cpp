// Host-independent coverage for the native SSIM comparison pipeline (Ssim.h/.cpp): decode,
// crop, windowed SSIM, and golden scoring. No game/provider involved — every image here is a
// small synthetic BMP written to a temp dir by this file, decoded back through the same
// stb_image path a real capture goes through.

#include "test_framework.h"

#include "Ssim.h"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>

using dvb::json;
using dvb::Ssim::ComputeSsim;
using dvb::Ssim::CropUv;
using dvb::Ssim::DecodeGray;
using dvb::Ssim::GrayImage;
using dvb::Ssim::ScoreAgainstGolden;

namespace
{
	namespace fs = std::filesystem;

	fs::path TestDir()
	{
		const fs::path  dir = fs::temp_directory_path() / "devbench_ssim_tests";
		std::error_code ec;
		fs::create_directories(dir, ec);
		return dir;
	}

	// Minimal 24bpp, uncompressed BMP writer — just enough to round-trip through stb_image's
	// BMP decoder, which is all these tests need (no PNG encoder is available in-tree; stb_image
	// only decodes).
	void WriteBmp24(const fs::path& a_path, int a_w, int a_h, const std::function<uint8_t(int, int)>& a_px)
	{
		const int            rowSize = ((a_w * 3 + 3) / 4) * 4;
		const uint32_t       pixelBytes = static_cast<uint32_t>(rowSize) * a_h;
		const uint32_t       fileSize = 14 + 40 + pixelBytes;
		std::vector<uint8_t> buf;
		buf.reserve(fileSize);

		auto put16 = [&](uint16_t v) { buf.push_back(v & 0xFF); buf.push_back((v >> 8) & 0xFF); };
		auto put32 = [&](uint32_t v) {
			buf.push_back(v & 0xFF);
			buf.push_back((v >> 8) & 0xFF);
			buf.push_back((v >> 16) & 0xFF);
			buf.push_back((v >> 24) & 0xFF);
		};

		buf.push_back('B');
		buf.push_back('M');
		put32(fileSize);
		put32(0);
		put32(54);

		put32(40);
		put32(static_cast<uint32_t>(a_w));
		put32(static_cast<uint32_t>(a_h));
		put16(1);
		put16(24);
		put32(0);
		put32(pixelBytes);
		put32(0);
		put32(0);
		put32(0);
		put32(0);

		for (int y = a_h - 1; y >= 0; --y) {
			int written = 0;
			for (int x = 0; x < a_w; ++x) {
				const uint8_t v = a_px(x, y);
				buf.push_back(v);
				buf.push_back(v);
				buf.push_back(v);
				written += 3;
			}
			while (written < rowSize) {
				buf.push_back(0);
				++written;
			}
		}

		std::ofstream out(a_path, std::ios::binary | std::ios::trunc);
		out.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
	}

	uint8_t Checker(int a_x, int a_y, int a_cell)
	{
		return ((a_x / a_cell) + (a_y / a_cell)) % 2 == 0 ? 220 : 30;
	}

	void WriteChecker(const fs::path& a_path, int a_w, int a_h, int a_cell, bool a_invert = false)
	{
		WriteBmp24(a_path, a_w, a_h, [&](int x, int y) {
			const uint8_t v = Checker(x, y, a_cell);
			return a_invert ? static_cast<uint8_t>(255 - v) : v;
		});
	}

	void WriteSolid(const fs::path& a_path, int a_w, int a_h, uint8_t a_value)
	{
		WriteBmp24(a_path, a_w, a_h, [&](int, int) { return a_value; });
	}
}

TEST_CASE("DecodeGray returns nullopt for a missing file")
{
	CHECK(!DecodeGray(TestDir() / "does_not_exist.bmp").has_value());
}

TEST_CASE("DecodeGray round-trips dimensions and pixel values")
{
	const fs::path path = TestDir() / "checker_small.bmp";
	WriteChecker(path, 16, 16, 4);

	const auto img = DecodeGray(path);
	CHECK(img.has_value());
	CHECK(img->width == 16);
	CHECK(img->height == 16);
	CHECK(std::abs(img->px[0] - 220.0f) < 1.0f);  // (0,0) is an "on" checker cell
}

TEST_CASE("identical images score SSIM close to 1.0")
{
	const fs::path a = TestDir() / "id_a.bmp";
	const fs::path b = TestDir() / "id_b.bmp";
	WriteChecker(a, 24, 24, 4);
	WriteChecker(b, 24, 24, 4);

	const auto ia = DecodeGray(a);
	const auto ib = DecodeGray(b);
	CHECK(ia.has_value() && ib.has_value());
	CHECK(std::abs(ComputeSsim(*ia, *ib) - 1.0) < 0.01);
}

TEST_CASE("fully value-inverted images score strongly negative")
{
	const fs::path a = TestDir() / "inv_a.bmp";
	const fs::path b = TestDir() / "inv_b.bmp";
	WriteChecker(a, 24, 24, 4, false);
	WriteChecker(b, 24, 24, 4, true);

	const auto ia = DecodeGray(a);
	const auto ib = DecodeGray(b);
	CHECK(ia.has_value() && ib.has_value());
	// Regression guard for the non-overlapping-block bug: a naive block-aligned SSIM scored
	// this exact case 0.54 (near-flat blocks blind the contrast term to sign inversion).
	CHECK(ComputeSsim(*ia, *ib) < -0.5);
}

TEST_CASE("a partial difference scores strictly between identical and fully inverted")
{
	const fs::path a = TestDir() / "part_a.bmp";
	const fs::path b = TestDir() / "part_b.bmp";
	WriteChecker(a, 24, 24, 4, false);
	// Right half inverted, left half untouched.
	WriteBmp24(b, 24, 24, [](int x, int y) {
		const uint8_t v = Checker(x, y, 4);
		return x >= 12 ? static_cast<uint8_t>(255 - v) : v;
	});

	const auto ia = DecodeGray(a);
	const auto ib = DecodeGray(b);
	CHECK(ia.has_value() && ib.has_value());
	const double score = ComputeSsim(*ia, *ib);
	CHECK(score < 1.0);
	CHECK(score > -0.5);
}

TEST_CASE("flat single-color images compare identical without a div-by-zero blowup")
{
	const fs::path a = TestDir() / "flat_a.bmp";
	const fs::path b = TestDir() / "flat_b.bmp";
	WriteSolid(a, 16, 16, 128);
	WriteSolid(b, 16, 16, 128);

	const auto ia = DecodeGray(a);
	const auto ib = DecodeGray(b);
	CHECK(ia.has_value() && ib.has_value());
	CHECK(std::abs(ComputeSsim(*ia, *ib) - 1.0) < 0.01);
}

TEST_CASE("an image with height 1 defensively returns 0.0")
{
	const fs::path a = TestDir() / "thin_a.bmp";
	const fs::path b = TestDir() / "thin_b.bmp";
	WriteSolid(a, 16, 1, 128);
	WriteSolid(b, 16, 1, 128);

	const auto ia = DecodeGray(a);
	const auto ib = DecodeGray(b);
	CHECK(ia.has_value() && ib.has_value());
	// win = min(8, w, h) = 1 -> the "win <= 1" guard fires regardless of content.
	CHECK(ComputeSsim(*ia, *ib) == 0.0);
}

TEST_CASE("an image smaller than the SSIM window still scores via the whole-image fallback")
{
	const fs::path a = TestDir() / "tiny_a.bmp";
	const fs::path b = TestDir() / "tiny_b.bmp";
	WriteChecker(a, 4, 4, 2);
	WriteChecker(b, 4, 4, 2);

	const auto ia = DecodeGray(a);
	const auto ib = DecodeGray(b);
	CHECK(ia.has_value() && ib.has_value());
	CHECK(std::abs(ComputeSsim(*ia, *ib) - 1.0) < 0.01);
}

TEST_CASE("CropUv extracts the requested UV rect")
{
	const fs::path path = TestDir() / "crop_src.bmp";
	WriteBmp24(path, 10, 10, [](int x, int y) { return static_cast<uint8_t>(x + y); });
	const auto img = DecodeGray(path);
	CHECK(img.has_value());

	const GrayImage right = CropUv(*img, json{ { "x", 0.5 }, { "y", 0.0 }, { "w", 0.5 }, { "h", 1.0 } });
	CHECK(right.width == 5);
	CHECK(right.height == 10);
	// (x=5,y=0) in the source maps to (0,0) in the crop.
	CHECK(std::abs(right.px[0] - 5.0f) < 0.5f);
}

TEST_CASE("ScoreAgainstGolden reports an error when the golden file is missing")
{
	const fs::path a = TestDir() / "golden_missing_candidate.bmp";
	WriteSolid(a, 8, 8, 100);

	const auto result = ScoreAgainstGolden(a, TestDir() / "no_such_golden.bmp", json::object());
	CHECK(!result.ok);
	CHECK(!result.error.empty());
}

TEST_CASE("ScoreAgainstGolden reports an error on a candidate/golden shape mismatch")
{
	const fs::path candidate = TestDir() / "shape_candidate.bmp";
	const fs::path golden = TestDir() / "shape_golden.bmp";
	WriteSolid(candidate, 8, 8, 100);
	WriteSolid(golden, 16, 16, 100);

	const auto result = ScoreAgainstGolden(candidate, golden, json::object());
	CHECK(!result.ok);
	CHECK(!result.error.empty());
}

TEST_CASE("ScoreAgainstGolden passes an identical capture against the default threshold")
{
	const fs::path candidate = TestDir() / "pass_candidate.bmp";
	const fs::path golden = TestDir() / "pass_golden.bmp";
	WriteChecker(candidate, 24, 24, 4);
	WriteChecker(golden, 24, 24, 4);

	const auto result = ScoreAgainstGolden(candidate, golden, json::object());
	CHECK(result.ok);
	CHECK(result.error.empty());
	CHECK(result.passed);
	CHECK(std::abs(result.threshold - 0.98) < 1e-9);
	CHECK(result.regions.empty());
}

TEST_CASE("ScoreAgainstGolden fails a mismatched capture against a strict threshold")
{
	const fs::path candidate = TestDir() / "fail_candidate.bmp";
	const fs::path golden = TestDir() / "fail_golden.bmp";
	WriteChecker(candidate, 24, 24, 4, false);
	WriteChecker(golden, 24, 24, 4, true);

	const auto result = ScoreAgainstGolden(candidate, golden, json{ { "threshold", 0.98 } });
	CHECK(result.ok);
	CHECK(!result.passed);
	CHECK(result.score < 0.98);
}

TEST_CASE("ScoreAgainstGolden with regions scores each region and propagates the worst")
{
	const fs::path candidate = TestDir() / "region_candidate.bmp";
	const fs::path golden = TestDir() / "region_golden.bmp";
	WriteChecker(golden, 24, 24, 4, false);
	// Left half matches the golden; right half is inverted.
	WriteBmp24(candidate, 24, 24, [](int x, int y) {
		const uint8_t v = Checker(x, y, 4);
		return x >= 12 ? static_cast<uint8_t>(255 - v) : v;
	});

	const json cfg{
		{ "threshold", 0.98 },
		{ "regions", json::array({ json{ { "name", "left" }, { "x", 0.0 }, { "y", 0.0 }, { "w", 0.5 }, { "h", 1.0 } },
						 json{ { "name", "right" }, { "x", 0.5 }, { "y", 0.0 }, { "w", 0.5 }, { "h", 1.0 }, { "threshold", -1.0 } } }) }
	};
	const auto result = ScoreAgainstGolden(candidate, golden, cfg);
	CHECK(result.ok);
	CHECK(result.regions.size() == 2);

	const auto& left = result.regions[0];
	const auto& right = result.regions[1];
	CHECK(left.name == "left");
	CHECK(left.passed);
	CHECK(std::abs(left.score - 1.0) < 0.05);

	CHECK(right.name == "right");
	CHECK(right.threshold == -1.0);
	CHECK(right.passed);  // loose per-region threshold overrides the top-level one

	// Overall verdict is the worst region, and passes only because the loose region threshold let it.
	CHECK(result.passed);
	CHECK(std::abs(result.score - right.score) < 1e-9);
}
