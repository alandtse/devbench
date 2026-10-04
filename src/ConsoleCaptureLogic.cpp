#include "ConsoleCaptureLogic.h"

#include <cctype>
#include <format>

namespace dvb::ConsoleLogCapture
{
	namespace
	{
		bool Word(char a_c)
		{
			return std::isalnum(static_cast<unsigned char>(a_c)) != 0 || a_c == '_';
		}

		// Position of a_marker as a whole token in a_text: the last one when a_last, else the first
		// at or after a_from; npos when absent.
		std::size_t FindMarker(std::string_view a_text, std::string_view a_marker, bool a_last, std::size_t a_from = 0)
		{
			if (a_marker.empty())
				return std::string_view::npos;
			std::size_t at = a_last ? a_text.rfind(a_marker) : a_text.find(a_marker, a_from);
			while (at != std::string_view::npos) {
				const std::size_t after = at + a_marker.size();
				const bool        alone = (at == 0 || !Word(a_text[at - 1])) && (after >= a_text.size() || !Word(a_text[after]));
				if (alone)
					return at;
				if (a_last) {
					if (at == 0)
						break;
					at = a_text.rfind(a_marker, at - 1);
				} else {
					at = a_text.find(a_marker, at + 1);
				}
			}
			return std::string_view::npos;
		}

		void TrimToMostRecent(std::vector<std::string>& a_lines, std::size_t a_maxLines)
		{
			if (a_lines.size() > a_maxLines)
				a_lines.erase(a_lines.begin(), a_lines.end() - static_cast<std::ptrdiff_t>(a_maxLines));
		}
	}

	Fence MakeFence(std::uint32_t a_nonce)
	{
		return { std::format("{}{:08X}", kMarkerBegin, a_nonce), std::format("{}{:08X}", kMarkerEnd, a_nonce) };
	}

	bool HasMarker(std::string_view a_text, std::string_view a_marker)
	{
		return FindMarker(a_text, a_marker, false) != std::string_view::npos;
	}

	std::optional<Frame> FrameOf(std::string_view a_line, std::string_view a_marker)
	{
		const std::size_t at = FindMarker(a_line, a_marker, false);
		if (at == std::string_view::npos)
			return std::nullopt;
		return Frame{ std::string(a_line.substr(0, at)), std::string(a_line.substr(at + a_marker.size())) };
	}

	bool IsFramedLine(std::string_view a_line, const Frame& a_frame, std::string_view a_marker)
	{
		return !a_marker.empty() && a_line.size() == a_frame.prefix.size() + a_marker.size() + a_frame.suffix.size() &&
		       a_line.starts_with(a_frame.prefix) && a_line.ends_with(a_frame.suffix) &&
		       a_line.substr(a_frame.prefix.size(), a_marker.size()) == a_marker;
	}

	bool IsFenceCommand(std::string_view a_command)
	{
		return a_command.starts_with(kMarkerBegin) || a_command.starts_with(kMarkerEnd);
	}

	namespace
	{
		bool LineBreak(char a_c) { return a_c == '\n' || a_c == '\r'; }

		// The begin marker's line (its frame) and the position of the first exactly framed end line after it.
		struct TextFence
		{
			std::size_t begin = std::string_view::npos;  // where the begin line starts
			std::size_t afterBegin = 0;                  // first character after the begin line
			std::size_t end = std::string_view::npos;    // where the end line starts
		};

		TextFence LocateFence(std::string_view a_text, const Fence& a_fence)
		{
			TextFence         out;
			const std::size_t at = FindMarker(a_text, a_fence.begin, true);
			if (at == std::string_view::npos)
				return out;
			std::size_t lineStart = at;
			while (lineStart > 0 && !LineBreak(a_text[lineStart - 1]))
				--lineStart;
			std::size_t lineEnd = at;
			while (lineEnd < a_text.size() && !LineBreak(a_text[lineEnd]))
				++lineEnd;
			out.begin = lineStart;
			out.afterBegin = lineEnd;
			const auto frame = FrameOf(a_text.substr(lineStart, lineEnd - lineStart), a_fence.begin);
			for (std::size_t s = lineEnd; s < a_text.size();) {
				if (LineBreak(a_text[s])) {
					++s;
					continue;
				}
				std::size_t e = s;
				while (e < a_text.size() && !LineBreak(a_text[e]))
					++e;
				if (frame && IsFramedLine(a_text.substr(s, e - s), *frame, a_fence.end)) {
					out.end = s;
					break;
				}
				s = e;
			}
			return out;
		}
	}

