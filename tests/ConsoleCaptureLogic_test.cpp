#include "test_framework.h"

#include "ConsoleCaptureLogic.h"

using dvb::ConsoleLogCapture::Fence;
using dvb::ConsoleLogCapture::FrameOf;
using dvb::ConsoleLogCapture::HasMarker;
using dvb::ConsoleLogCapture::IsFenceCommand;
using dvb::ConsoleLogCapture::IsFramedLine;
using dvb::ConsoleLogCapture::kRingMax;
using dvb::ConsoleLogCapture::LineSampler;
using dvb::ConsoleLogCapture::MakeFence;
using dvb::ConsoleLogCapture::PrintCollector;
using dvb::ConsoleLogCapture::SliceFencedLines;
using dvb::ConsoleLogCapture::SliceFencedText;

namespace
{
	const Fence kF = MakeFence(0x1234ABCD);

	std::string MarkerLine(const std::string& a_marker)
	{
		return "Script command \"" + a_marker + "\" not found.";
	}

	std::string BeginLine() { return MarkerLine(kF.begin); }
	std::string EndLine() { return MarkerLine(kF.end); }
}

TEST_CASE("text slicing returns the lines between the fence markers")
{
	const std::string text = "older output\n" + BeginLine() + "\nline one\nline two\n" + EndLine() + "\nlater\n";
	const auto        slice = SliceFencedText(text, kF, 200);
	CHECK(slice.sawBegin);
	CHECK(slice.sawEnd);
	CHECK(slice.lines.size() == 2);
	CHECK(slice.lines[0] == "line one");
	CHECK(slice.lines[1] == "line two");
}

TEST_CASE("text slicing handles CRLF and drops blank lines")
{
	const std::string text = BeginLine() + "\r\nalpha\r\n\r\nbeta\r\n" + EndLine() + "\r\n";
	const auto        slice = SliceFencedText(text, kF, 200);
	CHECK(slice.lines.size() == 2);
	CHECK(slice.lines[0] == "alpha");
	CHECK(slice.lines[1] == "beta");
}

TEST_CASE("text slicing uses the last begin marker")
{
	const std::string text = BeginLine() + "\nold\n" + EndLine() + "\n" + BeginLine() + "\nnew\n" + EndLine() + "\n";
	const auto        slice = SliceFencedText(text, kF, 200);
	CHECK(slice.lines.size() == 1);
	CHECK(slice.lines[0] == "new");
}

TEST_CASE("text slicing reports a missing end marker and a missing begin marker")
{
	const auto noEnd = SliceFencedText(BeginLine() + "\npartial\n", kF, 200);
	CHECK(noEnd.sawBegin);
	CHECK(!noEnd.sawEnd);
	CHECK(noEnd.lines.size() == 1);

	const auto noBegin = SliceFencedText("just output\n" + EndLine() + "\n", kF, 200);
	CHECK(!noBegin.sawBegin);
	CHECK(noBegin.lines.empty());

	CHECK(!SliceFencedText("", kF, 200).sawBegin);
}

TEST_CASE("slicing keeps only the most recent lines when capped")
{
	std::string text = BeginLine() + "\n";
	for (int i = 0; i < 10; ++i)
		text += "row " + std::to_string(i) + "\n";
	text += EndLine() + "\n";
	const auto slice = SliceFencedText(text, kF, 3);
	CHECK(slice.lines.size() == 3);
	CHECK(slice.lines.front() == "row 7");
	CHECK(slice.lines.back() == "row 9");
}

TEST_CASE("line slicing matches text slicing on the same fenced window")
{
	std::deque<std::string> lines{ "stale", BeginLine(), "one", "two", EndLine(), "after" };
	const auto              slice = SliceFencedLines(lines, kF, 200);
	CHECK(slice.sawBegin);
	CHECK(slice.sawEnd);
	CHECK(slice.lines.size() == 2);
	CHECK(slice.lines[0] == "one");

	std::deque<std::string> unfinished{ BeginLine(), "one" };
	const auto              partial = SliceFencedLines(unfinished, kF, 200);
	CHECK(partial.sawBegin);
	CHECK(!partial.sawEnd);
	CHECK(SliceFencedLines({}, kF, 200).lines.empty());
}

