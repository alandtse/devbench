// Game text (form names, cell/location names) is the game's 8-bit code page, not UTF-8.
// A raw 'Rán' made dump() throw type_error.316, so the whole HTTP reply was an empty 500.

#include "test_framework.h"

#include "Json.h"
#include "McpContent.h"

using dvb::json;

TEST_CASE("Dump re-reads a Windows-1252 name as UTF-8")
{
	const json reply = { { "name", "Shrine of R\xE1n" }, { "refs", json::array({ { { "name", "Hr\xF6nn" } } }) } };
	CHECK_THROWS(reply.dump());
	CHECK(dvb::Dump(reply) == "{\"name\":\"Shrine of R\xC3\xA1n\",\"refs\":[{\"name\":\"Hr\xC3\xB6nn\"}]}");
}

TEST_CASE("Dump leaves valid UTF-8 and ASCII untouched")
{
	const json reply = { { "name", "R\xC3\xA1n" }, { "id", 42 } };
	CHECK(dvb::Dump(reply) == reply.dump());
	CHECK(dvb::Dump(reply, 2) == reply.dump(2));
}

TEST_CASE("Utf8Text maps the 0x80-0x9F block, not Latin-1")
{
	CHECK(dvb::Utf8Text("\x80\x99\x9F") == "\xE2\x82\xAC\xE2\x84\xA2\xC5\xB8");  // € ™ Ÿ
	CHECK(dvb::Utf8Text("\x81") == "\xC2\x81");                                  // a hole keeps its C1 code point
}

TEST_CASE("MCP content of a Windows-1252 payload serializes")
{
	const json obj = dvb::ToContentBlocks(json{ { "name", "R\xE1n" } });
	CHECK_NOTHROW(obj.dump());
	const json str = dvb::ToContentBlocks(json("R\xE1n"));
	CHECK(str[0]["text"] == "R\xC3\xA1n");
}
