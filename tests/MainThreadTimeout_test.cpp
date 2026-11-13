#include "MainThreadTimeout.h"
#include "test_framework.h"
#include <utility>

using dvb::MainThread::DescribeTaskTimeout;
using namespace std::chrono_literals;

TEST_CASE("Main-thread timeout: started work remains uncertain and must not be replayed")
{
	const auto text = DescribeTaskTimeout(5000ms, true, 0, 0);
	CHECK(text.find("did not finish within 5000ms") != std::string::npos);
	CHECK(text.find("already started and may still complete") != std::string::npos);
	CHECK(text.find("do not automatically replay") != std::string::npos);
	CHECK(text.find("abandoned") == std::string::npos);
	CHECK(text.find("restart") == std::string::npos);
}

TEST_CASE("Main-thread timeout: missing frame samples leave the cause unknown")
{
	for (const auto pair : { std::pair{ -1, 0 }, std::pair{ 0, -1 }, std::pair{ -1, -1 } }) {
		const auto text = DescribeTaskTimeout(250ms, false, pair.first, pair.second);
		CHECK(text.find("abandoned before entry") != std::string::npos);
		CHECK(text.find("frame progress unavailable") != std::string::npos);
		CHECK(text.find("cause and recovery are unknown") != std::string::npos);
	}
}

TEST_CASE("Main-thread timeout: cold startup with no frame advance does not prescribe restart")
{
	const auto text = DescribeTaskTimeout(5000ms, false, 0, 0);
	CHECK(text.find("abandoned before entry") != std::string::npos);
	CHECK(text.find("no frame advance observed during this wait") != std::string::npos);
	CHECK(text.find("loading/compilation") != std::string::npos);
	CHECK(text.find("check blockedBy or menu action=list") != std::string::npos);
	CHECK(text.find("does not establish a hang or a need to restart") != std::string::npos);
	CHECK(text.find("only a process restart") == std::string::npos);
}

TEST_CASE("Main-thread timeout: advancing counters are observations not readiness proof")
{
	const auto text = DescribeTaskTimeout(5000ms, false, 0, 358);
	CHECK(text.find("abandoned before entry") != std::string::npos);
	CHECK(text.find("changed from 0 to 358") != std::string::npos);
	CHECK(text.find("cause and readiness are not established") != std::string::npos);
}

TEST_CASE("Main-thread timeout: a decreasing counter is not an elapsed frame count")
{
	const auto text = DescribeTaskTimeout(5000ms, false, 2147483647, 0);
	CHECK(text.find("changed from 2147483647 to 0") != std::string::npos);
	CHECK(text.find("frames elapsed") == std::string::npos);
	CHECK(text.find("cause and readiness are not established") != std::string::npos);
}
