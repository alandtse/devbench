#include "ConsoleLogCapture.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>

namespace
{
	std::mutex g_mutex;
	std::deque<dvb::ConsoleLogCapture::Line> g_lines;
	std::atomic<uint64_t> g_seq{ 0 };
	std::atomic<bool> g_capturing{ false };  // gate: detour is a near-no-op unless true
	constexpr size_t kMaxLines = 256;        // bounded; only between-marker lines land here

	// Fence state. Touched only on the main/console thread inside the deferred drain,
	// so a plain bool is fine; the begin marker arms recording, the end marker disarms.
	bool g_between = false;
	bool g_sawBegin = false;
	bool g_sawEnd = false;

	void Append(const char* a_text, size_t a_len)
	{
		while (a_len > 0 && (a_text[a_len - 1] == '\n' || a_text[a_len - 1] == '\r'))
			--a_len;
		if (a_len == 0)
			return;
		const uint64_t s = g_seq.fetch_add(1, std::memory_order_relaxed);
		std::lock_guard<std::mutex> lock(g_mutex);
		g_lines.push_back({ s, 0u, std::string(a_text, a_len) });
		while (g_lines.size() > kMaxLines)
			g_lines.pop_front();
	}

	// Detour on RE::ConsoleLog::VPrint. When no window is open this is a single relaxed
	// atomic load then a tail-forward. While capturing, it records ONLY the lines
	// between the begin and end markers — the surrounding spam is never buffered.
	struct VPrintHook
	{
		static void thunk(RE::ConsoleLog* a_self, const char* a_fmt, std::va_list a_args)
		{
			if (g_capturing.load(std::memory_order_relaxed) && a_fmt) {
				std::va_list copy;
				va_copy(copy, a_args);  // vsnprintf and the original each consume the list
				char buf[1024];
				const int n = std::vsnprintf(buf, sizeof(buf), a_fmt, copy);
				va_end(copy);
				if (n > 0) {
					const size_t len = (n < static_cast<int>(sizeof(buf))) ? static_cast<size_t>(n) : sizeof(buf) - 1;
					if (std::strstr(buf, dvb::ConsoleLogCapture::kMarkerBegin)) {
						// Fence opens: discard anything seen so far this window, start recording.
						g_sawBegin = true;
						g_between = true;
						{
							std::lock_guard<std::mutex> lock(g_mutex);
							g_lines.clear();
						}
					} else if (std::strstr(buf, dvb::ConsoleLogCapture::kMarkerEnd)) {
						g_sawEnd = true;
						g_between = false;
					} else if (g_between) {
						Append(buf, len);
					}
				}
			}
			func(a_self, a_fmt, a_args);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
}

namespace dvb::ConsoleLogCapture
{
	void Install()
	{
		static std::once_flag once;
		std::call_once(once, []() {
			// Same address CommonLibSSE-NG resolves for ConsoleLog::VPrint; the
			// RelocationID covers SE/AE and maps to VR via the address library.
			stl::detour_thunk<VPrintHook>(REL::RelocationID(50180, 51110));
			logs::info("ConsoleLogCapture: hooked ConsoleLog::VPrint (gated, fenced)");
		});
	}

	void BeginCapture()
	{
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			g_lines.clear();
		}
		g_between = false;
		g_sawBegin = false;
		g_sawEnd = false;
		g_capturing.store(true, std::memory_order_relaxed);
	}

	void EndCapture()
	{
		g_capturing.store(false, std::memory_order_relaxed);
		g_between = false;
	}

	bool IsCapturing() { return g_capturing.load(std::memory_order_relaxed); }
	bool SawBegin() { return g_sawBegin; }
	bool SawEnd() { return g_sawEnd; }
	uint64_t HeadSeq() { return g_seq.load(std::memory_order_relaxed); }

	std::vector<Line> Snapshot(size_t a_maxLines)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		const size_t total = g_lines.size();
		const size_t start = total > a_maxLines ? total - a_maxLines : 0;
		std::vector<Line> out;
		out.reserve(total - start);
		for (size_t i = start; i < total; ++i)
			out.push_back(g_lines[i]);
		return out;
	}
}