TEST_CASE("sampler records changes and ignores an unchanged line")
{
	LineSampler sampler;
	sampler.Reset("stale message", kF);
	CHECK(sampler.Observe("stale message") == LineSampler::Seen::kNothing);
	CHECK(sampler.Observe("") == LineSampler::Seen::kNothing);
	CHECK(sampler.Observe("first") == LineSampler::Seen::kLine);
	CHECK(sampler.Observe("first") == LineSampler::Seen::kNothing);
	CHECK(sampler.Observe("second") == LineSampler::Seen::kLine);
	CHECK(sampler.Samples() == 2);
	CHECK(sampler.Ticks() == 5);
}

TEST_CASE("sampler reports each marker once and slices a complete capture")
{
	LineSampler sampler;
	sampler.Reset("", kF);
	CHECK(sampler.Observe(BeginLine()) == LineSampler::Seen::kBegin);
	CHECK(sampler.SawBegin());
	CHECK(sampler.Observe("GetActorValue: Health >> 100.00") == LineSampler::Seen::kLine);
	CHECK(sampler.Observe(EndLine()) == LineSampler::Seen::kEnd);
	CHECK(sampler.SawEnd());

	const auto slice = SliceFencedLines(sampler.Lines(), kF, 200);
	CHECK(slice.sawBegin);
	CHECK(slice.sawEnd);
	CHECK(slice.lines.size() == 1);
	CHECK(slice.lines[0] == "GetActorValue: Health >> 100.00");
}

TEST_CASE("a stale begin marker left by an aborted capture cannot swallow the next one")
{
	LineSampler sampler;
	sampler.Reset(BeginLine(), kF);  // seeded with the marker an aborted capture left showing
	CHECK(sampler.Observe(BeginLine()) == LineSampler::Seen::kBegin);
	CHECK(sampler.SawBegin());
	CHECK(sampler.Observe("output") == LineSampler::Seen::kLine);
	CHECK(sampler.Observe(EndLine()) == LineSampler::Seen::kEnd);
	CHECK(SliceFencedLines(sampler.Lines(), kF, 200).lines.size() == 1);
}

TEST_CASE("the previous capture's end marker is not taken as this capture's end")
{
	LineSampler sampler;
	sampler.Reset(EndLine(), kF);  // the last capture's end marker is still showing
	CHECK(sampler.Observe(EndLine()) == LineSampler::Seen::kNothing);
	CHECK(!sampler.SawEnd());
	CHECK(sampler.Observe(BeginLine()) == LineSampler::Seen::kBegin);
	CHECK(sampler.Observe("output") == LineSampler::Seen::kLine);
	CHECK(!sampler.SawEnd());
	CHECK(sampler.Observe(EndLine()) == LineSampler::Seen::kEnd);
	CHECK(sampler.SawEnd());
	CHECK(SliceFencedLines(sampler.Lines(), kF, 200).lines.size() == 1);
}

TEST_CASE("a command that prints nothing still ends on the end marker")
{
	LineSampler sampler;
	sampler.Reset("", kF);
	sampler.Observe(BeginLine());
	CHECK(sampler.Observe(EndLine()) == LineSampler::Seen::kEnd);
	const auto slice = SliceFencedLines(sampler.Lines(), kF, 200);
	CHECK(slice.sawBegin);
	CHECK(slice.sawEnd);
	CHECK(slice.lines.empty());
}

TEST_CASE("reset clears the previous capture")
{
	LineSampler sampler;
	sampler.Reset("", kF);
	sampler.Observe(BeginLine());
	sampler.Observe("x");
	sampler.Observe(EndLine());
	sampler.Reset("seed", kF);
	CHECK(!sampler.SawBegin());
	CHECK(!sampler.SawEnd());
	CHECK(sampler.Lines().empty());
	CHECK(sampler.Samples() == 0);
	CHECK(sampler.Ticks() == 0);
}

TEST_CASE("the sampler ring is bounded")
{
	LineSampler sampler;
	sampler.Reset("", kF);
	for (std::size_t i = 0; i < kRingMax + 40; ++i)
		sampler.Observe("line " + std::to_string(i));
	CHECK(sampler.Lines().size() == kRingMax);
	CHECK(sampler.Lines().back() == "line " + std::to_string(kRingMax + 39));
}

using dvb::ConsoleLogCapture::kQuietLooks;
using dvb::ConsoleLogCapture::kStableLooks;
using dvb::ConsoleLogCapture::QuietDetector;
using dvb::ConsoleLogCapture::SourceChooser;

TEST_CASE("the buffer is chosen once it holds the begin marker for several looks in a row")
{
	SourceChooser chooser;
	for (int i = 1; i < kStableLooks; ++i)
		CHECK(chooser.Look(true, true) == SourceChooser::Choice::kUndecided);
	CHECK(chooser.Look(true, true) == SourceChooser::Choice::kBuffer);
}

