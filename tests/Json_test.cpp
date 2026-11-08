// Game text (form names, cell/location names) is the game's 8-bit code page, not UTF-8.
// A raw 'Rán' made dump() throw type_error.316, so the whole HTTP reply was an empty 500.

#include "test_framework.h"

#include "GameText.h"
#include "Json.h"
#include "McpContent.h"

using dvb::json;

namespace
{
	constexpr UINT kWin1252 = 1252;
}

TEST_CASE("FromGame decodes by code page, not by guessing")
{
	CHECK(dvb::FromGame("Shrine of R\xE1n", kWin1252) == "Shrine of R\xC3\xA1n");
	CHECK(dvb::FromGame("\x80\x99\x9F", kWin1252) == "\xE2\x82\xAC\xE2\x84\xA2\xC5\xB8");  // € ™ Ÿ
	// bytes that happen to form UTF-8 are still two Windows-1252 characters
	CHECK(dvb::FromGame("\xC3\xA9\xE9", kWin1252) == "\xC3\x83\xC2\xA9\xC3\xA9");
	CHECK(dvb::FromGame("plain ascii", kWin1252) == "plain ascii");
	CHECK(dvb::FromGame(static_cast<const char*>(nullptr), kWin1252).empty());
}

TEST_CASE("ToGame is the inverse for characters the code page has")
{
	CHECK(dvb::ToGame("Shrine of R\xC3\xA1n", kWin1252) == "Shrine of R\xE1n");
	CHECK(dvb::ToGame("\xE2\x82\xAC", kWin1252) == "\x80");
	CHECK(dvb::ToGame(dvb::FromGame("Hr\xF6nn \x99", kWin1252), kWin1252) == "Hr\xF6nn \x99");
	CHECK(dvb::ToGame("\xE6\x97\xA5", kWin1252) == "?");  // outside the code page
}

TEST_CASE("Dump replaces invalid UTF-8 instead of throwing")
{
	const json reply = { { "name", "Shrine of R\xE1n" }, { "refs", json::array({ { { "name", "Hr\xF6nn" } } }) } };
	CHECK_THROWS(reply.dump());
	CHECK(dvb::Dump(reply) ==
		  "{\"name\":\"Shrine of R\xEF\xBF\xBD"
		  "n\",\"refs\":[{\"name\":\"Hr\xEF\xBF\xBD"
		  "nn\"}]}");
}

TEST_CASE("Dump leaves valid UTF-8 and ASCII untouched")
{
	const json reply = { { "name", "R\xC3\xA1n" }, { "id", 42 } };
	CHECK(dvb::Dump(reply) == reply.dump());
	CHECK(dvb::Dump(reply, 2) == reply.dump(2));
}

TEST_CASE("MCP content of invalid UTF-8 serializes")
{
	const json obj = dvb::ToContentBlocks(json{ { "name", "R\xE1n" } });
	CHECK_NOTHROW(obj.dump());
	const json str = dvb::ToContentBlocks(json("R\xE1n"));
	CHECK_NOTHROW(str.dump());
	CHECK(str[0]["text"] ==
		  "R\xEF\xBF\xBD"
		  "n");
}

TEST_CASE("Utf8Prefix never cuts a multibyte character")
{
	const std::string text = "ab\xC3\xA1\xE2\x82\xAC";  // a, b, U+00E1 (2 bytes), U+20AC (3 bytes)
	CHECK(dvb::Utf8Prefix(text, 100) == text);
	CHECK(dvb::Utf8Prefix(text, 4) == "ab\xC3\xA1");
	CHECK(dvb::Utf8Prefix(text, 3) == "ab");
	CHECK(dvb::Utf8Prefix(text, 5) == "ab\xC3\xA1");
	CHECK(dvb::Utf8Prefix(text, 2) == "ab");
}

TEST_CASE("Utf8Prefix never cuts a multibyte character")
{
	const std::string text = "ab\xC3\xA1\xE2\x82\xAC";  // a, b, U+00E1 (2 bytes), U+20AC (3 bytes)
	CHECK(dvb::Utf8Prefix(text, 100) == text);
	CHECK(dvb::Utf8Prefix(text, 4) == "ab\xC3\xA1");
	CHECK(dvb::Utf8Prefix(text, 3) == "ab");
	CHECK(dvb::Utf8Prefix(text, 5) == "ab\xC3\xA1");
	CHECK(dvb::Utf8Prefix(text, 2) == "ab");
}
