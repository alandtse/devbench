#include "ConsoleLogCapture.h"

#include "GameState.h"
#include "MainThread.h"
#include "ToolRegistry.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>

namespace dvb::ConsoleLogCapture
{
	namespace
	{
		using namespace std::chrono;
		using Clock = steady_clock;

		constexpr auto kLookInterval = milliseconds(8);
		constexpr auto kLookTimeout = milliseconds(2000);
		constexpr auto kCaptureDeadline = seconds(30);
		constexpr int  kBeginLooks = 60;
		constexpr int  kCommandLooks = 25;
		constexpr int  kEndLooks = 60;

		enum class Source
		{
			kNone,
			kBuffer,
			kSampler,
		};

		// Sampler state is main thread only.
		LineSampler         g_sampler;
		int                 g_lastFrame = -1;
		std::size_t         g_engineFrames = 0;
		std::atomic<Source> g_source{ Source::kNone };
		std::atomic<bool>   g_timedOut{ false };
		std::mutex          g_captureMutex;

		std::string CurrentLine()
		{
			auto* cl = RE::ConsoleLog::GetSingleton();
			if (!cl)
				return {};
			return std::string(cl->lastMessage, ::strnlen(cl->lastMessage, sizeof(cl->lastMessage)));
		}

		std::string_view BufferText()
		{
			auto* cl = RE::ConsoleLog::GetSingleton();
			if (!cl)
				return {};
			const char* raw = cl->buffer.c_str();
			return raw ? std::string_view(raw) : std::string_view{};
		}

		bool Has(std::string_view a_text, const char* a_token)
		{
			return a_text.find(a_token) != std::string_view::npos;
		}

		struct LookView
		{
			LineSampler::Seen seen = LineSampler::Seen::kNothing;
			bool              samplerSawBegin = false;
			bool              samplerSawEnd = false;
			bool              bufferHasBegin = false;
			bool              bufferHasEnd = false;
		};

		LookView LookAtSources()
		{
			const int frame = game::CurrentFrame();
			if (frame != g_lastFrame) {
				++g_engineFrames;
				g_lastFrame = frame;
			}
			LookView view;
			view.seen = g_sampler.Observe(CurrentLine());
			view.samplerSawBegin = g_sampler.SawBegin();
			view.samplerSawEnd = g_sampler.SawEnd();
			const auto buffer = BufferText();
			view.bufferHasBegin = Has(buffer, kMarkerBegin);
			view.bufferHasEnd = Has(buffer, kMarkerEnd);
			return view;
		}

		void Queue(std::string a_command)
		{
			if (auto* task = SKSE::GetTaskInterface())
				task->AddTask([c = std::move(a_command)]() { RE::Console::ExecuteCommand(c.c_str()); });
		}

		void QueueThenEndMarker(std::string a_command)
		{
			if (auto* task = SKSE::GetTaskInterface())
				task->AddTask([c = std::move(a_command)]() {
					RE::Console::ExecuteCommand(c.c_str());
					RE::Console::ExecuteCommand(kMarkerEnd);
				});
		}

		// Empty when the main thread does not answer, e.g. during a load screen.
		std::optional<LookView> Look()
		{
			std::this_thread::sleep_for(kLookInterval);
			try {
				LookView view;
				MainThread::RunAndWait([&view]() -> json {
					view = LookAtSources();
					return true;
				},
					kLookTimeout);
				return view;
			} catch (const ToolError& e) {
				logs::warn("devbench: console capture stopped looking: {}", e.what());
				return std::nullopt;
			}
		}

		template <class Done>
		bool WaitFor(Clock::time_point a_deadline, int a_looks, Done a_done)
		{
			for (int i = 0; i < a_looks && Clock::now() < a_deadline; ++i) {
				const auto view = Look();
				if (!view)
					return false;
				if (a_done(*view))
					return true;
			}
			return false;
		}

