#include "ConsoleLogCapture.h"

#include "GameState.h"
#include "MainThread.h"
#include "PrologueScan.h"
#include "ToolRegistry.h"

#include <SKSE/ContextHook.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
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
			kPrint,
			kBuffer,
			kSampler,
		};

		// Sampler state and the buffer baseline are main thread only.
		LineSampler         g_sampler;
		std::size_t         g_bufferBaseline = 0;
		int                 g_lastFrame = -1;
		std::size_t         g_engineFrames = 0;
		std::atomic<Source> g_source{ Source::kNone };
		std::atomic<bool>   g_timedOut{ false };
		std::mutex          g_captureMutex;

		// The print hook runs on whatever thread prints, so its collector sits behind a lock and it
		// does nothing outside a capture.
		std::atomic<bool> g_printHooked{ false };
		std::atomic<bool> g_printCapturing{ false };
		std::mutex        g_printMutex;
		PrintCollector    g_printed;
		// Prints the detour could not format or keep; reset with the collector.
		std::atomic<std::size_t> g_printFailures{ 0 };

		// ConsoleLog::VPrint(this, fmt, va_list) at entry: rdx is the format, r8 the argument list.
		void PrintDetour(CONTEXT& a_ctx)
		{
			if (!g_printCapturing.load(std::memory_order_acquire))
				return;
			try {
				const auto* fmt = reinterpret_cast<const char*>(a_ctx.Rdx);
				if (!fmt)
					return;
				const auto   args = reinterpret_cast<std::va_list>(a_ctx.R8);
				char         head[1024];
				std::va_list first;
				va_copy(first, args);
				const int n = std::vsnprintf(head, sizeof(head), fmt, first);
				va_end(first);
				if (n < 0) {
					g_printFailures.fetch_add(1, std::memory_order_relaxed);
					return;
				}
				std::string text;
				if (static_cast<std::size_t>(n) < sizeof(head)) {
					text.assign(head, static_cast<std::size_t>(n));
				} else {
					text.resize(static_cast<std::size_t>(n) + 1);
					std::va_list second;
					va_copy(second, args);
					const int again = std::vsnprintf(text.data(), text.size(), fmt, second);
					va_end(second);
					if (again != n) {
						g_printFailures.fetch_add(1, std::memory_order_relaxed);
						return;
					}
					text.resize(static_cast<std::size_t>(n));
				}
				std::lock_guard<std::mutex> lk(g_printMutex);
				g_printed.Feed(text);
			} catch (...) {
				g_printFailures.fetch_add(1, std::memory_order_relaxed);
			}
		}

		struct PrintCaptureWindow
		{
			PrintCaptureWindow()
			{
				{
					std::lock_guard<std::mutex> lk(g_printMutex);
					g_printed.Reset();
				}
				g_printFailures.store(0, std::memory_order_relaxed);
				g_printCapturing.store(g_printHooked.load(), std::memory_order_release);
			}
			~PrintCaptureWindow() { g_printCapturing.store(false, std::memory_order_release); }
		};

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

		// A buffer that shrank below the baseline was drained, so all of it is newer.
		std::size_t BufferFromOffset(std::size_t a_bufferSize)
		{
			return a_bufferSize < g_bufferBaseline ? 0 : g_bufferBaseline;
		}

		struct LookView
		{
			LineSampler::Seen seen = LineSampler::Seen::kNothing;
			bool              printSawBegin = false;
			bool              printSawEnd = false;
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
			{
				std::lock_guard<std::mutex> lk(g_printMutex);
				view.printSawBegin = g_printed.SawBegin();
				view.printSawEnd = g_printed.SawEnd();
			}
			view.seen = g_sampler.Observe(CurrentLine());
			view.samplerSawBegin = g_sampler.SawBegin();
			view.samplerSawEnd = g_sampler.SawEnd();
			const auto buffer = BufferText();
			const auto fence = FindFence(buffer, BufferFromOffset(buffer.size()));
			view.bufferHasBegin = fence.hasBegin;
			view.bufferHasEnd = fence.hasEnd;
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
			bool          printed = false;
			WaitFor(a_deadline, kBeginLooks, [&](const LookView& v) {
				if (v.printSawBegin) {
					printed = true;
					return true;
				}
				choice = chooser.Look(v.bufferHasBegin, v.samplerSawBegin);
				return choice != SourceChooser::Choice::kUndecided;
			});
			if (printed)
				return Source::kPrint;
			switch (choice) {
			case SourceChooser::Choice::kBuffer:
				return Source::kBuffer;
			case SourceChooser::Choice::kSampler:
				return Source::kSampler;
			default:
				return Source::kNone;
			}
		}

		bool CaptureFromPrint(const std::string& a_command, Clock::time_point a_deadline)
		{
			QueueThenEndMarker(a_command);
			return WaitFor(a_deadline, kEndLooks, [](const LookView& v) { return v.printSawEnd; });
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

	void InstallPrintHook()
	{
		if (g_printHooked.load())
			return;
		REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(50180, 51110) };
		const auto*                     code = reinterpret_cast<const std::uint8_t*>(target.address());
		const std::size_t               length = SafePrologueLength(std::span<const std::uint8_t>(code, 32));
		if (length == 0 || length > 16) {
			std::string bytes;
			for (std::size_t i = 0; i < 16; ++i)
				bytes += std::format("{:02X} ", code[i]);
			logs::warn("devbench: console print hook not installed (ConsoleLog::VPrint starts {}); captures use the buffer or sampler", bytes);
			return;
		}
		// Negative include: the detour sees the registers as the caller left them, then the
		// copied prologue runs and execution resumes after it.
		if (!SKSE::stl::install_context_hook(target.address(), static_cast<int>(length), &PrintDetour, -static_cast<int>(length))) {
			logs::error("devbench: failed to install console print hook");
			return;
		}
		g_printHooked.store(true);
		logs::info("devbench: console print hook installed ({} byte prologue)", length);
	}

	bool RunFencedCapture(const std::string& a_command)
	{
		std::unique_lock<std::mutex> owned(g_captureMutex, std::try_to_lock);
		if (!owned.owns_lock())
			throw ToolError(409, "a console capture is already running; retry when it finishes");

		g_source.store(Source::kNone);
		g_timedOut.store(false);
		MainThread::RunAndWait([]() -> json {
			g_sampler.Reset(CurrentLine());
			g_bufferBaseline = BufferText().size();
			g_lastFrame = -1;
			g_engineFrames = 0;
			return true;
		},
			kLookTimeout);

		const PrintCaptureWindow window;
		const auto               deadline = Clock::now() + kCaptureDeadline;
		const auto               source = ChooseSource(deadline);
		if (source == Source::kNone) {
			g_timedOut.store(true);
			throw ToolError(504, "console capture never saw its begin marker; the command was not run");
		}
		g_source.store(source);

		bool finished = false;
		if (source == Source::kPrint)
			finished = CaptureFromPrint(a_command, deadline);
		else if (source == Source::kBuffer)
			finished = CaptureFromBuffer(a_command, deadline);
		else
			finished = CaptureFromSampler(a_command, deadline);
		if (!finished) {
			logs::warn("devbench: console capture did not see its end marker");
			g_timedOut.store(true);
		}
		return finished;
	}

	Result ReadFenced(std::size_t a_maxLines)
	{
		Result out;
		out.timedOut = g_timedOut.load();
		out.ringLines = g_sampler.Lines().size();
		out.samples = g_sampler.Samples();
		out.ticks = g_sampler.Ticks();
		out.engineFrames = g_engineFrames;
		out.printHooked = g_printHooked.load();

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
		if (source == Source::kPrint) {
			std::lock_guard<std::mutex> lk(g_printMutex);
			slice = SliceFencedLines(g_printed.Lines(), a_maxLines);
			out.printLines = g_printed.Lines().size();
			out.printDropped = g_printed.Dropped();
			out.printFailures = g_printFailures.load(std::memory_order_relaxed);
			out.source = "print";
			out.lossPossible = out.printDropped > 0 || out.printFailures > 0;
		} else if (source == Source::kSampler) {
			slice = SliceFencedLines(g_sampler.Lines(), a_maxLines);
			out.source = "sampler";
			out.lossPossible = true;
		} else if (source == Source::kBuffer || out.bufferHasBegin) {
			slice = SliceFencedText(buffer, a_maxLines, BufferFromOffset(buffer.size()));
			out.source = "buffer";
		}
		out.sawBegin = slice.sawBegin;
		out.sawEnd = slice.sawEnd;
		out.lines = std::move(slice.lines);
		return out;
	}
}
