#include "DrawStats.h"
#include "DrawStatsLogic.h"

#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <array>

namespace dvb::DrawStats
{
	namespace
	{
		using namespace DrawStatsLogic;

		template <class T, std::size_t N>
		using Counters = std::array<std::atomic<T>, N>;

		struct Live
		{
			std::atomic<std::uint64_t>                                       frames{ 0 };
			std::atomic<std::uint64_t>                                       frameTicks{ 0 };
			Counters<std::uint64_t, kPassCount>                              renders{};
			Counters<std::uint64_t, kPassCount>                              renderTicks{};
			Counters<std::uint64_t, kPassCount>                              accumulateTicks{};
			Counters<std::uint64_t, kPassCount>                              casters{};
			std::array<Counters<std::uint64_t, kShaderCount>, kPassCount>    objects{};
			std::array<Counters<std::uint64_t, kCasterSizeBins>, kPassCount> casterSizes{};
			Counters<std::uint64_t, kMaxCascades>                            cascadeObjects{};
			std::array<Counters<std::uint64_t, kShaderCount>, kMaxTargets>   targetObjects{};
			Counters<std::uint64_t, kPassCount>                              gpuNs{};
			Counters<std::uint64_t, kMaxTargets>                             targetGpuNs{};
			std::atomic<std::uint64_t>                                       gpuFrameNs{ 0 };
			std::atomic<std::uint64_t>                                       gpuFrames{ 0 };
			std::atomic<std::uint64_t>                                       gpuLost{ 0 };
		};
		Live g_live;

		template <class T, std::size_t N>
		std::array<T, N> Load(const Counters<T, N>& a_from)
		{
			std::array<T, N> out{};
			for (std::size_t i = 0; i < N; ++i)
				out[i] = a_from[i].load(std::memory_order_relaxed);
			return out;
		}

		std::int64_t Now()
		{
			LARGE_INTEGER t;
			QueryPerformanceCounter(&t);
			return t.QuadPart;
		}

		double TicksPerSecond()
		{
			static const double freq = [] {
				LARGE_INTEGER f;
				QueryPerformanceFrequency(&f);
				return static_cast<double>(f.QuadPart);
			}();
			return freq;
		}

		Counts Take()
		{
			Counts s;
			s.at = static_cast<std::uint64_t>(Now());
			s.frames = g_live.frames.load(std::memory_order_relaxed);
			s.frameTicks = g_live.frameTicks.load(std::memory_order_relaxed);
			s.renders = Load(g_live.renders);
			s.renderTicks = Load(g_live.renderTicks);
			s.accumulateTicks = Load(g_live.accumulateTicks);
			s.casters = Load(g_live.casters);
			for (std::size_t p = 0; p < kPassCount; ++p) {
				s.objects[p] = Load(g_live.objects[p]);
				s.casterSizes[p] = Load(g_live.casterSizes[p]);
			}
			s.cascadeObjects = Load(g_live.cascadeObjects);
			for (std::size_t t = 0; t < kMaxTargets; ++t)
				s.targetObjects[t] = Load(g_live.targetObjects[t]);
			s.gpuNs = Load(g_live.gpuNs);
			s.targetGpuNs = Load(g_live.targetGpuNs);
			s.gpuFrameNs = g_live.gpuFrameNs.load(std::memory_order_relaxed);
			s.gpuFrames = g_live.gpuFrames.load(std::memory_order_relaxed);
			s.gpuLost = g_live.gpuLost.load(std::memory_order_relaxed);
			return s;
		}

		// GPU spans are keyed by bucket: 0..kPassCount-1 a shadow kind, kTargetBucket + n main-pass target n.
		constexpr int kTargetBucket = static_cast<int>(kPassCount);

		namespace Gpu
		{
			constexpr std::size_t kRing = 6;
			constexpr std::size_t kMaxSpans = 160;

