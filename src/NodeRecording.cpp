#include "NodeRecording.h"

#include "FormLookup.h"
#include "GameState.h"
#include "MainThread.h"
#include "NodeRecordingLogic.h"
#include "ToolRegistry.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace dvb::NodeRecording
{
	namespace
	{
		constexpr auto kPacerRetryDelay = std::chrono::milliseconds(1);

		using Transform = std::array<float, 13>;

		struct Frame
		{
			int                                   frame;
			long long                             tMs;
			std::vector<std::optional<Transform>> nodes;
		};

		class Sampler
		{
		public:
			Sampler(RE::FormID a_formId, std::vector<std::string> a_names) :
				m_formId(a_formId), m_names(std::move(a_names)), m_start(std::chrono::steady_clock::now()),
				m_startFrame(game::CurrentFrame())
			{}

			void Begin()
			{
				m_pacer = std::thread([this]() {
					for (;;) {
						{
							std::unique_lock lock(m_wakeMutex);
							m_wakeCv.wait(lock, [this]() { return m_wake || m_cancelled.load(); });
							if (m_cancelled.load())
								return;
							m_wake = false;
						}
						std::this_thread::sleep_for(kPacerRetryDelay);
						if (!m_cancelled.load())
							Schedule();
					}
				});
				Schedule();
			}

			void End()
			{
				{
					std::lock_guard lock(m_wakeMutex);
					m_cancelled.store(true);
				}
				m_wakeCv.notify_all();
				if (m_pacer.joinable())
					m_pacer.join();
			}

			std::size_t FrameCount() const
			{
				std::lock_guard lock(m_framesMutex);
				return m_frames.size();
			}

			bool                            Truncated() const { return m_truncated.load(); }
			const std::vector<std::string>& Names() const { return m_names; }
			int                             StartFrame() const { return m_startFrame; }
			RE::FormID                      FormId() const { return m_formId; }

			std::vector<Frame> TakeFrames()
			{
				std::lock_guard lock(m_framesMutex);
				return std::move(m_frames);
			}

		private:
			void Schedule()
			{
				if (auto* task = SKSE::GetTaskInterface())
					task->AddTask([this]() { Tick(); });
			}

			void WakePacer()
			{
				{
					std::lock_guard lock(m_wakeMutex);
					m_wake = true;
				}
				m_wakeCv.notify_one();
			}

			void Tick()
			{
				if (m_cancelled.load(std::memory_order_relaxed))
					return;
				const int frame = game::CurrentFrame();
				if (frame != m_lastFrame && !m_truncated.load()) {
					m_lastFrame = frame;
					Sample(frame);
				}
				WakePacer();
			}

			void Sample(int a_frame)
			{
				auto* form = RE::TESForm::LookupByID(m_formId);
				auto* ref = form ? form->AsReference() : nullptr;
				auto* root = ref ? ref->Get3D() : nullptr;
				if (!root)
					return;
				Frame f{ a_frame,
					std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_start).count(),
					{} };
				f.nodes.reserve(m_names.size());
				for (const auto& name : m_names) {
					auto* node = root->GetObjectByName(name);
					if (!node) {
						f.nodes.emplace_back(std::nullopt);
						continue;
					}
					const auto& w = node->world;
					Transform   t{ w.translate.x, w.translate.y, w.translate.z };
					for (std::size_t r = 0; r < 3; ++r)
						for (std::size_t c = 0; c < 3; ++c)
							t[3 + r * 3 + c] = w.rotate.entry[r][c];
					t[12] = w.scale;
					f.nodes.emplace_back(t);
				}
				std::lock_guard lock(m_framesMutex);
				if (m_frames.size() >= NodeRecordingLogic::kMaxFrames) {
					m_truncated.store(true);
					return;
				}
				m_frames.push_back(std::move(f));
			}

			const RE::FormID                            m_formId;
			const std::vector<std::string>              m_names;
			const std::chrono::steady_clock::time_point m_start;
			const int                                   m_startFrame;
			int                                         m_lastFrame = -2;  // main thread only
			std::atomic<bool>                           m_cancelled{ false };
			std::atomic<bool>                           m_truncated{ false };
			mutable std::mutex                          m_framesMutex;
			std::vector<Frame>                          m_frames;
			std::mutex                                  m_wakeMutex;
			std::condition_variable                     m_wakeCv;
			bool                                        m_wake = false;
			std::thread                                 m_pacer;
		};

		std::mutex               g_mutex;
		std::unique_ptr<Sampler> g_sampler;
	}

	void Start(const std::string& a_formId, std::vector<std::string> a_names)
	{
		const std::string arg = a_formId.empty() ? "0x14" : a_formId;
		const auto        formId = MainThread::RunAndWait([&]() -> json {
			auto* form = FormLookup::LookupFormArg(arg);
			auto* ref = form ? form->AsReference() : nullptr;
			if (!ref)
				throw ToolError(404, std::format("record nodes: '{}' is not a placed reference", arg));
			if (!ref->Get3D())
				throw ToolError(409, std::format("record nodes: '{}' has no loaded 3D", arg));
			return json(ref->GetFormID());
		});
		std::lock_guard   lock(g_mutex);
		if (g_sampler)
			throw ToolError(409, "a node recording is already running");
		g_sampler = std::make_unique<Sampler>(formId.get<RE::FormID>(), std::move(a_names));
		g_sampler->Begin();
	}

	bool IsActive()
	{
		std::lock_guard lock(g_mutex);
		return g_sampler != nullptr;
	}

	void Cancel()
	{
		std::unique_ptr<Sampler> sampler;
		{
			std::lock_guard lock(g_mutex);
			sampler = std::move(g_sampler);
		}
		if (sampler)
			sampler->End();
	}

	json StopAndWrite(const std::filesystem::path& a_path)
	{
		std::unique_ptr<Sampler> sampler;
		{
			std::lock_guard lock(g_mutex);
			sampler = std::move(g_sampler);
		}
		if (!sampler)
			return json{ { "frames", 0 } };
		sampler->End();
		const auto frames = sampler->TakeFrames();
		const bool truncated = sampler->Truncated();

		std::ofstream out(a_path, std::ios::binary | std::ios::trunc);
		if (!out)
			throw std::runtime_error(std::format("cannot write {}", a_path.string()));
		out << json{ { "format", "devbench-node-frames-1" }, { "formId", std::format("{:08X}", sampler->FormId()) },
			{ "nodes", sampler->Names() }, { "startFrame", sampler->StartFrame() },
			{ "layout", "per node: [tx,ty,tz, r00,r01,r02,r10,r11,r12,r20,r21,r22, scale] world space, or null when the node was absent" } }
				   .dump()
		    << '\n';
		for (const auto& f : frames) {
			json nodes = json::array();
			for (const auto& n : f.nodes)
				nodes.push_back(n ? json(*n) : json(nullptr));
			out << json{ { "frame", f.frame }, { "tMs", f.tMs }, { "nodes", std::move(nodes) } }.dump() << '\n';
		}
		out.flush();
		if (!out)
			throw std::runtime_error(std::format("write failed: {}", a_path.string()));
		return json{ { "path", a_path.generic_string() }, { "frames", frames.size() },
			{ "nodes", sampler->Names().size() }, { "truncated", truncated } };
	}

	json Status()
	{
		std::lock_guard lock(g_mutex);
		if (!g_sampler)
			return json{ { "active", false } };
		return json{ { "active", true }, { "frames", g_sampler->FrameCount() },
			{ "nodes", g_sampler->Names().size() }, { "truncated", g_sampler->Truncated() } };
	}
}