TEST_CASE("a marker that vanishes from the buffer falls back to the sampler")
{
	// The Console menu drains the buffer each frame: the marker shows once, then is gone.
	SourceChooser chooser;
	CHECK(chooser.Look(true, true) == SourceChooser::Choice::kUndecided);
	SourceChooser::Choice choice = SourceChooser::Choice::kUndecided;
	for (int i = 0; i < kStableLooks && choice == SourceChooser::Choice::kUndecided; ++i)
		choice = chooser.Look(false, true);
	CHECK(choice == SourceChooser::Choice::kSampler);
}

TEST_CASE("a buffer marker that flickers never reaches a stable streak")
{
	SourceChooser chooser;
	CHECK(chooser.Look(true, true) == SourceChooser::Choice::kUndecided);
	CHECK(chooser.Look(true, true) == SourceChooser::Choice::kUndecided);
	CHECK(chooser.Look(false, true) == SourceChooser::Choice::kUndecided);
	CHECK(chooser.Look(true, true) == SourceChooser::Choice::kSampler);
}

TEST_CASE("nothing is chosen until a begin marker has been seen")
{
	SourceChooser chooser;
	for (int i = 0; i < 20; ++i)
		CHECK(chooser.Look(false, false) == SourceChooser::Choice::kUndecided);
}

TEST_CASE("a command counts as finished only after output and then quiet")
{
	QuietDetector detector;
	for (int i = 0; i < 10; ++i)
		CHECK(!detector.Look(false));  // a command that has not printed yet is never finished
	CHECK(!detector.Look(true));
	for (int i = 1; i < kQuietLooks; ++i)
		CHECK(!detector.Look(false));
	CHECK(detector.Look(false));
}

TEST_CASE("new output restarts the quiet count")
{
	QuietDetector detector;
	detector.Look(true);
	detector.Look(false);
	detector.Look(false);
	CHECK(!detector.Look(true));
	for (int i = 1; i < kQuietLooks; ++i)
		CHECK(!detector.Look(false));
	CHECK(detector.Look(false));
}

using dvb::ConsoleLogCapture::FindFence;

TEST_CASE("a fence before the starting offset belongs to an earlier capture")
{
	const std::string previous = BeginLine() + "\nold output\n" + EndLine() + "\n";
	const auto        state = FindFence(previous, kF, previous.size());
	CHECK(!state.hasBegin);
	CHECK(!state.hasEnd);
	CHECK(!SliceFencedText(previous, kF, 200, previous.size()).sawBegin);
}

TEST_CASE("a stale end marker cannot complete a new capture")
{
	const std::string previous = BeginLine() + "\nold output\n" + EndLine() + "\n";
	std::string       text = previous + BeginLine() + "\n";
	const auto        started = FindFence(text, kF, previous.size());
	CHECK(started.hasBegin);
	CHECK(!started.hasEnd);

	text += "new output\n" + EndLine() + "\n";
	const auto finished = FindFence(text, kF, previous.size());
	CHECK(finished.hasBegin);
	CHECK(finished.hasEnd);
	const auto slice = SliceFencedText(text, kF, 200, previous.size());
	CHECK(slice.lines.size() == 1);
	CHECK(slice.lines[0] == "new output");
}

TEST_CASE("fence detection with no offset finds the latest fence")
{
	const std::string text = BeginLine() + "\na\n" + EndLine() + "\n" + BeginLine() + "\n";
	const auto        state = FindFence(text, kF);
	CHECK(state.hasBegin);
	CHECK(!state.hasEnd);
	CHECK(!FindFence("no markers here", kF).hasBegin);
}

TEST_CASE("print collector keeps every line of a multi-line command between the markers")
{
	PrintCollector c;
	c.Reset(kF);
	c.Feed("noise before the capture");
	c.Feed(BeginLine());
	c.Feed("00000014 (2 lights)");
	c.Feed("> skeleton_female.nif\n> NPC Root [Root]\r\n> MagicRight\n");
	c.Feed("> LP_Light[Let There Be Glow|MagicLightWhite01](1)#0 (radius: 246.9|fade: 0.97|visible)");
	c.Feed(EndLine());
	c.Feed("noise after the capture");
	CHECK(c.SawBegin());
	CHECK(c.SawEnd());
	const auto slice = SliceFencedLines(c.Lines(), kF, 200);
	CHECK(slice.sawBegin);
	CHECK(slice.sawEnd);
	CHECK(slice.lines.size() == 5);
	CHECK(slice.lines[0] == "00000014 (2 lights)");
	CHECK(slice.lines[2] == "> NPC Root [Root]");
	CHECK(slice.lines[4].starts_with("> LP_Light["));
}