	FenceState FindFence(std::string_view a_text, const Fence& a_fence, std::size_t a_fromOffset)
	{
		FenceState state;
		const auto fence = LocateFence(a_text, a_fence);
		if (fence.begin == std::string_view::npos || fence.begin < a_fromOffset)
			return state;
		state.hasBegin = true;
		state.hasEnd = fence.end != std::string_view::npos;
		return state;
	}

	Slice SliceFencedText(std::string_view a_text, const Fence& a_fence, std::size_t a_maxLines, std::size_t a_fromOffset)
	{
		Slice      out;
		const auto fence = LocateFence(a_text, a_fence);
		if (fence.begin == std::string_view::npos || fence.begin < a_fromOffset)
			return out;
		out.sawBegin = true;

		const std::size_t start = fence.afterBegin;
		std::size_t       stop = a_text.size();
		if (fence.end != std::string_view::npos) {
			out.sawEnd = true;
			stop = fence.end;
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

	Slice SliceFencedLines(const std::deque<std::string>& a_lines, const Fence& a_fence, std::size_t a_maxLines)
	{
		Slice                out;
		std::size_t          begin = a_lines.size();
		std::optional<Frame> frame;
		for (std::size_t i = a_lines.size(); i-- > 0;) {
			if ((frame = FrameOf(a_lines[i], a_fence.begin))) {
				begin = i;
				break;
			}
		}
		if (begin >= a_lines.size())
			return out;
		out.sawBegin = true;

		std::size_t end = a_lines.size();
		for (std::size_t i = begin + 1; i < a_lines.size(); ++i) {
			if (IsFramedLine(a_lines[i], *frame, a_fence.end)) {
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

	void LineSampler::Reset(std::string a_seedLine, Fence a_fence)
	{
		m_fence = std::move(a_fence);
		m_frame.reset();
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
		if (!m_sawBegin && (m_frame = FrameOf(a_line, m_fence.begin))) {
			m_sawBegin = true;
			Record(a_line);
			return Seen::kBegin;
		}
		if (m_sawBegin && !m_sawEnd && IsFramedLine(a_line, *m_frame, m_fence.end)) {
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

	std::uint64_t PrintWindows::Open(Fence a_fence)
	{
		m_collector.Reset(std::move(a_fence));
		m_open = ++m_count;
		return m_open;
	}

	bool PrintWindows::Keep(std::uint64_t a_window, const std::string* a_text)
	{
		if (a_window == 0 || a_window != m_open)
			return false;
		if (a_text)
			m_collector.Feed(*a_text);
		else
			m_collector.NoteUnreadable();
		return true;
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

	void PrintCollector::Reset(Fence a_fence)
	{
		m_fence = std::move(a_fence);
		m_frame.reset();
		m_lines.clear();
		m_outputLines = 0;
		m_bytes = 0;
		m_dropped = 0;
		m_sawBegin = false;
		m_sawEnd = false;
	}

	void PrintCollector::NoteUnreadable()
	{
		if (m_sawBegin && !m_sawEnd)
			++m_dropped;
	}

	void PrintCollector::Line(std::string_view a_line)
	{
		if (m_sawEnd || a_line.empty())
			return;
		if (!m_sawBegin) {
			if (!(m_frame = FrameOf(a_line, m_fence.begin)))
				return;
			m_sawBegin = true;
			m_lines.emplace_back(a_line);
			return;
		}
		if (IsFramedLine(a_line, *m_frame, m_fence.end)) {
			m_sawEnd = true;
			m_lines.emplace_back(a_line);
			return;
		}
		if (m_outputLines >= kMaxLines || m_bytes + a_line.size() > kMaxBytes) {
			++m_dropped;
			return;
		}
		++m_outputLines;
		m_bytes += a_line.size();
		m_lines.emplace_back(a_line);
	}

	void PrintCollector::Feed(std::string_view a_text)
	{
		std::size_t start = 0;
		for (std::size_t i = 0; i <= a_text.size(); ++i) {
			if (i == a_text.size() || a_text[i] == '\n' || a_text[i] == '\r') {
				Line(a_text.substr(start, i - start));
				start = i + 1;
			}
		}
	}
}
