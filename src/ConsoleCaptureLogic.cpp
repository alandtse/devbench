#include "ConsoleCaptureLogic.h"

namespace dvb::ConsoleLogCapture
{
	namespace
	{
		bool Contains(std::string_view a_text, const char* a_token)
		{
			return a_text.find(a_token) != std::string_view::npos;
		}

		void TrimToMostRecent(std::vector<std::string>& a_lines, std::size_t a_maxLines)
		{
			if (a_lines.size() > a_maxLines)
				a_lines.erase(a_lines.begin(), a_lines.end() - static_cast<std::ptrdiff_t>(a_maxLines));
		}
	}

	Slice SliceFencedText(std::string_view a_text, std::size_t a_maxLines)
	{
		Slice             out;
		const std::size_t begin = a_text.rfind(kMarkerBegin);
		if (begin == std::string_view::npos)
			return out;
		out.sawBegin = true;
		const std::size_t end = a_text.find(kMarkerEnd, begin);

		std::size_t start = a_text.find('\n', begin);
		start = (start == std::string_view::npos) ? a_text.size() : start + 1;
		std::size_t stop = a_text.size();
		if (end != std::string_view::npos) {
			out.sawEnd = true;
			const std::size_t lineStart = a_text.rfind('\n', end);
			stop = (lineStart == std::string_view::npos || lineStart < start) ? start : lineStart;
		}

		std::string line;
		const auto  flush = [&]() {
			if (!line.empty()) {
				out.lines.push_back(line);
				line.clear();
			}
		};
		for (const char c : a_text.substr(start, stop - start)) {
			if (c == '\n' || c == '\r')
				flush();
			else
				line += c;
		}
		flush();
		TrimToMostRecent(out.lines, a_maxLines);
		return out;
	}

	Slice SliceFencedLines(const std::deque<std::string>& a_lines, std::size_t a_maxLines)
	{
		Slice       out;
		std::size_t begin = a_lines.size();
		for (std::size_t i = a_lines.size(); i-- > 0;) {
			if (Contains(a_lines[i], kMarkerBegin)) {
				begin = i;
				break;
			}
		}
		if (begin >= a_lines.size())
			return out;
		out.sawBegin = true;

		std::size_t end = a_lines.size();
		for (std::size_t i = begin + 1; i < a_lines.size(); ++i) {
			if (Contains(a_lines[i], kMarkerEnd)) {
				end = i;
				out.sawEnd = true;
				break;
			}
		}
		for (std::size_t i = begin + 1; i < end; ++i)
			if (!a_lines[i].empty())
				out.lines.push_back(a_lines[i]);
		TrimToMostRecent(out.lines, a_maxLines);
		return out;
	}

	void LineSampler::Reset(std::string a_seedLine)
	{
		m_lines.clear();
		m_lastSeen = std::move(a_seedLine);
		m_samples = 0;
		m_ticks = 0;
		m_sawBegin = false;
		m_sawEnd = false;
	}

	void LineSampler::Record(std::string_view a_line)
	{
		m_lastSeen.assign(a_line);
		++m_samples;
		m_lines.emplace_back(a_line);
		while (m_lines.size() > kRingMax)
			m_lines.pop_front();
	}

	LineSampler::Seen LineSampler::Observe(std::string_view a_line)
	{
		++m_ticks;
		if (a_line.empty())
			return Seen::kNothing;
		// A marker is recorded the first time it shows even if it matches the seed, so a stale
		// marker left by an aborted capture cannot swallow this capture's own.
		if (Contains(a_line, kMarkerBegin) && !m_sawBegin) {
			m_sawBegin = true;
			Record(a_line);
			return Seen::kBegin;
		}
		if (Contains(a_line, kMarkerEnd) && m_sawBegin && !m_sawEnd) {
			m_sawEnd = true;
			Record(a_line);
			return Seen::kEnd;
		}
		if (a_line == m_lastSeen)
			return Seen::kNothing;
		Record(a_line);
		return Seen::kLine;
	}

	SourceChooser::Choice SourceChooser::Look(bool a_bufferHasBegin, bool a_samplerSawBegin)
	{
		m_bufferStreak = a_bufferHasBegin ? m_bufferStreak + 1 : 0;
		if (a_samplerSawBegin)
			++m_samplerLooks;
		if (m_bufferStreak >= kStableLooks)
			return Choice::kBuffer;
		if (m_samplerLooks >= kStableLooks)
			return Choice::kSampler;
		return Choice::kUndecided;
	}

	bool QuietDetector::Look(bool a_newLine)
	{
		if (a_newLine) {
			m_sawLine = true;
			m_quiet = 0;
		} else if (m_sawLine) {
			++m_quiet;
		}
		return m_sawLine && m_quiet >= kQuietLooks;
	}
}
