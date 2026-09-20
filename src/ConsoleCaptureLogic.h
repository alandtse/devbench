#pragma once

#include <cstddef>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace dvb::ConsoleLogCapture
{
	inline constexpr const char* kMarkerBegin = "DVBCAPBEGINx9F3";
	inline constexpr const char* kMarkerEnd = "DVBCAPENDx9F3";

	inline constexpr std::size_t kRingMax = 512;

	/// Looks the begin marker must survive in the buffer, or be seen in the sampler, before a
	/// source is chosen.
	inline constexpr int kStableLooks = 3;
	/// Looks without a new line after which a command's output counts as finished.
	inline constexpr int kQuietLooks = 3;

	struct Slice
	{
		bool                     sawBegin = false;
		bool                     sawEnd = false;
		std::vector<std::string> lines;
	};

	/// The lines between the LAST begin marker and the end marker after it, marker lines
	/// excluded, blank lines dropped, at most a_maxLines (the most recent).
	Slice SliceFencedText(std::string_view a_text, std::size_t a_maxLines);
	Slice SliceFencedLines(const std::deque<std::string>& a_lines, std::size_t a_maxLines);

	/// Builds a scrollback from repeated looks at one "most recent line" slot. A line replaced
	/// between two looks is never seen.
	class LineSampler
	{
	public:
		enum class Seen
		{
			kNothing,
			kLine,
			kBegin,
			kEnd,
		};

		void Reset(std::string a_seedLine);

		Seen Observe(std::string_view a_line);

		[[nodiscard]] bool                           SawBegin() const { return m_sawBegin; }
		[[nodiscard]] bool                           SawEnd() const { return m_sawEnd; }
		[[nodiscard]] const std::deque<std::string>& Lines() const { return m_lines; }
		[[nodiscard]] std::size_t                    Samples() const { return m_samples; }
		[[nodiscard]] std::size_t                    Ticks() const { return m_ticks; }

	private:
		void Record(std::string_view a_line);

		std::deque<std::string> m_lines;
		std::string             m_lastSeen;
		std::size_t             m_samples = 0;
		std::size_t             m_ticks = 0;
		bool                    m_sawBegin = false;
		bool                    m_sawEnd = false;
	};

	/// Chooses where a capture's output will be read from, given what each source shows after the
	/// begin marker was queued. The buffer is drained every frame once the Console menu exists, so
	/// it is chosen only if the marker is still there kStableLooks looks in a row.
	class SourceChooser
	{
	public:
		enum class Choice
		{
			kUndecided,
			kBuffer,
			kSampler,
		};

		Choice Look(bool a_bufferHasBegin, bool a_samplerSawBegin);

	private:
		int m_bufferStreak = 0;
		int m_samplerLooks = -1;
	};

	/// Reports when a command has stopped printing: at least one new line seen, then
	/// kQuietLooks looks with none.
	class QuietDetector
	{
	public:
		bool Look(bool a_newLine);

	private:
		bool m_sawLine = false;
		int  m_quiet = 0;
	};
}
