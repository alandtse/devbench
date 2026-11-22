#include "ActorIdle.h"

#include "ActorIdleLogic.h"
#include "ToolRegistry.h"

#include <RE/A/AIProcess.h>
#include <RE/A/Actor.h>
#include <RE/P/PlayerCamera.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESFile.h>
#include <RE/T/TESIdleForm.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

namespace dvb::ActorIdle
{
	namespace
	{
		constexpr std::int64_t              kSettleMs = 1500;
		constexpr std::chrono::milliseconds kTickInterval{ 100 };

		struct Entry
		{
			RE::FormID   actor{ 0 };
			RE::FormID   idle{ 0 };  // 0: no loop, only a pending freeze
			std::int64_t lastPlayMs{ 0 };
			std::int64_t repeatMs{ 0 };
			std::int64_t freezeAtMs{ -1 };  // -1: none
		};

		struct Locomotion
		{
			RE::FormID    actor{ 0 };
			float         speed{ 0.0f };
			bool          sprint{ false };
			std::uint64_t speedPolls{ 0 };
		};

		std::vector<Entry> g_entries;  // main thread
		bool               g_ticking{ false };
		bool               g_locomotionHooked{ false };

		// The graph polls channels off the main thread, so the in-place runs are guarded; the count lets the hook skip the lock
		// when none is active.
		std::mutex               g_locomotionLock;
		std::vector<Locomotion>  g_locomotion;
		std::atomic<std::size_t> g_locomotionCount{ 0 };

		std::int64_t NowMs()
		{
			using namespace std::chrono;
			return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
		}

		RE::AIProcess* ProcessOf(RE::Actor* a_actor)
		{
			return a_actor ? a_actor->GetActorRuntimeData().currentProcess : nullptr;
		}

		bool Playable(RE::Actor* a_actor)
		{
			return a_actor && !a_actor->IsPlayerRef() && !a_actor->IsDead() && a_actor->Is3DLoaded() && ProcessOf(a_actor);
		}

		void Drop(RE::FormID a_actor)
		{
			std::erase_if(g_entries, [&](const Entry& e) { return e.actor == a_actor; });
		}

		// The graph's Speed variable is rebuilt every update by a data channel, which caches the value it polls in a float at
		// +0x18. Overwriting that float after the channel's PollChannelUpdateImpl (virtual 1) is what the graph then reads; the
		// actor pointer is at +0x28.
		constexpr std::size_t kPollSlot = 1;
		constexpr std::size_t kChannelValueOffset = 0x18;
		constexpr std::size_t kChannelActorOffset = 0x28;

		// Caller holds g_locomotionLock.
		Locomotion* FindLocomotion(RE::FormID a_actor)
		{
			const auto it = std::ranges::find(g_locomotion, a_actor, &Locomotion::actor);
			return it != g_locomotion.end() ? &*it : nullptr;
		}

