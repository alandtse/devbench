#include "ConsoleLogCapture.h"

#include "GameState.h"

#include <cstring>
#include <deque>
#include <string>

namespace dvb::ConsoleLogCapture
{
	namespace
	{
		// Main thread only - BeginCapture, SampleOnce and ReadFenced all run there.
		std::deque<std::string> g_ring;
		std::string             g_lastSeen;
		std::size_t             g_samples = 0;
		std::size_t             g_ticks = 0;
		std::size_t             g_engineFrames = 0;
		int                     g_lastFrame = -1;
		bool                    g_sawEndMarker = false;
		bool                    g_timedOut = false;

		std::string CurrentLine()
		{
			auto* cl = RE::ConsoleLog::GetSingleton();
			if (!cl)
				return {};
			return std::string(cl->lastMessage, ::strnlen(cl->lastMessage, sizeof(cl->lastMessage)));
		}
	}

	void BeginCapture()
	{
		g_ring.clear();
		// Seed with the line already showing, so the capture does not open by recording a
		// stale message as if it were this command's output.
		g_lastSeen = CurrentLine();
		g_samples = 0;
		g_ticks = 0;
		g_engineFrames = 0;
		g_lastFrame = -1;
		g_sawEndMarker = false;
		g_timedOut = false;
	}

	bool SampleOnce()
	{
		++g_ticks;
		const int f = game::CurrentFrame();
		if (f != g_lastFrame) {
			++g_engineFrames;
			g_lastFrame = f;
		}
		std::string now = CurrentLine();
		if (!now.empty() && now != g_lastSeen) {
			g_lastSeen = now;
			++g_samples;
			g_ring.push_back(now);
			while (g_ring.size() > kRingMax)
				g_ring.pop_front();
			if (now.find(kMarkerEnd) != std::string::npos)
				g_sawEndMarker = true;
		}
		return g_sawEndMarker;
	}

	void MarkTimedOut()
	{
		g_timedOut = true;
	}

	Result ReadFenced(size_t a_maxLines)
	{
		Result out;
		out.ringLines = g_ring.size();
		out.samples = g_samples;
		out.ticks = g_ticks;
		out.engineFrames = g_engineFrames;
		out.timedOut = g_timedOut;
		out.sameFrameRisk = g_timedOut || (g_ticks > 0 && g_engineFrames > 0 &&
		                                   g_samples > 0 && g_engineFrames < g_samples);

		auto* cl = RE::ConsoleLog::GetSingleton();
		if (!cl) {
			out.consoleLogNull = true;
			return out;
		}
		out.lastMessage.assign(cl->lastMessage, ::strnlen(cl->lastMessage, sizeof(cl->lastMessage)));
		out.lastMessageHasBegin = out.lastMessage.find(kMarkerBegin) != std::string::npos;
		const char* raw = cl->buffer.c_str();
		if (!raw || !*raw) {
			out.bufferEmpty = true;
		} else {
			const std::string buf(raw);
			out.bufferLen = buf.size();
			out.bufferHasBegin = buf.find(kMarkerBegin) != std::string::npos;
		}

		std::size_t b = g_ring.size();
		for (std::size_t i = g_ring.size(); i-- > 0;) {
			if (g_ring[i].find(kMarkerBegin) != std::string::npos) {
				b = i;
				break;
			}
		}
		if (b >= g_ring.size())
			return out;
		out.sawBegin = true;

		std::size_t e = g_ring.size();
		for (std::size_t i = b + 1; i < g_ring.size(); ++i) {
			if (g_ring[i].find(kMarkerEnd) != std::string::npos) {
				e = i;
				out.sawEnd = true;
				break;
			}
		}
		for (std::size_t i = b + 1; i < e; ++i) {
			if (!g_ring[i].empty())
				out.lines.push_back(g_ring[i]);
		}
		if (out.lines.size() > a_maxLines)
			out.lines.erase(out.lines.begin(), out.lines.end() - static_cast<std::ptrdiff_t>(a_maxLines));
		return out;
	}
}