		Source ChooseSource(Clock::time_point a_deadline)
		{
			Queue(kMarkerBegin);
			SourceChooser chooser;
			auto          choice = SourceChooser::Choice::kUndecided;
			WaitFor(a_deadline, kBeginLooks, [&](const LookView& v) {
				choice = chooser.Look(v.bufferHasBegin, v.samplerSawBegin);
				return choice != SourceChooser::Choice::kUndecided;
			});
			switch (choice) {
			case SourceChooser::Choice::kBuffer:
				return Source::kBuffer;
			case SourceChooser::Choice::kSampler:
				return Source::kSampler;
			default:
				return Source::kNone;
			}
		}

		bool CaptureFromBuffer(const std::string& a_command, Clock::time_point a_deadline)
		{
			QueueThenEndMarker(a_command);
			return WaitFor(a_deadline, kEndLooks, [](const LookView& v) { return v.bufferHasEnd; });
		}

		// The sampler keeps one line per look, so the command and the end marker go on separate ticks.
		bool CaptureFromSampler(const std::string& a_command, Clock::time_point a_deadline)
		{
			Queue(a_command);
			QuietDetector quiet;
			WaitFor(a_deadline, kCommandLooks, [&](const LookView& v) {
				return quiet.Look(v.seen == LineSampler::Seen::kLine);
			});
			Queue(kMarkerEnd);
			return WaitFor(a_deadline, kEndLooks, [](const LookView& v) { return v.samplerSawEnd; });
		}
	}

	void RunFencedCapture(const std::string& a_command)
	{
		std::unique_lock<std::mutex> owned(g_captureMutex, std::try_to_lock);
		if (!owned.owns_lock())
			throw ToolError(409, "a console capture is already running; retry when it finishes");

		g_source.store(Source::kNone);
		g_timedOut.store(false);
		MainThread::RunAndWait([]() -> json {
			g_sampler.Reset(CurrentLine());
			g_lastFrame = -1;
			g_engineFrames = 0;
			return true;
		},
			kLookTimeout);

		const auto deadline = Clock::now() + kCaptureDeadline;
		const auto source = ChooseSource(deadline);
		if (source == Source::kNone) {
			logs::warn("devbench: console capture never saw its begin marker");
			g_timedOut.store(true);
			return;
		}
		g_source.store(source);

		const bool finished = source == Source::kBuffer ? CaptureFromBuffer(a_command, deadline) :
		                                                  CaptureFromSampler(a_command, deadline);
		if (!finished) {
			logs::warn("devbench: console capture did not see its end marker");
			g_timedOut.store(true);
		}
	}

	Result ReadFenced(std::size_t a_maxLines)
	{
		Result out;
		out.timedOut = g_timedOut.load();
		out.ringLines = g_sampler.Lines().size();
		out.samples = g_sampler.Samples();
		out.ticks = g_sampler.Ticks();
		out.engineFrames = g_engineFrames;

		if (auto* ui = RE::UI::GetSingleton()) {
			out.consoleMenuExists = ui->GetMenu(RE::Console::MENU_NAME).get() != nullptr;
			out.consoleMenuOpen = ui->IsMenuOpen(RE::Console::MENU_NAME);
		}
		out.consoleMode = RE::ConsoleLog::IsConsoleMode();

		auto* cl = RE::ConsoleLog::GetSingleton();
		if (!cl) {
			out.consoleLogNull = true;
			return out;
		}
		out.lastMessage = CurrentLine();
		out.lastMessageHasBegin = Has(out.lastMessage, kMarkerBegin);
		const auto buffer = BufferText();
		out.bufferEmpty = buffer.empty();
		out.bufferLen = buffer.size();
		out.bufferHasBegin = Has(buffer, kMarkerBegin);

		const Source source = g_source.load();
		Slice        slice;
		if (source == Source::kSampler) {
			slice = SliceFencedLines(g_sampler.Lines(), a_maxLines);
			out.source = "sampler";
			out.lossPossible = true;
		} else if (source == Source::kBuffer || out.bufferHasBegin) {
			slice = SliceFencedText(buffer, a_maxLines);
			out.source = "buffer";
		}
		out.sawBegin = slice.sawBegin;
		out.sawEnd = slice.sawEnd;
		out.lines = std::move(slice.lines);
		return out;
	}
}
