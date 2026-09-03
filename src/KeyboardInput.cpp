#include "KeyboardInput.h"

#include "EventBus.h"
#include "GameState.h"
#include "KeyboardInputState.h"
#include "MainThread.h"
#include "ToolRegistry.h"
#include "VRInput.h"
#include "VRInputState.h"

#include <RE/B/BSInputEventQueue.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace dvb
{
	namespace
	{
		using namespace std::chrono;

		constexpr int         kKeyboardContractVersion = 1;
		constexpr int         kDefaultTapMs = 50;
		constexpr int         kDefaultMaxHoldMs = 5000;
		constexpr int         kMaximumMaxHoldMs = 60000;
		constexpr std::size_t kMaximumHeldKeys = 8;
		constexpr std::size_t kMaximumSequenceEvents = 128;
		constexpr int         kMaximumSequenceMs = 30000;
		constexpr int         kReleaseAttempts = 5;
		constexpr auto        kReleaseRetryDelay = milliseconds(100);
		constexpr auto        kWatchdogRetryDelay = seconds(1);

		std::atomic<bool> g_inputReady{ false };

		std::int64_t NowMs()
		{
			return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
		}

		int BoundedInteger(const json& a_object, const char* a_name, int a_default, int a_min, int a_max)
		{
			try {
				return static_cast<int>(ParseBoundedIntegerArgument(
					a_object, a_name, a_default, a_min, a_max));
			} catch (const std::invalid_argument& e) {
				throw ToolError(400, e.what());
			}
		}

		bool BooleanArgument(const json& a_object, const char* a_name, bool a_default)
		{
			if (!a_object.contains(a_name))
				return a_default;
			if (!a_object[a_name].is_boolean())
				throw ToolError(400, std::format("'{}' must be a boolean", a_name));
			return a_object[a_name].get<bool>();
		}

		KeyboardKey ParseKey(const json& a_args)
		{
			if (!a_args.contains("key"))
				throw ToolError(400, "keyboard action requires 'key' (a documented name or DirectInput scancode)");
			std::optional<KeyboardKey> key;
			if (a_args["key"].is_string())
				key = ResolveKeyboardKey(a_args["key"].get<std::string>());
			else if (a_args["key"].is_number_integer()) {
				const auto value = BoundedInteger(a_args, "key", 0, 0,
					std::numeric_limits<std::uint16_t>::max());
				key = ResolveKeyboardKey(value);
			} else
				throw ToolError(400, "'key' must be a string name or integer DirectInput scancode");
			if (!key)
				throw ToolError(400, std::format("unknown/invalid keyboard key {} — use action='capabilities' for names and scan codes", a_args["key"].dump()));
			return *key;
		}

		std::string ResolveOwner(const json& a_args, const ToolContext& a_ctx)
		{
			std::string owner;
			if (a_args.contains("owner")) {
				if (!a_args["owner"].is_string())
					throw ToolError(400, "'owner' must be a string");
				owner = a_args["owner"].get<std::string>();
			} else if (!a_ctx.clientId.empty()) {
				owner = "mcp:" + a_ctx.clientId;
			} else {
				owner = "rest:anonymous";
			}
			if (owner.empty() || owner.size() > 128)
				throw ToolError(400, "'owner' must contain 1..128 characters");
			return owner;
		}

		json KeyJson(const KeyboardKey& a_key)
		{
			return json{ { "key", a_key.name }, { "scancode", a_key.scancode } };
		}

		json ContractJson()
		{
			return json{ { "name", "devbench.input" },
				{ "version", json{ { "major", 2 }, { "minor", 0 } } } };
		}

		struct QueueResult
		{
			bool pending = false;
			int  frame = -1;
		};

		class KeyboardManager
		{
		public:
			static KeyboardManager& Get()
			{
				// Process-lifetime singleton: the bounded watchdog may still reference it
				// during DLL/process teardown, so deliberately do not run a static destructor.
				static auto* instance = new KeyboardManager();
				return *instance;
			}

			void SetEvents(EventBus& a_events)
			{
				std::lock_guard lock(m_mutex);
				m_events = &a_events;
			}

			json Capabilities()
			{
				json keys = json::array();
				for (const auto& key : KeyboardKeyCatalog())
					keys.push_back(KeyJson(key));
				return json{
					{ "contract", ContractJson() },
					{ "capabilities", json{
										  { "keyboard", json{
															{ "version", kKeyboardContractVersion },
															{ "available", true },
															{ "ready", g_inputReady.load(std::memory_order_relaxed) },
															{ "injection", "Skyrim.BSInputEventQueue" },
															{ "encoding", "DirectInputScanCode" },
															{ "actions", json::array({ "status", "down", "up", "tap", "sequence", "releaseAll" }) },
															{ "defaultTapMs", kDefaultTapMs },
															{ "defaultMaxHoldMs", kDefaultMaxHoldMs },
															{ "maximumMaxHoldMs", kMaximumMaxHoldMs },
															{ "maximumHeldKeys", kMaximumHeldKeys },
															{ "maximumSequenceEvents", kMaximumSequenceEvents },
															{ "maximumSequenceMs", kMaximumSequenceMs },
															{ "keys", std::move(keys) },
														} },
										  { "vrTrackedSet", VRInputCapabilities() },
									  } },
				};
			}

			json Status()
			{
				const auto now = NowMs();
				json       held = json::array();
				{
					std::lock_guard lock(m_mutex);
					for (const auto& lease : m_leases.Snapshot()) {
						json item = KeyJson(lease.key);
						item["owner"] = lease.owner;
						item["generation"] = lease.generation;
						item["heldForMs"] = std::max<std::int64_t>(0, now - lease.pressedAtMs);
						item["remainingMs"] = std::max<std::int64_t>(0, lease.expiresAtMs - now);
						held.push_back(std::move(item));
					}
				}
				return json{
					{ "contract", ContractJson() },
					{ "device", "keyboard" },
					{ "ready", g_inputReady.load(std::memory_order_relaxed) },
					{ "held", std::move(held) },
				};
			}

			json Down(const KeyboardKey& a_key, const std::string& a_owner, int a_maxHoldMs,
				bool a_requireFresh = false)
			{
				RequireReady();
				EnsureWatchdog();
				KeyboardAcquireResult acquired;
				QueueResult           queued;
				{
					std::lock_guard lock(m_mutex);
					if (m_leases.Size() >= kMaximumHeldKeys && !m_leases.Find(a_key.scancode))
						throw ToolError(409, std::format("keyboard synthetic hold limit reached ({}) — release a key or call releaseAll", kMaximumHeldKeys));
					acquired = m_leases.Acquire(a_key, a_owner, NowMs(), a_maxHoldMs);
					if (acquired.status == KeyboardAcquireStatus::kConflict)
						throw ToolError(409, std::format("key '{}' is held by owner '{}'", a_key.name, acquired.lease.owner));
					if (acquired.status == KeyboardAcquireStatus::kAlreadyOwned) {
						if (a_requireFresh)
							throw ToolError(409, std::format("key '{}' is already held by this owner; a tap/sequence will not release an earlier hold", a_key.name));
						json result = EventResult("down", acquired.lease, false, false);
						result["alreadyHeld"] = true;
						return result;
					}
					m_latestGeneration.store(acquired.lease.generation, std::memory_order_release);
					try {
						queued = QueueButton(a_key, true, 0.0F);
					} catch (...) {
						m_leases.RemoveExact(a_key.scancode, acquired.lease.generation);
						throw;
					}
				}

				SignalWatchdog();
				Publish("down", acquired.lease, "request", queued.pending);
				return EventResult("down", acquired.lease, queued.pending, false, queued.frame);
			}

			json Up(const KeyboardKey& a_key, const std::string& a_owner, bool a_force = false,
				std::string_view a_reason = "request")
			{
				RequireReady();
				std::lock_guard lock(m_mutex);
				const auto      current = m_leases.Find(a_key.scancode);
				if (!current) {
					json result{ { "contract", ContractJson() }, { "device", "keyboard" },
						{ "action", "up" }, { "released", false }, { "notHeld", true } };
					result.update(KeyJson(a_key));
					return result;
				}
				if (!a_force && current->owner != a_owner)
					throw ToolError(409, std::format("key '{}' is held by owner '{}' (not '{}')", a_key.name, current->owner, a_owner));
				const float       heldSecs = std::max(0.001F,
					static_cast<float>(NowMs() - current->pressedAtMs) / 1000.0F);
				const QueueResult queued = QueueButton(a_key, false, heldSecs);
				m_leases.RemoveExact(a_key.scancode, current->generation);
				SignalWatchdog();
				Publish("up", *current, a_reason, queued.pending);
				return EventResult("up", *current, queued.pending, true, queued.frame);
			}

			json Tap(const KeyboardKey& a_key, const std::string& a_owner, int a_durationMs)
			{
				json down = Down(a_key, a_owner, std::min(kMaximumMaxHoldMs, a_durationMs + 2000), true);
				std::this_thread::sleep_for(milliseconds(a_durationMs));
				json up;
				try {
					up = Up(a_key, a_owner);
				} catch (...) {
					// The bounded lease remains and its watchdog will make a second release attempt.
					throw;
				}
				json result{
					{ "contract", ContractJson() },
					{ "device", "keyboard" },
					{ "action", "tap" },
					{ "durationMs", a_durationMs },
					{ "down", std::move(down) },
					{ "up", std::move(up) },
				};
				result.update(KeyJson(a_key));
				return result;
			}

			json Sequence(const json& a_args, const std::string& a_owner)
			{
				RequireReady();
				if (!a_args.contains("events") || !a_args["events"].is_array())
					throw ToolError(400, "action='sequence' requires an 'events' array");
				const auto& events = a_args["events"];
				if (events.empty() || events.size() > kMaximumSequenceEvents)
					throw ToolError(400, std::format("'events' must contain 1..{} entries", kMaximumSequenceEvents));

				// Validate the complete sequence before injecting anything. Persistent holds belong to
				// action='down'; a sequence is balanced by contract and cannot strand a modifier/key.
				std::unordered_set<std::uint16_t> balanced;
				int                               totalMs = 0;
				for (const auto& event : events) {
					if (!event.is_object())
						throw ToolError(400, "each sequence event must be an object");
					const std::string action = event.value("action", std::string("tap"));
					const int         afterMs = BoundedInteger(event, "afterMs", 0, 0, 10000);
					totalMs += afterMs;
					if (action == "wait") {
						totalMs += BoundedInteger(event, "durationMs", 0, 0, 10000);
						continue;
					}
					const auto key = ParseKey(event);
					if (action == "tap") {
						if (balanced.contains(key.scancode))
							throw ToolError(400, std::format("sequence taps key '{}' while it is held by an earlier sequence down", key.name));
						totalMs += BoundedInteger(event, "durationMs", kDefaultTapMs, 10, 5000);
					} else if (action == "down") {
						if (!balanced.insert(key.scancode).second)
							throw ToolError(400, std::format("sequence presses key '{}' twice without an intervening up", key.name));
					} else if (action == "up") {
						if (!balanced.erase(key.scancode))
							throw ToolError(400, std::format("sequence releases key '{}' without a matching sequence down", key.name));
					} else {
						throw ToolError(400, std::format("unknown sequence event action '{}' (tap|down|up|wait)", action));
					}
				}
				if (!balanced.empty())
					throw ToolError(400, "sequence contains an unbalanced down; add a matching up or use action='down' for a bounded persistent hold");
				if (totalMs > kMaximumSequenceMs)
					throw ToolError(400, std::format("sequence duration {}ms exceeds the {}ms limit", totalMs, kMaximumSequenceMs));

				json                       results = json::array();
				std::vector<KeyboardLease> opened;
				try {
					for (const auto& event : events) {
						const std::string action = event.value("action", std::string("tap"));
						json              result;
						if (action == "wait") {
							const int durationMs = BoundedInteger(event, "durationMs", 0, 0, 10000);
							std::this_thread::sleep_for(milliseconds(durationMs));
							result = json{ { "action", "wait" }, { "durationMs", durationMs } };
						} else {
							const auto key = ParseKey(event);
							if (action == "tap") {
								result = Tap(key, a_owner, BoundedInteger(event, "durationMs", kDefaultTapMs, 10, 5000));
							} else if (action == "down") {
								result = Down(key, a_owner, std::min(kMaximumMaxHoldMs, totalMs + 2000), true);
								opened.push_back(KeyboardLease{ key, a_owner, result.value("generation", 0ull), 0, 0 });
							} else {
								result = Up(key, a_owner);
								std::erase_if(opened, [&](const KeyboardLease& a_lease) { return a_lease.key.scancode == key.scancode; });
							}
						}
						results.push_back(std::move(result));
						const int afterMs = BoundedInteger(event, "afterMs", 0, 0, 10000);
						if (afterMs)
							std::this_thread::sleep_for(milliseconds(afterMs));
					}
				} catch (...) {
					for (const auto& lease : opened) {
						try {
							ReleaseGenerationWithRetry(lease.key.scancode, lease.generation, "sequenceFailure");
						} catch (const std::exception& e) {
							logs::warn("devbench: sequence cleanup could not release '{}' (generation {}): {}",
								lease.key.name, lease.generation, e.what());
						}
					}
					throw;
				}

				return json{
					{ "contract", ContractJson() },
					{ "device", "keyboard" },
					{ "action", "sequence" },
					{ "owner", a_owner },
					{ "durationMs", totalMs },
					{ "eventsRun", results.size() },
					{ "results", std::move(results) },
				};
			}

			json ReleaseAll(const std::string& a_owner, bool a_all, std::string_view a_reason = "request")
			{
				RequireReady();
				std::vector<KeyboardLease> selected;
				{
					std::lock_guard lock(m_mutex);
					for (const auto& lease : m_leases.Snapshot())
						if (a_all || lease.owner == a_owner)
							selected.push_back(lease);
				}
				json released = json::array();
				json failed = json::array();
				for (const auto& lease : selected) {
					try {
						if (ReleaseGenerationWithRetry(lease.key.scancode, lease.generation, a_reason))
							released.push_back(KeyJson(lease.key));
					} catch (const std::exception& e) {
						json item = KeyJson(lease.key);
						item["error"] = e.what();
						failed.push_back(std::move(item));
					}
				}
				return json{
					{ "contract", ContractJson() },
					{ "device", "keyboard" },
					{ "action", "releaseAll" },
					{ "owner", a_all ? "*" : a_owner },
					{ "released", std::move(released) },
					{ "failed", std::move(failed) },
				};
			}

			void RequestLifecycleRelease() noexcept
			{
				const auto cutoff = m_latestGeneration.load(std::memory_order_acquire);
				if (!cutoff)
					return;
				RaiseLifecycleCutoff(cutoff);
				SignalWatchdog();
			}

		private:
			void RequireReady() const
			{
				if (!g_inputReady.load(std::memory_order_relaxed))
					throw ToolError(503, "keyboard input is advertised but not ready — SKSE kInputLoaded has not completed; inspect input capabilities/status and retry");
			}

			QueueResult QueueButton(const KeyboardKey& a_key, bool a_down, float a_heldSecs)
			{
				try {
					const json result = MainThread::RunAndWait([=]() -> json {
						auto* queue = RE::BSInputEventQueue::GetSingleton();
						if (!queue)
							throw ToolError(503, "Skyrim BSInputEventQueue unavailable");
						if (queue->buttonEventCount >= RE::BSInputEventQueue::MAX_BUTTON_EVENTS)
							throw ToolError(503, "Skyrim keyboard input queue is full for this frame — retry after the next frame");
						queue->AddButtonEvent(RE::INPUT_DEVICE::kKeyboard, 0, a_key.scancode,
							a_down ? 1.0F : 0.0F, a_down ? 0.0F : a_heldSecs);
						return json{ { "frame", game::CurrentFrame() } };
					});
					return { false, result.value("frame", -1) };
				} catch (const ToolError& e) {
					// RunAndWait's 504 means the task is already queued and will execute if the main
					// thread resumes. Treat that as accepted/pending so a down retains its lease and
					// watchdog, and an up can safely retire it without a retry racing the queued event.
					if (e.code == 504)
						return { true, -1 };
					throw;
				}
			}

			json EventResult(std::string_view a_action, const KeyboardLease& a_lease,
				bool a_pending, bool a_released, int a_frame = -1) const
			{
				json result{
					{ "contract", ContractJson() },
					{ "device", "keyboard" },
					{ "action", a_action },
					{ "owner", a_lease.owner },
					{ "generation", a_lease.generation },
					{ "accepted", true },
					{ "pending", a_pending },
					{ "released", a_released },
					{ "frame", a_frame },
				};
				result.update(KeyJson(a_lease.key));
				return result;
			}

			void Publish(std::string_view a_action, const KeyboardLease& a_lease,
				std::string_view a_reason, bool a_pending)
			{
				if (!m_events)
					return;
				json payload{
					{ "contractVersion", kKeyboardContractVersion },
					{ "device", "keyboard" },
					{ "action", a_action },
					{ "owner", a_lease.owner },
					{ "generation", a_lease.generation },
					{ "reason", a_reason },
					{ "pending", a_pending },
				};
				payload.update(KeyJson(a_lease.key));
				m_events->Publish("input.keyboard", std::move(payload));
			}

			void EnsureWatchdog()
			{
				std::lock_guard lock(m_mutex);
				if (m_watchdogStarted)
					return;
				// Provision the sole watchdog before any key-down is admitted. Thread
				// construction failure therefore cannot strand an unmonitored lease.
				std::thread([this] { WatchdogLoop(); }).detach();
				m_watchdogStarted = true;
			}

			void WatchdogLoop() noexcept
			{
				for (;;) {
					std::vector<KeyboardLease> selected;
					const auto                 lifecycleCutoff = m_lifecycleCutoff.exchange(0, std::memory_order_acq_rel);
					const bool                 lifecycle = lifecycleCutoff != 0;
					{
						std::unique_lock lock(m_mutex);
						const auto       leases = m_leases.Snapshot();
						if (!lifecycle && leases.empty()) {
							const auto revision = m_watchdogRevision.load(std::memory_order_acquire);
							m_watchdogCv.wait(lock, [&] {
								return m_lifecycleCutoff.load(std::memory_order_acquire) != 0 ||
								       m_watchdogRevision.load(std::memory_order_acquire) != revision;
							});
							continue;
						}
						const auto now = NowMs();
						if (lifecycle) {
							for (const auto& lease : leases)
								if (lease.generation <= lifecycleCutoff)
									selected.push_back(lease);
						} else {
							const auto earliest = std::min_element(leases.begin(), leases.end(),
								[](const auto& a, const auto& b) { return a.expiresAtMs < b.expiresAtMs; });
							if (earliest != leases.end() && earliest->expiresAtMs > now) {
								const auto revision = m_watchdogRevision.load(std::memory_order_acquire);
								m_watchdogCv.wait_for(lock, milliseconds(earliest->expiresAtMs - now), [&] {
									return m_lifecycleCutoff.load(std::memory_order_acquire) != 0 ||
									       m_watchdogRevision.load(std::memory_order_acquire) != revision;
								});
								continue;
							}
							for (const auto& lease : leases)
								if (lease.expiresAtMs <= now)
									selected.push_back(lease);
						}
					}

					bool retryPending = false;
					for (const auto& lease : selected) {
						try {
							ReleaseGenerationWithRetry(lease.key.scancode, lease.generation,
								lifecycle ? "lifecycle" : "leaseExpired");
						} catch (const std::exception& e) {
							retryPending = true;
							if (lifecycle)
								RaiseLifecycleCutoff(lifecycleCutoff);
							logs::warn(
								"devbench: automatic keyboard release remains pending for {} "
								"(generation {}): {}",
								lease.key.name, lease.generation, e.what());
						}
					}
					if (retryPending) {
						std::unique_lock lock(m_mutex);
						m_watchdogCv.wait_for(lock, kWatchdogRetryDelay);
					}
				}
			}

			void SignalWatchdog() noexcept
			{
				m_watchdogRevision.fetch_add(1, std::memory_order_release);
				m_watchdogCv.notify_all();
			}

			void RaiseLifecycleCutoff(std::uint64_t a_cutoff) noexcept
			{
				auto pending = m_lifecycleCutoff.load(std::memory_order_relaxed);
				while (pending < a_cutoff && !m_lifecycleCutoff.compare_exchange_weak(
												 pending, a_cutoff, std::memory_order_release, std::memory_order_relaxed)) {
				}
			}

			bool ReleaseGenerationWithRetry(std::uint16_t a_scancode, std::uint64_t a_generation,
				std::string_view a_reason)
			{
				for (int attempt = 1; attempt <= kReleaseAttempts; ++attempt) {
					try {
						return ReleaseGeneration(a_scancode, a_generation, a_reason);
					} catch (const ToolError& e) {
						if (e.code != 503 || attempt == kReleaseAttempts)
							throw;
						logs::warn("devbench: keyboard release attempt {}/{} deferred: {}",
							attempt, kReleaseAttempts, e.what());
						std::this_thread::sleep_for(kReleaseRetryDelay);
					}
				}
				return false;
			}

			bool ReleaseGeneration(std::uint16_t a_scancode, std::uint64_t a_generation,
				std::string_view a_reason)
			{
				std::lock_guard lock(m_mutex);
				const auto      current = m_leases.Find(a_scancode);
				if (!current || current->generation != a_generation)
					return false;
				const float       heldSecs = std::max(0.001F,
					static_cast<float>(NowMs() - current->pressedAtMs) / 1000.0F);
				const QueueResult queued = QueueButton(current->key, false, heldSecs);
				m_leases.RemoveExact(a_scancode, a_generation);
				Publish("up", *current, a_reason, queued.pending);
				return true;
			}

			std::mutex                 m_mutex;
			std::condition_variable    m_watchdogCv;
			KeyboardLeaseTable         m_leases;
			EventBus*                  m_events = nullptr;
			bool                       m_watchdogStarted = false;
			std::atomic<std::uint64_t> m_latestGeneration{ 0 };
			std::atomic<std::uint64_t> m_lifecycleCutoff{ 0 };
			std::atomic<std::uint64_t> m_watchdogRevision{ 0 };
		};

		json HandleInputImpl(const json& a_args, const ToolContext& a_ctx)
		{
			if (!a_args.is_object())
				throw ToolError(400, "input arguments must be an object");
			const std::string action = a_args.value("action", std::string("capabilities"));
			if (action == "capabilities")
				return KeyboardManager::Get().Capabilities();
			if (a_args.contains("device") && !a_args["device"].is_string())
				throw ToolError(400, "'device' must be a string");
			const std::string device = a_args.value("device", std::string("keyboard"));
			if (IsVRInputDevice(device))
				return HandleVRInput(a_args, a_ctx);
			if (device != "keyboard")
				throw ToolError(400, "input contract v2 supports device='keyboard' or device='vrTrackedSet'");
			if (action == "status")
				return KeyboardManager::Get().Status();

			const std::string owner = ResolveOwner(a_args, a_ctx);
			if (action == "down")
				return KeyboardManager::Get().Down(ParseKey(a_args), owner,
					BoundedInteger(a_args, "maxHoldMs", kDefaultMaxHoldMs, 100, kMaximumMaxHoldMs));
			if (action == "up")
				return KeyboardManager::Get().Up(ParseKey(a_args), owner,
					a_ctx.internal && BooleanArgument(a_args, "force", false));
			if (action == "tap")
				return KeyboardManager::Get().Tap(ParseKey(a_args), owner,
					BoundedInteger(a_args, "durationMs", kDefaultTapMs, 10, 5000));
			if (action == "sequence")
				return KeyboardManager::Get().Sequence(a_args, owner);
			if (action == "releaseAll")
				return KeyboardManager::Get().ReleaseAll(owner,
					a_ctx.internal && BooleanArgument(a_args, "all", false));
			throw ToolError(400, std::format("unknown input action '{}' (capabilities|status|down|up|tap|sequence|releaseAll)", action));
		}

		json HandleInput(const json& a_args, const ToolContext& a_ctx)
		{
			try {
				if (!a_ctx.internal && a_args.contains("force") && BooleanArgument(a_args, "force", false))
					throw ToolError(403, "cross-owner keyboard release is reserved for internal cleanup");
				if (!a_ctx.internal && a_args.contains("all") && BooleanArgument(a_args, "all", false))
					throw ToolError(403, "all-owner keyboard cleanup is reserved for internal lifecycle/replay paths");
				return HandleInputImpl(a_args, a_ctx);
			} catch (const json::exception& e) {
				throw ToolError(400, std::format("invalid keyboard input JSON: {}", e.what()));
			}
		}
	}

	void RegisterInputTool(ToolRegistry& a_registry, EventBus& a_events)
	{
		KeyboardManager::Get().SetEvents(a_events);
		ToolDescriptor input;
		input.name = "input";
		input.description =
			"Versioned synthetic input interface. action='capabilities' (default) returns the exact "
			"contract/version, readiness, limits, injection path, supported actions, and complete "
			"keyboard name→DirectInput-scan-code catalog; clients MUST capability-negotiate rather "
			"than assuming this tool exists. Contract v1 implements device='keyboard' using Skyrim's "
			"own BSInputEventQueue (not Windows SendInput, so window focus is irrelevant). 'status' "
			"reports readiness and every synthetic held key with owner/lease timing. 'down' starts a "
			"bounded owned hold (default maxHoldMs 5000; automatic up on expiry); repeated down by the "
			"same owner is idempotent and another owner gets 409. 'up' releases that owner's key; "
			"cross-owner force is reserved for internal cleanup. Releasing an unheld key is an idempotent "
			"no-op. 'tap' emits down, waits durationMs (default 50), then up. 'sequence' executes a "
			"prevalidated balanced array of tap/down/up/wait events (optional afterMs); it rejects an "
			"unbalanced hold before injecting anything and releases keys acquired by the sequence on "
			"failure. 'releaseAll' releases this owner only; all-owner cleanup is internal-only. Every "
			"lease watchdog retries a failed automatic release until it succeeds or becomes obsolete. "
			"owner defaults to the MCP session id or rest:anonymous; automation should "
			"supply a stable task owner. DevBench also releases owned keys on load/new-game and emits "
			"input.keyboard events for accepted down/up transitions. Contract v2 also implements "
			"device='vrTrackedSet': read-only observe returns the current physical OpenVR HMD and both controllers; "
			"one prevalidated timestamped sequence whose every frame atomically "
			"contains HMD, left-controller, and right-controller pose plus both controllers' complete "
			"OpenVR button/touch/axis state. It is applied at the OpenVR compositor/system boundary, is "
			"always asynchronous, has one owner, passes the real runtime through when inactive, and "
			"returns a controlToken required for external stop. Failed controller-index restoration remains "
			"retryable through that token. Ad-hoc sequences stop on lifecycle boundaries; recording replay "
			"may explicitly survive the load/new-game events it is reproducing. There are deliberately no per-device or "
			"per-button VR mutation actions: submit the coherent tracked set so poses and controls cannot "
			"come from different clocks.";
		input.inputSchema = json{
			{ "type", "object" },
			{ "properties", json{
								{ "action", json{ { "type", "string" }, { "enum", json::array({ "capabilities", "status", "down", "up", "tap", "sequence", "stop", "releaseAll" }) } } },
								{ "device", json{ { "type", "string" }, { "enum", json::array({ "keyboard", "vrTrackedSet" }) }, { "description", "mutation/status device; omit for capabilities" } } },
								{ "key", json{ { "oneOf", json::array({ json{ { "type", "string" } }, json{ { "type", "integer" }, { "minimum", 1 }, { "maximum", 255 } } }) }, { "description", "down/up/tap: documented key name or raw DirectInput scancode" } } },
								{ "owner", json{ { "type", "string" }, { "minLength", 1 }, { "maxLength", 128 }, { "description", "stable task/session owner; defaults to MCP session id or rest:anonymous" } } },
								{ "durationMs", json{ { "type", "integer" }, { "minimum", 10 }, { "maximum", 5000 }, { "description", "tap duration (default 50); sequence tap/wait event duration" } } },
								{ "maxHoldMs", json{ { "type", "integer" }, { "minimum", 100 }, { "maximum", kMaximumMaxHoldMs }, { "description", "down safety lease (default 5000); automatic up at expiry" } } },
								{ "force", json{ { "type", "boolean" }, { "description", "internal cleanup only; external true is rejected" } } },
								{ "all", json{ { "type", "boolean" }, { "description", "internal cleanup only; external true is rejected" } } },
								{ "events", json{ { "type", "array" }, { "minItems", 1 }, { "maxItems", kMaximumSequenceEvents }, { "description", "sequence: balanced [{action:tap|down|up|wait,key?,durationMs?,afterMs?}]" }, { "items", json{ { "type", "object" } } } } },
								{ "frames", json{ { "type", "array" }, { "minItems", 1 }, { "maxItems", kMaximumVRTrackedFrames }, { "description", "vrTrackedSet sequence: monotonic atomic frames; each requires tMs, originCode, hmd, left, right; controller objects carry packetNumber/pressed/touched/five axes" }, { "items", json{ { "type", "object" } } } } },
								{ "tailMs", json{ { "type", "integer" }, { "minimum", 10 }, { "maximum", 1000 }, { "description", "vrTrackedSet sequence: final-state visibility before automatic release (default 50ms)" } } },
								{ "controlToken", json{ { "type", "string" }, { "description", "vrTrackedSet stop/releaseAll: opaque token returned by sequence start" } } },
								{ "surviveLifecycle", json{ { "type", "boolean" }, { "description", "internal recording replay only; external true is rejected" } } },
							} },
		};
		a_registry.Register(std::move(input), &HandleInput);
	}

	void MarkKeyboardInputReady()
	{
		g_inputReady.store(true, std::memory_order_relaxed);
		logs::info("devbench: keyboard input API ready (contract v{}, Skyrim BSInputEventQueue)", kKeyboardContractVersion);
	}

	void ReleaseKeyboardInputForLifecycle(const char* a_reason)
	{
		if (!g_inputReady.load(std::memory_order_relaxed))
			return;
		(void)a_reason;
		// The lifecycle callback cannot take the manager mutex while another request is
		// waiting on this main thread. The already-provisioned bounded watchdog owns cleanup.
		KeyboardManager::Get().RequestLifecycleRelease();
	}
}
