#include "test_framework.h"

#include "PluginHeaderLogic.h"

#include <cstring>
#include <string>
#include <vector>

using namespace dvb::PluginHeaderLogic;

namespace
{
	void AppendSub(std::vector<std::uint8_t>& a_data, const char* a_type, const void* a_payload, std::size_t a_size)
	{
		a_data.insert(a_data.end(), a_type, a_type + 4);
		a_data.push_back(static_cast<std::uint8_t>(a_size & 0xFF));
		a_data.push_back(static_cast<std::uint8_t>(a_size >> 8));
		const auto* p = static_cast<const std::uint8_t*>(a_payload);
		a_data.insert(a_data.end(), p, p + a_size);
	}

	void AppendText(std::vector<std::uint8_t>& a_data, const char* a_type, const std::string& a_text)
	{
		AppendSub(a_data, a_type, a_text.c_str(), a_text.size() + 1);
	}

	std::vector<std::uint8_t> MakeTes4(std::uint32_t a_flags)
	{
		std::vector<std::uint8_t> data;
		const float               hedr[3]{ 1.7f, 0.0f, 0.0f };
		std::uint8_t              hedrBytes[12];
		std::memcpy(hedrBytes, hedr, sizeof(hedr));
		const std::uint32_t records = 1234;
		std::memcpy(hedrBytes + 4, &records, sizeof(records));
		AppendSub(data, "HEDR", hedrBytes, sizeof(hedrBytes));
		AppendText(data, "CNAM", "Dint999");
		AppendText(data, "SNAM", "Xing SMP wigs");
		AppendText(data, "MAST", "Skyrim.esm");
		AppendText(data, "MAST", "Update.esm");

		std::vector<std::uint8_t> bytes(24, 0);
		std::memcpy(bytes.data(), "TES4", 4);
		const auto size = static_cast<std::uint32_t>(data.size());
		std::memcpy(bytes.data() + 4, &size, sizeof(size));
		std::memcpy(bytes.data() + 8, &a_flags, sizeof(a_flags));
		bytes.insert(bytes.end(), data.begin(), data.end());
		return bytes;
	}
}

TEST_CASE("a plugin header yields its author, description, masters and counts")
{
	const auto header = Parse(MakeTes4(0));
	CHECK(header.has_value());
	if (!header)
		return;
	CHECK(header->author == "Dint999");
	CHECK(header->description == "Xing SMP wigs");
	CHECK(header->masters.size() == 2);
	CHECK(header->masters[1] == "Update.esm");
	CHECK(header->version == 1.7f);
	CHECK(header->recordCount == 1234);
	CHECK(!header->master);
	CHECK(!header->light);
}

TEST_CASE("the master and light flags are read")
{
	const auto both = Parse(MakeTes4(0x201));
	CHECK(both.has_value());
	if (!both)
		return;
	CHECK(both->master);
	CHECK(both->light);
}

TEST_CASE("bytes that do not start with a TES4 record, or are cut short, are refused")
{
	CHECK(!Parse({}).has_value());
	std::vector<std::uint8_t> notTes4 = MakeTes4(0);
	notTes4[0] = 'X';
	CHECK(!Parse(notTes4).has_value());
	const auto whole = MakeTes4(0);
	CHECK(!Parse(std::span<const std::uint8_t>(whole.data(), whole.size() - 1)).has_value());
	CHECK(!Parse(std::span<const std::uint8_t>(whole.data(), 10)).has_value());
}

TEST_CASE("the record size is the header plus its data, within a limit")
{
	const auto whole = MakeTes4(0);
	CHECK(RecordSize(whole, 1 << 20) == whole.size());
	CHECK(!RecordSize(whole, 10).has_value());
	CHECK(!RecordSize(std::span<const std::uint8_t>(whole.data(), 10), 1 << 20).has_value());
}