		struct SpeedPollHook
		{
			static void thunk(void* a_channel, std::uintptr_t a_arg)
			{
				func(a_channel, a_arg);
				if (g_locomotionCount.load(std::memory_order_acquire) == 0)
					return;
				const auto  base = reinterpret_cast<std::uintptr_t>(a_channel);
				auto* actor = *reinterpret_cast<RE::Actor**>(base + kChannelActorOffset);
				if (!actor)
					return;
				bool sprint = false;
				{
					const std::lock_guard lock{ g_locomotionLock };
					auto*                 loco = FindLocomotion(actor->GetFormID());
					if (!loco)
						return;
					*reinterpret_cast<float*>(base + kChannelValueOffset) = loco->speed;
					++loco->speedPolls;
					sprint = loco->sprint;
				}
				// The player's own update rewrites the sprint variables from its input state, so they are set again on every poll.
				if (sprint) {
					actor->SetGraphVariableBool("IsSprinting", true);
					actor->SetGraphVariableInt("iSyncSprintState", 1);
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// Offset of the speed channel's vtable by runtime; only the AE builds that have been analysed.
		struct ChannelVtable
		{
			REL::Version  version;
			std::uint64_t speed;
		};
		constexpr ChannelVtable kChannelVtables[] = {
			{ REL::Version{ 1, 6, 1170, 0 }, 0x189E568 },
			{ REL::Version{ 1, 7, 104, 0 }, 0x191C240 },
		};

		/// False when this runtime's channel vtables are not known.
		bool InstallLocomotionHook()
		{
			if (g_locomotionHooked)
				return true;
			const auto version = REL::Module::get().version();
			const auto it = std::ranges::find(kChannelVtables, version, &ChannelVtable::version);
			if (it == std::end(kChannelVtables))
				return false;
			REL::Relocation<std::uintptr_t> speedVtbl{ REL::Offset(it->speed) };
			SpeedPollHook::func = speedVtbl.write_vfunc(kPollSlot, SpeedPollHook::thunk);
			g_locomotionHooked = true;
			return true;
		}

		/// Returns whether an in-place run was removed.
		bool DropLocomotion(RE::FormID a_actor)
		{
			const std::lock_guard lock{ g_locomotionLock };
			const auto            removed = std::erase_if(g_locomotion, [&](const Locomotion& l) { return l.actor == a_actor; });
			g_locomotionCount.store(g_locomotion.size(), std::memory_order_release);
			return removed != 0;
		}

		void AddLocomotion(RE::FormID a_actor, float a_speed, bool a_sprint)
		{
			const std::lock_guard lock{ g_locomotionLock };
			g_locomotion.push_back({ a_actor, a_speed, a_sprint });
			g_locomotionCount.store(g_locomotion.size(), std::memory_order_release);
		}

		void Tick();

		// SKSE drains its task queue until it is empty, so a task that queues itself spins the main thread forever: a
		// short-lived thread waits one interval, then queues the next tick.
		void Schedule()
		{
			if (g_ticking || g_entries.empty())
				return;
			g_ticking = true;
			std::thread([] {
				std::this_thread::sleep_for(kTickInterval);
				SKSE::GetTaskInterface()->AddTask(Tick);
			}).detach();
		}

		void Tick()
		{
			g_ticking = false;
			const auto now = NowMs();
			for (auto it = g_entries.begin(); it != g_entries.end();) {
				auto* actor = RE::TESForm::LookupByID<RE::Actor>(it->actor);
				if (!Playable(actor)) {
					it = g_entries.erase(it);
					continue;
				}
				auto* process = ProcessOf(actor);
				if (it->idle) {
					auto* idle = RE::TESForm::LookupByID<RE::TESIdleForm>(it->idle);
					if (!idle) {
						it = g_entries.erase(it);
						continue;
					}
					if (ActorIdleLogic::ReplayDue(now, it->lastPlayMs, it->repeatMs)) {
						process->PlayIdle(actor, idle, nullptr);
						it->lastPlayMs = now;
					}
				}
				if (it->freezeAtMs >= 0 && now >= it->freezeAtMs) {
					actor->EnableAI(false);
					it->freezeAtMs = -1;
					if (!it->idle) {
						it = g_entries.erase(it);
						continue;
					}
				}
				++it;
			}
			Schedule();
		}

		json Describe(RE::Actor* a_actor)
		{
			return json{ { "ref", std::format("0x{:08X}", a_actor->GetFormID()) } };
		}

		void Require(RE::Actor* a_actor, bool a_allowPlayer = false)
		{
			if (!a_actor)
				throw ToolError(409, "not an actor");
			if (a_actor->IsPlayerRef() && !a_allowPlayer)
				throw ToolError(409, "the player is not driven by idle control");
			if (a_actor->IsDead())
				throw ToolError(409, "the actor is dead");
			if (!a_actor->Is3DLoaded() || !ProcessOf(a_actor))
				throw ToolError(409, "the actor is not loaded (no 3D or AI process) — move the player near it");
		}
	}

	json Play(RE::Actor* a_actor, RE::TESIdleForm* a_idle, bool a_loop, std::int64_t a_repeatMs, bool a_freezeAi)
	{
		Require(a_actor);
		if (!a_idle)
			throw ToolError(404, "idle not found");

		DropLocomotion(a_actor->GetFormID());
		// Do not drop EnableAI(true): PlayIdle does not animate while the actor's AI is off.
		a_actor->InitiateDoNothingPackage();
		a_actor->EnableAI(true);
		const bool ok = ProcessOf(a_actor)->PlayIdle(a_actor, a_idle, nullptr);

		if (ok) {
			Drop(a_actor->GetFormID());
			const auto now = NowMs();
			if (a_loop || a_freezeAi)
				g_entries.push_back({ a_actor->GetFormID(), a_loop ? a_idle->GetFormID() : 0, now, a_repeatMs, !a_loop && a_freezeAi ? now + kSettleMs : -1 });
			Schedule();
		}

		json out = Describe(a_actor);
		out["ok"] = ok;
		out["idle"] = a_idle->GetFormEditorID();
		out["looping"] = ok && a_loop;
		out["aiFrozen"] = ok && !a_loop && a_freezeAi;
		if (!ok)
			out["error"] = "PlayIdle refused the idle (not usable by this actor right now — see listIdles)";
		return out;
	}

	json Stop(RE::Actor* a_actor, bool a_freezeAi)
	{
		Require(a_actor);
		Drop(a_actor->GetFormID());
		DropLocomotion(a_actor->GetFormID());

		a_actor->EnableAI(true);
		auto* process = ProcessOf(a_actor);
		process->StopCurrentIdle(a_actor, true);
		auto*      reset = RE::TESForm::LookupByEditorID<RE::TESIdleForm>("ResetRoot");
		const bool reverted = reset ? process->PlayIdle(a_actor, reset, nullptr) : false;
		if (a_freezeAi) {
			g_entries.push_back({ a_actor->GetFormID(), 0, NowMs(), 0, NowMs() + kSettleMs });
			Schedule();
		}

		json out = Describe(a_actor);
		out["ok"] = true;
		out["reverted"] = reverted;
		out["aiFrozen"] = a_freezeAi;
		if (!reset)
			out["note"] = "ResetRoot idle not found; the current idle was stopped but no neutral pose was forced";
		return out;
	}

	namespace
	{
		// The sprint start and stop actions set iSyncSprintState and the sprint anim event; the graph variable IsSprinting
		// mirrors it.
		void SetSprintState(RE::Actor* a_actor, bool a_on)
		{
			a_actor->SetGraphVariableBool("IsSprinting", a_on);
			a_actor->SetGraphVariableInt("iSyncSprintState", a_on ? 1 : 0);
			a_actor->NotifyAnimationGraph(a_on ? "sprintStart" : "sprintStop");
		}
	}

	json StartLocomotion(RE::Actor* a_actor, float a_speed, bool a_sprint)
	{
		Require(a_actor, true);
		const bool player = a_actor->IsPlayerRef();
		if (player) {
			const auto* camera = RE::PlayerCamera::GetSingleton();
			if (!camera || camera->IsInFirstPerson())
				throw ToolError(409, "the player runs in place only outside first person: switch with camera setPov third");
		}
		if (REL::Module::IsVR())
			throw ToolError(501, "in-place locomotion is not supported on Skyrim VR yet");
		if (!InstallLocomotionHook())
			throw ToolError(501, "in-place locomotion is not supported on this game version");
		Drop(a_actor->GetFormID());
		DropLocomotion(a_actor->GetFormID());

		if (!player) {
			a_actor->InitiateDoNothingPackage();
			a_actor->EnableAI(true);
		}
		AddLocomotion(a_actor->GetFormID(), a_speed, a_sprint);
		const bool notified = a_actor->NotifyAnimationGraph("moveStart");
		if (a_sprint)
			SetSprintState(a_actor, true);

		json out = Describe(a_actor);
		out["ok"] = true;
		out["speed"] = a_speed;
		out["sprint"] = a_sprint;
		out["moveStartAccepted"] = notified;
		return out;
	}

	json StopLocomotion(RE::Actor* a_actor)
	{
		Require(a_actor, true);
		const bool wasRunning = DropLocomotion(a_actor->GetFormID());
		SetSprintState(a_actor, false);
		a_actor->NotifyAnimationGraph("moveStop");

		json out = Describe(a_actor);
		out["ok"] = true;
		out["wasRunning"] = wasRunning;
		return out;
	}

	json List(RE::Actor* a_actor, const std::string& a_filter, std::size_t a_limit)
	{
		Require(a_actor);
		auto lower = [](std::string s) {
			std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return s;
		};
		const auto filter = lower(a_filter);

		json        idles = json::array();
		std::size_t total = 0, scanned = 0, unnamed = 0, unusable = 0, failedConditions = 0;

		std::vector<RE::TESIdleForm*> all;
		{
			const auto& [map, lock] = RE::TESForm::GetAllForms();
			const RE::BSReadLockGuard guard{ lock };
			if (map)
				for (const auto& [id, form] : *map)
					if (form && form->Is(RE::FormType::Idle))
						all.push_back(static_cast<RE::TESIdleForm*>(form));
		}
		std::ranges::sort(all, {}, &RE::TESIdleForm::formID);

		for (auto* idle : all) {
			++scanned;
			const char* edid = idle->GetFormEditorID();
			if (!edid || !*edid) {
				++unnamed;
				continue;
			}
			if (!filter.empty() && lower(edid).find(filter) == std::string::npos)
				continue;
			if (!a_actor->CanUseIdle(idle)) {
				++unusable;
				continue;
			}
			if (!idle->CheckConditions(a_actor, nullptr, false)) {
				++failedConditions;
				continue;
			}
			++total;
			if (idles.size() < a_limit) {
				const char* event = idle->animEventName.c_str();
				idles.push_back(json{
					{ "editorId", edid },
					{ "formId", std::format("0x{:08X}", idle->GetFormID()) },
					{ "animEvent", event ? event : "" },
					{ "animFile", idle->animFileName.c_str() ? idle->animFileName.c_str() : "" },
					{ "plugin", idle->GetFile(0) ? std::string(idle->GetFile(0)->GetFilename()) : std::string{} },
					{ "loopMin", idle->data.loopMin },
					{ "loopMax", idle->data.loopMax },
					{ "replayDelay", idle->data.replayDelay } });
			}
		}
		json out = Describe(a_actor);
		out["total"] = total;
		out["scanned"] = scanned;
		out["withoutEditorId"] = unnamed;
		out["cannotUse"] = unusable;
		out["failedConditions"] = failedConditions;
		out["returned"] = idles.size();
		out["idles"] = std::move(idles);
		return out;
	}

	json Status(RE::Actor* a_actor)
	{
		Require(a_actor, true);
		json        out = Describe(a_actor);
		const auto  it = std::ranges::find(g_entries, a_actor->GetFormID(), &Entry::actor);
		const auto* idle = it != g_entries.end() && it->idle ? RE::TESForm::LookupByID<RE::TESIdleForm>(it->idle) : nullptr;
		out["looping"] = idle != nullptr;
		out["loopIdle"] = idle ? json(idle->GetFormEditorID()) : json(nullptr);
		const std::lock_guard lock{ g_locomotionLock };
		const auto*           loco = FindLocomotion(a_actor->GetFormID());
		if (loco) {
			bool         sprintVariable = false;
			std::int32_t sprintSync = 0;
			a_actor->GetGraphVariableBool("IsSprinting", sprintVariable);
			a_actor->GetGraphVariableInt("iSyncSprintState", sprintSync);
			out["locomotion"] = json{ { "speed", loco->speed }, { "sprint", loco->sprint }, { "speedPolls", loco->speedPolls }, { "graphIsSprinting", sprintVariable }, { "graphSyncSprintState", sprintSync } };
		} else {
			out["locomotion"] = nullptr;
		}
		return out;
	}

	void EndSession()
	{
		g_entries.clear();
		const std::lock_guard lock{ g_locomotionLock };
		g_locomotion.clear();
		g_locomotionCount.store(0, std::memory_order_release);
	}
}