			struct Frame
			{
				ID3D11Query*                            disjoint = nullptr;
				ID3D11Query*                            start = nullptr;
				ID3D11Query*                            stop = nullptr;
				std::array<ID3D11Query*, kMaxSpans * 2> marks{};
				std::array<int, kMaxSpans>              bucket{};
				std::size_t                             spans = 0;
				bool                                    pending = false;
				std::uint32_t                           window = 0;
			};

			std::array<Frame, kRing> g_ring{};
			std::size_t              g_cur = 0;
			ID3D11DeviceContext*     g_ctx = nullptr;
			DWORD                    g_thread = 0;
			bool                     g_ok = false;
			bool                     g_inFrame = false;
			int                      g_open = -1;
			// A frame issued before the last reset is dropped when it resolves.
			std::atomic<std::uint32_t> g_window{ 0 };

			bool Ready(ID3D11Query* a_query, std::uint64_t& a_out)
			{
				return a_query && g_ctx->GetData(a_query, &a_out, sizeof(a_out), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
			}

			void Collect(Frame& a_frame)
			{
				if (!a_frame.pending)
					return;
				a_frame.pending = false;
				if (a_frame.window != g_window.load(std::memory_order_acquire))
					return;
				D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
				std::uint64_t                       t0 = 0;
				std::uint64_t                       t1 = 0;
				if (g_ctx->GetData(a_frame.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || dj.Disjoint || dj.Frequency == 0 ||
					!Ready(a_frame.start, t0) || !Ready(a_frame.stop, t1) || t1 < t0) {
					g_live.gpuLost.fetch_add(1, std::memory_order_relaxed);
					return;
				}
				const double toNs = 1.0e9 / static_cast<double>(dj.Frequency);
				g_live.gpuFrameNs.fetch_add(static_cast<std::uint64_t>(static_cast<double>(t1 - t0) * toNs), std::memory_order_relaxed);
				g_live.gpuFrames.fetch_add(1, std::memory_order_relaxed);
				for (std::size_t i = 0; i < a_frame.spans; ++i) {
					std::uint64_t b = 0;
					std::uint64_t e = 0;
					if (!Ready(a_frame.marks[i * 2], b) || !Ready(a_frame.marks[i * 2 + 1], e) || e < b)
						continue;
					const auto ns = static_cast<std::uint64_t>(static_cast<double>(e - b) * toNs);
					const int  k = a_frame.bucket[i];
					if (k >= 0 && k < kTargetBucket)
						g_live.gpuNs[static_cast<std::size_t>(k)].fetch_add(ns, std::memory_order_relaxed);
					else if (k >= kTargetBucket && k < kTargetBucket + static_cast<int>(kMaxTargets))
						g_live.targetGpuNs[static_cast<std::size_t>(k - kTargetBucket)].fetch_add(ns, std::memory_order_relaxed);
				}
			}

			ID3D11Query* MakeQuery(ID3D11Device* a_device, D3D11_QUERY a_type)
			{
				const D3D11_QUERY_DESC desc{ a_type, 0 };
				ID3D11Query*           q = nullptr;
				return SUCCEEDED(a_device->CreateQuery(&desc, &q)) ? q : nullptr;
			}

			void ReleaseQueries()
			{
				const auto drop = [](ID3D11Query*& a_query) {
					if (a_query) {
						a_query->Release();
						a_query = nullptr;
					}
				};
				for (auto& f : g_ring) {
					drop(f.disjoint);
					drop(f.start);
					drop(f.stop);
					for (auto& m : f.marks)
						drop(m);
				}
			}

			bool Init(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
			{
				g_ctx = a_context;
				if (!a_device || !g_ctx)
					return false;
				for (auto& f : g_ring) {
					f.disjoint = MakeQuery(a_device, D3D11_QUERY_TIMESTAMP_DISJOINT);
					f.start = MakeQuery(a_device, D3D11_QUERY_TIMESTAMP);
					f.stop = MakeQuery(a_device, D3D11_QUERY_TIMESTAMP);
					for (auto& m : f.marks)
						m = MakeQuery(a_device, D3D11_QUERY_TIMESTAMP);
					if (!f.disjoint || !f.start || !f.stop || std::ranges::any_of(f.marks, [](ID3D11Query* q) { return q == nullptr; })) {
						ReleaseQueries();
						return false;
					}
				}
				g_ok = true;
				return true;
			}

			void Switch(int a_bucket)
			{
				if (!g_ok || !g_inFrame || GetCurrentThreadId() != g_thread || a_bucket == g_open)
					return;
				auto& f = g_ring[g_cur];
				if (g_open >= 0 && f.spans > 0)
					g_ctx->End(f.marks[(f.spans - 1) * 2 + 1]);
				g_open = -1;
				if (a_bucket >= 0 && f.spans < kMaxSpans) {
					f.bucket[f.spans] = a_bucket;
					g_ctx->End(f.marks[f.spans * 2]);
					++f.spans;
					g_open = a_bucket;
				}
			}

			void FrameBegin()
			{
				if (!g_ok)
					return;
				g_thread = GetCurrentThreadId();
				g_cur = (g_cur + 1) % kRing;
				auto& f = g_ring[g_cur];
				Collect(f);
				f.spans = 0;
				f.window = g_window.load(std::memory_order_acquire);
				g_ctx->Begin(f.disjoint);
				g_ctx->End(f.start);
				g_inFrame = true;
				g_open = -1;
			}

			void FrameEnd()
			{
				if (!g_ok || !g_inFrame)
					return;
				Switch(-1);
				auto& f = g_ring[g_cur];
				g_ctx->End(f.stop);
				g_ctx->End(f.disjoint);
				f.pending = true;
				g_inFrame = false;
			}
		}

		std::mutex                              g_targetLock;
		std::vector<Target>                     g_targets;
		bool                                    g_targetsDropped = false;  // guarded by g_targetLock
		std::unordered_map<const void*, int>    g_viewTarget;              // render thread only; emptied every Present, so a reused view address is looked up again
		std::pair<std::uint32_t, std::uint32_t> g_screen{ 0, 0 };
		void*                                   g_immediate = nullptr;
		std::int64_t                            g_lastPresent = 0;

		thread_local Pass t_pass = Pass::kMain;
		std::atomic<bool> g_stoodDown{ false };

		bool StoodDown() { return g_stoodDown.load(std::memory_order_relaxed); }

		thread_local Pass  t_accumulating = Pass::kMain;
		thread_local int   t_cascade = -1;
		thread_local void* t_lastDsv = nullptr;
		thread_local int   t_target = -1;
		RE::NiPoint3       g_cameraPos{};

		std::size_t Idx(Pass a_pass) { return static_cast<std::size_t>(a_pass); }

		int MainBucket() { return t_target >= 0 ? kTargetBucket + t_target : -1; }

		template <Pass P>
		struct ShadowRender
		{
			static void thunk(RE::BSShadowLight* a_light, std::uint32_t& a_index)
			{
				if (StoodDown())
					return func(a_light, a_index);
				const Pass  prevPass = t_pass;
				const int   prevCascade = t_cascade;
				void* const prevDsv = t_lastDsv;
				t_pass = P;
				t_cascade = -1;
				t_lastDsv = nullptr;
				Gpu::Switch(static_cast<int>(P));
				const auto t0 = Now();
				func(a_light, a_index);
				g_live.renderTicks[Idx(P)].fetch_add(static_cast<std::uint64_t>(Now() - t0), std::memory_order_relaxed);
				g_live.renders[Idx(P)].fetch_add(1, std::memory_order_relaxed);
				t_pass = prevPass;
				t_cascade = prevCascade;
				t_lastDsv = prevDsv;
				Gpu::Switch(prevPass != Pass::kMain ? static_cast<int>(prevPass) : MainBucket());
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		template <Pass P>
		struct ShadowAccumulate
		{
			static void thunk(RE::BSShadowLight* a_light, std::uint32_t& a_count, std::uint32_t a_channel, RE::NiAVObject* a_scene, std::uint8_t a_vr)
			{
				if (StoodDown())
					return func(a_light, a_count, a_channel, a_scene, a_vr);
				if (auto* cam = RE::PlayerCamera::GetSingleton(); cam && cam->cameraRoot)
					g_cameraPos = cam->cameraRoot->world.translate;
				const Pass prev = t_accumulating;
				t_accumulating = P;
				const auto t0 = Now();
				func(a_light, a_count, a_channel, a_scene, a_vr);
				t_accumulating = prev;
				g_live.accumulateTicks[Idx(P)].fetch_add(static_cast<std::uint64_t>(Now() - t0), std::memory_order_relaxed);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		void NoteCaster(RE::BSRenderPass* a_pass)
		{
			if (t_accumulating == Pass::kMain || !a_pass || !a_pass->geometry)
				return;
			const auto& bound = a_pass->geometry->worldBound;
			g_live.casters[Idx(t_accumulating)].fetch_add(1, std::memory_order_relaxed);
			g_live.casterSizes[Idx(t_accumulating)][CasterSizeBin(bound.radius, bound.center.GetDistance(g_cameraPos))].fetch_add(1, std::memory_order_relaxed);
		}

		template <int Slot>
		struct RegisterPass
		{
			static void thunk(RE::BSBatchRenderer* a_batch, RE::BSRenderPass* a_pass, std::uint32_t a_technique)
			{
				if (!StoodDown())
					NoteCaster(a_pass);
				func(a_batch, a_pass, a_technique);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		template <std::size_t S>
		struct SetupGeometry
		{
			static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, std::uint32_t a_flags)
			{
				if (StoodDown())
					return func(a_shader, a_pass, a_flags);
				g_live.objects[Idx(t_pass)][S].fetch_add(1, std::memory_order_relaxed);
				if (t_pass == Pass::kSun && t_cascade >= 0 && t_cascade < static_cast<int>(kMaxCascades))
					g_live.cascadeObjects[static_cast<std::size_t>(t_cascade)].fetch_add(1, std::memory_order_relaxed);
				else if (t_pass == Pass::kMain && t_target >= 0)
					g_live.targetObjects[static_cast<std::size_t>(t_target)][S].fetch_add(1, std::memory_order_relaxed);
				func(a_shader, a_pass, a_flags);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// The original is stored before the slot is written: another thread may call the hook at once.
		template <class T>
		void WriteHook(REL::Relocation<std::uintptr_t>& a_vtable, std::size_t a_index)
		{
			T::func = *reinterpret_cast<const std::uintptr_t*>(a_vtable.address() + a_index * sizeof(void*));
			a_vtable.write_vfunc(a_index, T::thunk);
		}

		template <Pass P>
		void HookShadowLight(REL::VariantID a_vtable)
		{
			REL::Relocation<std::uintptr_t> vt{ a_vtable };
			WriteHook<ShadowRender<P>>(vt, 0x0A);
			WriteHook<ShadowAccumulate<P>>(vt, 0x09);
		}

		template <std::size_t S>
		void HookShader(REL::VariantID a_vtable)
		{
			REL::Relocation<std::uintptr_t> vt{ a_vtable };
			WriteHook<SetupGeometry<S>>(vt, 0x06);
		}

		int TargetOf(ID3D11View* a_view, bool a_depth)
		{
			if (!a_view)
				return -1;
			if (const auto it = g_viewTarget.find(a_view); it != g_viewTarget.end())
				return it->second;
			Target info{};
			info.depth = a_depth;
			ID3D11Resource* res = nullptr;
			a_view->GetResource(&res);
			if (res) {
				ID3D11Texture2D* tex = nullptr;
				if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex) {
					D3D11_TEXTURE2D_DESC d{};
					tex->GetDesc(&d);
					info = { d.Width, d.Height, static_cast<std::uint32_t>(d.Format), d.ArraySize, a_depth };
					tex->Release();
				}
				res->Release();
			}
			int index = -1;
			{
				std::scoped_lock lock{ g_targetLock };
				if (const auto it = std::ranges::find(g_targets, info); it != g_targets.end())
					index = static_cast<int>(it - g_targets.begin());
				else if (g_targets.size() < kMaxTargets) {
					g_targets.push_back(info);
					index = static_cast<int>(g_targets.size()) - 1;
				} else
					g_targetsDropped = true;
			}
			g_viewTarget.emplace(a_view, index);
			return index;
		}

		struct Slot
		{
			void**           at = nullptr;
			void*            ours = nullptr;
			std::string_view name;
		};
		std::vector<Slot> g_slots;

		template <class F>
		bool SwapEntry(void* a_object, std::size_t a_index, F a_hook, F& a_original, std::string_view a_name)
		{
			auto** vtbl = *reinterpret_cast<void***>(a_object);
			DWORD  old = 0;
			a_original = reinterpret_cast<F>(vtbl[a_index]);
			if (!VirtualProtect(&vtbl[a_index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old))
				return false;
			vtbl[a_index] = reinterpret_cast<void*>(a_hook);
			VirtualProtect(&vtbl[a_index], sizeof(void*), old, &old);
			g_slots.push_back({ &vtbl[a_index], reinterpret_cast<void*>(a_hook), a_name });
			return true;
		}

		using OMSetRenderTargets_t = void (*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
		using Present_t = HRESULT (*)(IDXGISwapChain*, UINT, UINT);
		OMSetRenderTargets_t g_omSetRenderTargets = nullptr;
		Present_t            g_present = nullptr;

		void HkOMSetRenderTargets(ID3D11DeviceContext* a_ctx, UINT a_count, ID3D11RenderTargetView* const* a_rtvs, ID3D11DepthStencilView* a_dsv)
		{
			if (StoodDown())
				return g_omSetRenderTargets(a_ctx, a_count, a_rtvs, a_dsv);
			// Inside the sun's Render each newly bound depth target is the next cascade.
			if (t_pass == Pass::kSun && a_dsv && a_dsv != t_lastDsv) {
				t_lastDsv = a_dsv;
				++t_cascade;
			}
			if (t_pass == Pass::kMain && a_ctx == g_immediate) {
				const bool  depth = a_dsv != nullptr;
				ID3D11View* view = depth ? static_cast<ID3D11View*>(a_dsv) : (a_count > 0 && a_rtvs ? a_rtvs[0] : nullptr);
				t_target = TargetOf(view, depth);
				Gpu::Switch(MainBucket());
			}
			g_omSetRenderTargets(a_ctx, a_count, a_rtvs, a_dsv);
		}

		HRESULT HkPresent(IDXGISwapChain* a_swap, UINT a_sync, UINT a_flags)
		{
			if (StoodDown())
				return g_present(a_swap, a_sync, a_flags);
			Gpu::FrameEnd();
			const auto now = Now();
			if (g_lastPresent)
				g_live.frameTicks.fetch_add(static_cast<std::uint64_t>(now - g_lastPresent), std::memory_order_relaxed);
			g_lastPresent = now;
			g_live.frames.fetch_add(1, std::memory_order_relaxed);
			const HRESULT hr = g_present(a_swap, a_sync, a_flags);
			t_target = -1;
			g_viewTarget.clear();
			Gpu::FrameBegin();
			return hr;
		}

		bool HookD3D()
		{
			auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
			if (!renderer)
				return false;
			auto& data = renderer->GetRuntimeData();
			auto* ctx = reinterpret_cast<ID3D11DeviceContext*>(data.context);
			auto* device = reinterpret_cast<ID3D11Device*>(data.forwarder);
			auto* window = RE::BSGraphics::Renderer::GetCurrentRenderWindow();
			auto* swap = window ? reinterpret_cast<IDXGISwapChain*>(window->swapChain) : nullptr;
			if (!ctx || !swap)
				return false;
			g_immediate = ctx;
			if (DXGI_SWAP_CHAIN_DESC sd{}; SUCCEEDED(swap->GetDesc(&sd)))
				g_screen = { sd.BufferDesc.Width, sd.BufferDesc.Height };
			// ID3D11DeviceContext::OMSetRenderTargets is vtable slot 33, IDXGISwapChain::Present slot 8.
			if (!SwapEntry(ctx, 33, &HkOMSetRenderTargets, g_omSetRenderTargets, "OMSetRenderTargets"))
				return false;
			const bool gpu = Gpu::Init(device, ctx);
			if (!SwapEntry(swap, 8, &HkPresent, g_present, "Present"))
				return false;
			logs::info("devbench: draws hooked (GPU timer {})", gpu ? "on" : "unavailable");
			return true;
		}

		bool        g_armed = false;
		bool        g_d3dHooked = false;
		std::string g_armError;
		Counts      g_windowStart;

		void Arm()
		{
			if (g_armed || StoodDown())
				return;
			g_armed = true;
			HookShadowLight<Pass::kSun>(RE::VTABLE_BSShadowDirectionalLight[0]);
			HookShadowLight<Pass::kSpot>(RE::VTABLE_BSShadowFrustumLight[0]);
			HookShadowLight<Pass::kPoint>(RE::VTABLE_BSShadowParabolicLight[0]);
			HookShader<0>(RE::VTABLE_BSLightingShader[0]);
			HookShader<1>(RE::VTABLE_BSUtilityShader[0]);
			HookShader<2>(RE::VTABLE_BSEffectShader[0]);
			HookShader<3>(RE::VTABLE_BSGrassShader[0]);
			HookShader<4>(RE::VTABLE_BSSkyShader[0]);
			HookShader<5>(RE::VTABLE_BSWaterShader[0]);
			HookShader<6>(RE::VTABLE_BSParticleShader[0]);
			HookShader<7>(RE::VTABLE_BSBloodSplatterShader[0]);
			HookShader<8>(RE::VTABLE_BSDistantTreeShader[0]);
			REL::Relocation<std::uintptr_t> batch{ RE::VTABLE_BSBatchRenderer[0] };
			WriteHook<RegisterPass<1>>(batch, 0x01);
			WriteHook<RegisterPass<2>>(batch, 0x02);
			g_d3dHooked = HookD3D();
			if (!g_d3dHooked)
				g_armError = "no D3D11 context or swap chain to hook: render targets, frame times and GPU time are not measured";
			g_windowStart = Take();
		}

		json Report(const Counts& a_from, const Counts& a_to)
		{
			std::vector<Target> targets;
			bool                dropped = false;
			{
				std::scoped_lock lock{ g_targetLock };
				targets = g_targets;
				dropped = g_targetsDropped;
			}
			json out = Reading(a_from, a_to, targets, dropped, g_screen.first, g_screen.second, Gpu::g_ok, TicksPerSecond());
			json lost = json::array();
			for (const auto& s : g_slots)
				if (*s.at != s.ours)
					lost.push_back(std::string(s.name));
			out["hooksReplaced"] = std::move(lost);
			if (!g_armError.empty())
				out["error"] = g_armError;
			return out;
		}
	}

	void StandDown()
	{
		g_stoodDown.store(true, std::memory_order_relaxed);
	}

	json Read(bool a_reset)
	{
		if (!g_armed) {
			Arm();
			json out = Report(g_windowStart, g_windowStart);
			out["started"] = true;
			return out;
		}
		if (a_reset)
			Gpu::g_window.fetch_add(1, std::memory_order_acq_rel);
		const Counts now = Take();
		json         out = Report(g_windowStart, now);
		if (a_reset)
			g_windowStart = now;
		return out;
	}
}
