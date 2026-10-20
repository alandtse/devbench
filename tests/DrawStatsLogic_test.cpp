#include "test_framework.h"

#include "DrawStatsLogic.h"

using namespace dvb::DrawStatsLogic;
using dvb::json;

TEST_CASE("draws: pass names")
{
	CHECK(PassName(Pass::kMain) == "main");
	CHECK(PassName(Pass::kSun) == "sunShadows");
	CHECK(PassName(Pass::kSpot) == "spotShadows");
	CHECK(PassName(Pass::kPoint) == "pointShadows");
}

TEST_CASE("draws: caster size bins split at their edges")
{
	CHECK(CasterSizeBin(0.5f, 1000.0f) == 0);    // 0.0005
	CHECK(CasterSizeBin(1.0f, 1000.0f) == 1);    // exactly the first edge
	CHECK(CasterSizeBin(3.0f, 1000.0f) == 2);    // 0.003
	CHECK(CasterSizeBin(50.0f, 1000.0f) == 6);   // exactly the last edge
	CHECK(CasterSizeBin(500.0f, 1000.0f) == 6);  // far above
	CHECK(CasterSizeBin(0.0005f, 0.0f) == 0);    // a distance under 1 counts as 1
	CHECK(CasterSizeBin(1.0f, 0.0f) == kCasterSizeBins - 1);
}

TEST_CASE("draws: target labels")
{
	CHECK(TargetLabel({ 1920, 1080, 40, 1, true }, 1920, 1080) == "screen depth");
	CHECK(TargetLabel({ 1920, 1080, 10, 1, false }, 1920, 1080) == "screen");
	CHECK(TargetLabel({ 960, 540, 10, 1, false }, 1920, 1080) == "half screen");
	CHECK(TargetLabel({ 2048, 2048, 40, 1, true }, 1920, 1080) == "square map 2048 depth");
	CHECK(TargetLabel({ 640, 360, 10, 1, false }, 1920, 1080) == "640x360");
	CHECK(TargetLabel({ 1920, 1080, 10, 1, false }, 0, 0) == "1920x1080");
}

TEST_CASE("draws: per-frame averages")
{
	CHECK(PerFrame(10, 40, 3) == 10.0);
	CHECK(PerFrame(10, 40, 0) == 0.0);
	CHECK(PerFrame(40, 10, 3) == 0.0);
}

TEST_CASE("draws: a reading's shape and per-frame numbers")
{
	Counts a{};
	Counts b{};
	a.at = 1000;
	b.at = 3000;  // 2 s at 1000 ticks per second
	b.frames = 100;
	b.frameTicks = 1500;
	b.objects[0][0] = 5000;  // main pass, Lighting
	b.objects[1][1] = 2000;  // sun shadows, Utility
	b.renders[1] = 100;
	b.renderTicks[1] = 50;
	b.casters[1] = 900;
	b.casterSizes[1][0] = 300;
	b.cascadeObjects[0] = 1500;
	b.cascadeObjects[1] = 500;
	b.gpuFrames = 80;
	b.gpuFrameNs = 80 * 6'000'000ull;
	b.gpuNs[1] = 80 * 1'000'000ull;
	b.targetObjects[0][0] = 1000;
	b.targetObjects[1][0] = 4000;
	b.targetGpuNs[0] = 80 * 500'000ull;
	b.targetGpuNs[1] = 80 * 2'000'000ull;
	const std::vector<Target> targets{ { 1920, 1080, 10, 1, false }, { 1920, 1080, 40, 1, true } };

	const auto r = Reading(a, b, targets, false, 1920, 1080, true, 1000.0);
	CHECK(r["frames"] == 100);
	CHECK(r["seconds"] == 2.0);
	CHECK(r["fps"] == 50.0);
	CHECK(r["frameMs"] == 15.0);
	CHECK(r["gpu"]["timed"] == true);
	CHECK(r["gpu"]["framesTimed"] == 80);
	CHECK(r["gpu"]["frameMs"] == 6.0);

	CHECK(r["passes"].size() == kPassCount);
	const auto& main = r["passes"][0];
	CHECK(main["pass"] == "main");
	CHECK(main["objects"] == 50.0);
	CHECK(main["objectsByShader"]["Lighting"] == 50.0);
	CHECK(!main.contains("gpuMs"));
	const auto& sun = r["passes"][1];
	CHECK(sun["pass"] == "sunShadows");
	CHECK(sun["objects"] == 20.0);
	CHECK(sun["renders"] == 1.0);
	CHECK(sun["renderCpuMs"] == 0.5);
	CHECK(sun["gpuMs"] == 1.0);
	CHECK(sun["casters"] == 9.0);
	CHECK(sun["casterSizes"].size() == kCasterSizeBins);
	CHECK(sun["casterSizes"][0]["count"] == 3.0);
	CHECK(!sun["casterSizes"][0].contains("atLeast"));
	CHECK(sun["objectsPerDepthTarget"] == dvb::json::array({ 15.0, 5.0 }));
	CHECK(!r["passes"][2].contains("objectsPerDepthTarget"));

	const auto& t = r["mainPassTargets"];
	CHECK(t.size() == 2);
	CHECK(t[0]["label"] == "screen depth");  // the higher GPU time first
	CHECK(t[0]["gpuMs"] == 2.0);
	CHECK(t[0]["objects"] == 40.0);
	CHECK(t[1]["label"] == "screen");
	CHECK(r["mainPassTargetsCapped"] == false);
}

