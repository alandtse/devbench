#include "test_framework.h"

#include "ModalLogic.h"
#include "Notices.h"
#include "ToolRegistry.h"

using dvb::json;
using dvb::Notices::Severity;

TEST_CASE("A posted notice replaces the same key and sorts most severe first")
{
	dvb::Notices::Board board;
	board.Post("a", "p", Severity::kInfo, "first", 0, 0);
	board.Post("b", "p", Severity::kCritical, "bad", 0, 0);
	board.Post("a", "p", Severity::kWarning, "second", 0, 0);
	const auto active = board.Active(1);
	CHECK(active.size() == 2);
	CHECK(active[0].key == "b");
	CHECK(active[1].text == "second");
}

TEST_CASE("A notice expires after its ttl and can be cleared")
{
	dvb::Notices::Board board;
	board.Post("t", "p", Severity::kInfo, "x", 100, 1000);
	CHECK(board.Active(1099).size() == 1);
	CHECK(board.Active(1100).empty());
	board.Post("c", "p", Severity::kInfo, "y", 0, 0);
	CHECK(board.Clear("c"));
	CHECK(!board.Clear("c"));
}

TEST_CASE("The board stays at its cap by dropping the oldest notice")
{
	dvb::Notices::Board board;
	for (std::size_t i = 0; i < dvb::Notices::kMaxNotices + 5; ++i)
		board.Post("k" + std::to_string(i), "p", Severity::kInfo, "x", 0, 0);
	const auto active = board.Active(0);
	CHECK(active.size() == dvb::Notices::kMaxNotices);
	CHECK(active.front().key == "k5");
}

TEST_CASE("A key past the byte cap clears with the same long key")
{
	dvb::Notices::Board board;
	const std::string   longKey(dvb::Notices::kMaxKeyBytes + 1, 'k');
	board.Post(longKey, "p", Severity::kInfo, "x", 0, 0);
	CHECK(board.Clear(longKey));
	CHECK(board.Active(0).empty());
}

TEST_CASE("ParseSeverity rejects an unknown name")
{
	Severity s = Severity::kWarning;
	CHECK(!dvb::Notices::ParseSeverity("fatal", s));
	CHECK(s == Severity::kWarning);
	CHECK(dvb::Notices::ParseSeverity("critical", s));
	CHECK(s == Severity::kCritical);
}

TEST_CASE("Only non-benign menus block, each with a hint")
{
	const auto blockers = dvb::Modal::Blockers({ "HUD Menu", "Tutorial Menu", "MessageBoxMenu", "Cursor Menu" });
	CHECK(blockers.size() == 2);
	CHECK(blockers[0].name == "MessageBoxMenu");
	CHECK(blockers[0].hint.find("accept") != std::string::npos);
	CHECK(blockers[1].hint == "menu action=close name=\"Tutorial Menu\"");
	CHECK(dvb::Modal::Blockers({ "HUD Menu", "Mist Menu", "LootMenu" }).empty());
}

TEST_CASE("A message box is never auto-dismissed")
{
	CHECK(dvb::Modal::CanAutoDismiss("Tutorial Menu"));
	CHECK(!dvb::Modal::CanAutoDismiss("MessageBoxMenu"));
	CHECK(!dvb::Modal::CanAutoDismiss("HUD Menu"));
	CHECK(!dvb::Modal::CanAutoDismiss(""));
}

namespace
{
	void Fill(dvb::ToolRegistry& r)
	{
		r.Register({ "ok", "d" }, [](const json&, const dvb::ToolContext&) { return json{ { "x", 1 } }; });
		r.Register({ "bad", "d" }, [](const json&, const dvb::ToolContext&) -> json { throw dvb::ToolError(404, "nope"); });
		r.SetAnnotator([] { return dvb::ToolRegistry::Annotation{ json{ { "blockedBy", json::array() }, { "x", 2 } }, "blocked" }; });
	}
}

TEST_CASE("The annotator adds fields to a success without overwriting the tool's own")
{
	dvb::ToolRegistry reg;
	Fill(reg);
	const auto r = reg.Invoke("ok", json::object(), {});
	CHECK(r.ok);
	CHECK(r.value["x"] == 1);
	CHECK(r.value.contains("blockedBy"));
}

TEST_CASE("The annotator appends its summary to a failure")
{
	dvb::ToolRegistry reg;
	Fill(reg);
	const auto r = reg.Invoke("bad", json::object(), {});
	CHECK(!r.ok);
	CHECK(r.errorMessage == "nope [blocked]");
}

TEST_CASE("A non-boolean noNotices is a 400")
{
	dvb::ToolRegistry reg;
	Fill(reg);
	const auto r = reg.Invoke("ok", json{ { "noNotices", "yes" } }, {});
	CHECK(!r.ok);
	CHECK(r.errorCode == 400);
}

TEST_CASE("noNotices and internal calls skip the annotator")
{
	dvb::ToolRegistry reg;
	Fill(reg);
	CHECK(!reg.Invoke("ok", json{ { "noNotices", true } }, {}).value.contains("blockedBy"));
	CHECK(!reg.Invoke("ok", json::object(), dvb::ToolContext{ "", true }).value.contains("blockedBy"));
	CHECK(reg.Invoke("ok", json{ { "noNotices", false } }, {}).value.contains("blockedBy"));
}