TEST_CASE("print collector ignores an end marker before the begin marker")
{
	PrintCollector c;
	c.Reset(kF);
	c.Feed(EndLine());
	CHECK(!c.SawEnd());
	c.Feed(BeginLine());
	c.Feed("only line");
	CHECK(c.SawBegin());
	CHECK(!c.SawEnd());
	CHECK(SliceFencedLines(c.Lines(), kF, 200).lines.size() == 1);
}

TEST_CASE("print collector caps its lines, counts the drop, and still sees the end marker")
{
	PrintCollector c;
	c.Reset(kF);
	c.Feed(BeginLine());
	for (std::size_t i = 0; i < PrintCollector::kMaxLines + 10; ++i)
		c.Feed("x");
	c.Feed(EndLine());
	CHECK(c.SawEnd());
	CHECK(c.Dropped() == 10);  // the markers sit outside the cap
	CHECK(SliceFencedLines(c.Lines(), kF, PrintCollector::kMaxLines).lines.size() == PrintCollector::kMaxLines);
	c.Reset(kF);
	CHECK(!c.SawBegin());
	CHECK(c.Lines().empty());
	CHECK(c.Dropped() == 0);
}

TEST_CASE("each capture's fence carries its own nonce")
{
	const Fence other = MakeFence(0x1234ABCE);
	CHECK(kF.begin != other.begin);
	CHECK(kF.end != other.end);
	CHECK(IsFenceCommand(kF.begin));
	CHECK(IsFenceCommand(other.end));
	CHECK(!IsFenceCommand("getav health"));
}

TEST_CASE("a marker counts only as a whole token")
{
	CHECK(HasMarker(BeginLine(), kF.begin));
	CHECK(HasMarker(kF.begin, kF.begin));
	CHECK(!HasMarker("Script command \"" + kF.begin + "0\" not found.", kF.begin));  // a longer token
	CHECK(!HasMarker("x" + kF.begin, kF.begin));
	CHECK(!HasMarker("anything", ""));
	CHECK(!HasMarker(BeginLine(), MakeFence(0x1234ABCE).begin));
}

TEST_CASE("another capture's markers do not open or close this one")
{
	const Fence    old = MakeFence(0x0BADF00D);
	PrintCollector c;
	c.Reset(kF);
	c.Feed(MarkerLine(old.begin));
	CHECK(!c.SawBegin());
	c.Feed(BeginLine());
	c.Feed(MarkerLine(old.end));
	CHECK(!c.SawEnd());
	c.Feed(EndLine());
	CHECK(c.SawEnd());
	const auto slice = SliceFencedLines(c.Lines(), kF, 200);
	CHECK(slice.lines.size() == 1);
	CHECK(slice.lines[0] == MarkerLine(old.end));

	const std::string text = MarkerLine(old.begin) + "\nold\n" + MarkerLine(old.end) + "\n";
	CHECK(!FindFence(text, kF).hasBegin);
	LineSampler sampler;
	sampler.Reset("", kF);
	CHECK(sampler.Observe(MarkerLine(old.begin)) == LineSampler::Seen::kLine);
	CHECK(!sampler.SawBegin());
}

TEST_CASE("an empty fence matches nothing")
{
	PrintCollector c;
	c.Reset(Fence{});
	c.Feed(BeginLine());
	CHECK(!c.SawBegin());
	CHECK(!SliceFencedLines({ BeginLine(), "x", EndLine() }, Fence{}, 200).sawBegin);
}

TEST_CASE("print collector caps output bytes and keeps both markers outside the caps")
{
	PrintCollector c;
	c.Reset(kF);
	c.Feed(BeginLine());
	const std::string big(PrintCollector::kMaxBytes / 2, 'a');
	c.Feed(big);
	c.Feed(big);
	c.Feed("one byte too many");
	c.Feed(EndLine());
	CHECK(c.SawBegin());
	CHECK(c.SawEnd());
	CHECK(c.Bytes() == PrintCollector::kMaxBytes);
	CHECK(c.Dropped() == 1);
	const auto slice = SliceFencedLines(c.Lines(), kF, 200);
	CHECK(slice.sawEnd);
	CHECK(slice.lines.size() == 2);
}

