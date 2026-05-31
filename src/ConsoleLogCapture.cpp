#include "ConsoleLogCapture.h"

#include <cstring>

namespace dvb::ConsoleLogCapture
{
	Result ReadFenced(size_t a_maxLines)
	{
		Result out;
		auto* cl = RE::ConsoleLog::GetSingleton();
		if (!cl)
			return out;
		const char* raw = cl->buffer.c_str();  // ConsoleLog::buffer — the accumulated scrollback
		if (!raw || !*raw)
			return out;
		const std::string buf(raw);

		// Most recent window: the LAST begin marker, then the first end marker after it.
		const size_t b = buf.rfind(kMarkerBegin);
		if (b == std::string::npos)
			return out;
		out.sawBegin = true;
		const size_t e = buf.find(kMarkerEnd, b + std::strlen(kMarkerBegin));

		// Output is between the end of the begin-marker line and the start of the end-marker
		// line (both marker lines excluded).
		size_t start = buf.find('\n', b);
		start = (start == std::string::npos) ? buf.size() : start + 1;
		size_t stop;
		if (e == std::string::npos) {
			stop = buf.size();  // end marker not in the buffer yet (still draining / evicted)
		} else {
			out.sawEnd = true;
			const size_t ls = buf.rfind('\n', e);  // start of the end-marker line
			stop = (ls == std::string::npos || ls < start) ? start : ls;
		}

		// Split [start, stop) into trimmed, non-empty lines.
		const std::string region = buf.substr(start, stop - start);
		std::string line;
		auto flush = [&]() {
			if (!line.empty()) {
				out.lines.push_back(line);
				line.clear();
			}
		};
		for (const char c : region) {
			if (c == '\n' || c == '\r')
				flush();
			else
				line += c;
		}
		flush();

		if (out.lines.size() > a_maxLines)
			out.lines.erase(out.lines.begin(), out.lines.end() - static_cast<std::ptrdiff_t>(a_maxLines));
		return out;
	}
}
