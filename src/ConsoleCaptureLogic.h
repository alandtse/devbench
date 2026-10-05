#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dvb::ConsoleLogCapture
{
	/// Prefixes of the fence commands. Each capture appends its own nonce, so output and markers
	/// from another capture never match this one's.
	inline constexpr const char* kMarkerBegin = "DVBCAPBEGINx9F3";
	inline constexpr const char* kMarkerEnd = "DVBCAPENDx9F3";

	/// One capture's fence commands. A line holds a marker when it contains the whole command as a
	/// token, not followed or preceded by another letter or digit. An empty fence matches nothing.
	struct Fence
	{
		std::string begin;
		std::string end;
	};
	Fence MakeFence(std::uint32_t a_nonce);
	bool  HasMarker(std::string_view a_text, std::string_view a_marker);

	/// The text the game printed round a capture's begin command (e.g. `Script command "` and `" not found.`).
	/// The end marker counts only on a line that is exactly that frame round the end command, so output that
	/// merely mentions the end command never closes the capture. Learned from the game's own line, so it holds in
	/// any language.
	struct Frame
	{
		std::string prefix;
		std::string suffix;
	};
	/// The frame round a_marker when a_line holds it as a whole token.
	std::optional<Frame> FrameOf(std::string_view a_line, std::string_view a_marker);
	bool                 IsFramedLine(std::string_view a_line, const Frame& a_frame, std::string_view a_marker);
	/// Whether a console command is one of devbench's fence commands.
	bool IsFenceCommand(std::string_view a_command);

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

	/// Whether the text holds a begin marker at or after a_fromOffset, and an end marker after it.
	/// Markers before a_fromOffset belong to an earlier capture.
	struct FenceState
	{
		bool hasBegin = false;
		bool hasEnd = false;
	};
	FenceState FindFence(std::string_view a_text, const Fence& a_fence, std::size_t a_fromOffset = 0);

	/// The lines between the LAST begin marker (at or after a_fromOffset) and the end marker after
	/// it, marker lines excluded, blank lines dropped, at most a_maxLines (the most recent).
	Slice SliceFencedText(std::string_view a_text, const Fence& a_fence, std::size_t a_maxLines, std::size_t a_fromOffset = 0);
	Slice SliceFencedLines(const std::deque<std::string>& a_lines, const Fence& a_fence, std::size_t a_maxLines);

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

		void Reset(std::string a_seedLine, Fence a_fence);

		Seen Observe(std::string_view a_line);

		[[nodiscard]] bool                           SawBegin() const { return m_sawBegin; }
		[[nodiscard]] bool                           SawEnd() const { return m_sawEnd; }
		[[nodiscard]] const std::deque<std::string>& Lines() const { return m_lines; }
		[[nodiscard]] std::size_t                    Samples() const { return m_samples; }
		[[nodiscard]] std::size_t                    Ticks() const { return m_ticks; }

	private:
		void Record(std::string_view a_line);

		Fence                   m_fence;
		std::optional<Frame>    m_frame;
		std::deque<std::string> m_lines;
		std::string             m_lastSeen;
		std::size_t             m_samples = 0;
		std::size_t             m_ticks = 0;
		bool                    m_sawBegin = false;
		bool                    m_sawEnd = false;
	};

	/// Picks the output source. The buffer is drained every frame once the Console menu exists, so it
	/// is chosen only if the begin marker is still in it kStableLooks looks in a row.
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

	/// Collects every line printed through ConsoleLog::VPrint during a capture: nothing before the
	/// begin marker, everything from it through the end marker, nothing after. A print may hold
	/// several lines. Output lines are bounded by count and bytes; the two marker lines are kept
	/// outside those bounds. Not thread-safe; the caller locks.
	class PrintCollector
	{
	public:
		static constexpr std::size_t kMaxLines = 20000;
		static constexpr std::size_t kMaxBytes = 4 * 1024 * 1024;

		void Reset(Fence a_fence);
		void Feed(std::string_view a_text);
		/// A print that could not be formatted; counted as one lost line once the begin marker is seen.
		void NoteUnreadable();

		[[nodiscard]] bool                           SawBegin() const { return m_sawBegin; }
		[[nodiscard]] bool                           SawEnd() const { return m_sawEnd; }
		[[nodiscard]] const std::deque<std::string>& Lines() const { return m_lines; }
		[[nodiscard]] std::size_t                    Dropped() const { return m_dropped; }
		[[nodiscard]] std::size_t                    Bytes() const { return m_bytes; }

	private:
		void Line(std::string_view a_line);

		Fence                   m_fence;
		std::optional<Frame>    m_frame;
		std::deque<std::string> m_lines;
		std::size_t             m_outputLines = 0;
		std::size_t             m_bytes = 0;
		std::size_t             m_dropped = 0;
		bool                    m_sawBegin = false;
		bool                    m_sawEnd = false;
	};

	/// Numbers the print collector's capture windows. A print is admitted to the window open when it
	/// starts, formatted outside any lock, then kept only if that window is still the open one, so a
	/// slow print cannot land in a later capture. Not thread-safe; the caller locks.
	class PrintWindows
	{
	public:
		/// Opens a new window with a fresh collector and returns its number (never 0).
		std::uint64_t Open(Fence a_fence);
		/// Closes the open window; what it collected stays readable.
		void                        Close() { m_open = 0; }
		[[nodiscard]] std::uint64_t Current() const { return m_open; }

		/// Feeds a_text (or notes an unreadable print when null) if a_window is still open; false if not.
		bool Keep(std::uint64_t a_window, const std::string* a_text);

		[[nodiscard]] const PrintCollector& Collected() const { return m_collector; }

	private:
		PrintCollector m_collector;
		std::uint64_t  m_count = 0;
		std::uint64_t  m_open = 0;
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
