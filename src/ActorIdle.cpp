#include "ActorIdle.h"

#include "ActorIdleLogic.h"
#include "ToolRegistry.h"

#include <RE/A/AIProcess.h>
#include <RE/A/Actor.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESIdleForm.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

namespace dvb::ActorIdle
{
	namespace
	{
		// How long the AI stays on after a play or stop before a requested freeze: PlayIdle only animates while it runs.
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

		std::vector<Entry> g_entries;  // main thread
		bool               g_ticking{ false };

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

		void Require(RE::Actor* a_actor)
		{
			if (!a_actor)
				throw ToolError(409, "not an actor");
			if (a_actor->IsPlayerRef())
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

		// PlayIdle only animates while the AI runs; the do-nothing package keeps the actor where it stands.
		a_actor->InitiateDoNothingPackage();
		a_actor->EnableAI(true);
		const bool ok = ProcessOf(a_actor)->PlayIdle(a_actor, a_idle, nullptr);

		if (ok) {
			Drop(a_actor->GetFormID());
			const auto now = NowMs();
			// A frozen actor cannot be replayed, so a loop keeps the AI on and a_freezeAi applies to a single play only.
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

		// The data handler's per-type array does not hold idles, so walk every form.
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
		Require(a_actor);
		json        out = Describe(a_actor);
		const auto  it = std::ranges::find(g_entries, a_actor->GetFormID(), &Entry::actor);
		const auto* idle = it != g_entries.end() && it->idle ? RE::TESForm::LookupByID<RE::TESIdleForm>(it->idle) : nullptr;
		out["looping"] = idle != nullptr;
		out["loopIdle"] = idle ? json(idle->GetFormEditorID()) : json(nullptr);
		return out;
	}

	void EndSession()
	{
		g_entries.clear();
	}
}