TEST_CASE("draws: an empty window reads zeros, never a division by zero")
{
	const Counts c{};
	const auto   r = Reading(c, c, {}, false, 0, 0, false, 1000.0);
	CHECK(r["frames"] == 0);
	CHECK(r["fps"] == 0.0);
	CHECK(r["frameMs"] == 0.0);
	CHECK(r["gpu"]["frameMs"] == 0.0);
	CHECK(r["mainPassTargets"].empty());
}

TEST_CASE("draws: every reply names its source")
{
	const Counts c{};
	const auto   own = WithSource(Reading(c, c, {}, false, 0, 0, false, 1000.0), kOwnSource);
	for (const auto* key : { "frames", "seconds", "fps", "frameMs", "gpu", "passes", "mainPassTargets", "mainPassTargetsCapped", "source" })
		CHECK(own.contains(key));
	CHECK(own["source"] == "devbench");

	CHECK(WithSource(json{ { "frames", 3 }, { "source", "spoofed" } }, "Open Shaders")["source"] == "Open Shaders");
	CHECK(WithSource(json{ { "frames", 3 } }, "Open Shaders")["frames"] == 3);
	const auto wrapped = WithSource(json("not an object"), "Open Shaders");
	CHECK(wrapped["source"] == "Open Shaders");
	CHECK(wrapped["reading"] == "not an object");
}

TEST_CASE("draws: a provider is named by its descriptor")
{
	CHECK(ProviderSource(json{ { "description", "draws" }, { "source", "Open Shaders" } }) == "Open Shaders");
	CHECK(ProviderSource(json{ { "description", "draws" } }) == "extension");
	CHECK(ProviderSource(json{ { "source", "" } }) == "extension");
	CHECK(ProviderSource(json{ { "source", 3 } }) == "extension");
	CHECK(ProviderSource(json::array()) == "extension");
}

TEST_CASE("draws: a full target list is capped only once a target was refused")
{
	const Counts              c{};
	const std::vector<Target> full(kMaxTargets, Target{ 1920, 1080, 10, 1, false });
	CHECK(Reading(c, c, full, false, 1920, 1080, false, 1000.0)["mainPassTargetsCapped"] == false);
	CHECK(Reading(c, c, full, true, 1920, 1080, false, 1000.0)["mainPassTargetsCapped"] == true);
	CHECK(Reading(c, c, full, false, 1920, 1080, false, 1000.0)["mainPassTargets"].size() == kMaxTargets);
}