TEST_CASE("an unreadable print counts as lost only inside the fence")
{
	PrintCollector c;
	c.Reset(kF);
	c.NoteUnreadable();
	CHECK(c.Dropped() == 0);
	c.Feed(BeginLine());
	c.NoteUnreadable();
	c.Feed(EndLine());
	c.NoteUnreadable();
	CHECK(c.Dropped() == 1);
}

using dvb::ConsoleLogCapture::PrintWindows;

TEST_CASE("a print admitted to an earlier window cannot land in a later one")
{
	PrintWindows windows;
	const auto   first = windows.Open(kF);
	CHECK(first != 0);
	const std::string begin = BeginLine();
	CHECK(windows.Keep(first, &begin));
	windows.Close();
	const std::string late = "late output from the first capture";
	CHECK(!windows.Keep(first, &late));  // the window closed while the print was being formatted

	const Fence next = MakeFence(0x0000BEEF);
	const auto  second = windows.Open(next);
	CHECK(second != first);
	CHECK(!windows.Keep(first, &late));  // nor does it land after the next capture opened
	const std::string nextBegin = MarkerLine(next.begin);
	const std::string output = "this capture's output";
	const std::string nextEnd = MarkerLine(next.end);
	CHECK(windows.Keep(second, &nextBegin));
	CHECK(windows.Keep(second, &output));
	CHECK(windows.Keep(second, &nextEnd));
	const auto slice = SliceFencedLines(windows.Collected().Lines(), next, 200);
	CHECK(slice.sawEnd);
	CHECK(slice.lines.size() == 1);
	CHECK(slice.lines[0] == output);
}

TEST_CASE("a closed window keeps what it collected and admits nothing")
{
	PrintWindows windows;
	CHECK(windows.Current() == 0);
	const std::string line = "x";
	CHECK(!windows.Keep(0, &line));
	const auto        w = windows.Open(kF);
	const std::string begin = BeginLine();
	windows.Keep(w, &begin);
	windows.Keep(w, nullptr);
	windows.Close();
	CHECK(windows.Current() == 0);
	CHECK(!windows.Keep(w, nullptr));
	CHECK(windows.Collected().SawBegin());
	CHECK(windows.Collected().Dropped() == 1);
}

TEST_CASE("the end marker counts only in the begin marker's exact frame")
{
	const auto frame = FrameOf(BeginLine(), kF.begin);
	CHECK(frame.has_value());
	CHECK(frame->prefix == "Script command \"");
	CHECK(frame->suffix == "\" not found.");
	CHECK(IsFramedLine(EndLine(), *frame, kF.end));
	CHECK(!IsFramedLine("echo: " + kF.end, *frame, kF.end));                             // output that mentions the end command
	CHECK(!IsFramedLine(EndLine() + " (repeated)", *frame, kF.end));                     // a longer line
	CHECK(!IsFramedLine("Script command \"" + kF.end + "\" missing.", *frame, kF.end));  // another frame
}

TEST_CASE("output mentioning the end command does not close any source")
{
	const std::string mention = "the fence ends with " + kF.end + " here";
	PrintCollector    c;
	c.Reset(kF);
	c.Feed(BeginLine());
	c.Feed(mention);
	CHECK(!c.SawEnd());
	c.Feed(EndLine());
	CHECK(c.SawEnd());
	CHECK(SliceFencedLines(c.Lines(), kF, 200).lines.size() == 1);

	LineSampler sampler;
	sampler.Reset("", kF);
	sampler.Observe(BeginLine());
	CHECK(sampler.Observe(mention) == LineSampler::Seen::kLine);
	CHECK(sampler.Observe(EndLine()) == LineSampler::Seen::kEnd);

	const std::string text = BeginLine() + "\n" + mention + "\nreal output\n" + EndLine() + "\n";
	CHECK(FindFence(text, kF).hasEnd);
	const auto slice = SliceFencedText(text, kF, 200);
	CHECK(slice.sawEnd);
	CHECK(slice.lines.size() == 2);
	CHECK(!FindFence(BeginLine() + "\n" + mention + "\n", kF).hasEnd);
}

TEST_CASE("a frame learned in another language still closes the capture")
{
	const std::string begin = "Commande de script « " + kF.begin + " » introuvable.";
	const std::string end = "Commande de script « " + kF.end + " » introuvable.";
	PrintCollector    c;
	c.Reset(kF);
	c.Feed(begin);
	c.Feed("sortie");
	c.Feed(end);
	CHECK(c.SawEnd());
	CHECK(SliceFencedLines(c.Lines(), kF, 200).lines.size() == 1);
}
