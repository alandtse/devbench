#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "Json.h"

namespace dvb
{
	/// Fan-out for asynchronous notifications (e.g. "shader recompiled"). The MCP
	/// adapter subscribes and forwards as MCP notifications; the REST adapter serves
	/// the recent ring via `GET /api/events?since=N` (poll) or an SSE stream.
	///
	/// Thread-safe. CRITICAL: Publish only appends to the ring + a delivery queue and
	/// returns — it NEVER calls subscribers on the caller's thread. A dedicated worker
	/// thread delivers to subscribers. This is load-bearing: events are published from
	/// the GAME'S MAIN THREAD (console hook, cell-load sink, shader-recompile), and a
	/// subscriber may block (the MCP adapter does a synchronous socket write to connected
	/// clients). Delivering inline would let a slow/half-open client hang the main thread
	/// and freeze the game (observed live). Off-thread delivery confines any block to the
	/// worker.
	class EventBus
	{
	public:
		struct Event
		{
			uint64_t    seq = 0;     ///< monotonic, assigned on publish
			int         frame = -1;  ///< game frame at publish (-1 if no provider / unresolved)
			std::string topic;
			json        payload;
		};

		EventBus()
		{
			m_worker = std::thread([this] { DeliverLoop(); });
		}

		~EventBus()
		{
			{
				std::lock_guard lock(m_mutex);
				m_stop = true;
			}
			m_cv.notify_all();
			if (m_worker.joinable())
				m_worker.join();
		}

		EventBus(const EventBus&) = delete;
		EventBus& operator=(const EventBus&) = delete;
		EventBus(EventBus&&) = delete;
		EventBus& operator=(EventBus&&) = delete;

		/// Stamp each published event with the current game frame (for syncing events to a
		/// Tracy/CS capture). Optional — without a provider, Event::frame stays -1.
		void SetFrameProvider(std::function<int()> a_fn)
		{
			std::lock_guard lock(m_mutex);
			m_frameProvider = std::move(a_fn);
		}

		using Subscriber = std::function<void(const Event&)>;
		using SubId = uint64_t;

		/// Subscribe to all topics. Returns an id for Unsubscribe. Callbacks run on the bus
		/// worker thread (NOT the publisher's), so a blocking callback can't stall a publisher.
		SubId Subscribe(Subscriber a_fn)
		{
			std::lock_guard lock(m_mutex);
			const SubId     id = ++m_nextSub;
			m_subs.emplace_back(id, std::move(a_fn));
			return id;
		}

		/// Remove a subscriber. After this returns the callback is guaranteed not to run again,
		/// so the caller may safely destroy whatever the callback captured — it waits out any
		/// in-flight delivery batch (the worker holds m_deliverMutex across a batch).
		void Unsubscribe(SubId a_id)
		{
			{
				std::lock_guard lock(m_mutex);
				std::erase_if(m_subs, [&](const auto& s) { return s.first == a_id; });
			}
			std::lock_guard deliver(m_deliverMutex);  // barrier: no callback for a_id runs past here
		}

		/// Append to the recent ring and queue for delivery. Returns immediately — subscribers
		/// are notified later on the worker thread. Safe to call from the main thread.
		void Publish(std::string_view a_topic, json a_payload)
		{
			Event ev;
			{
				std::lock_guard lock(m_mutex);
				ev.seq = ++m_seq;
				if (m_frameProvider)
					ev.frame = m_frameProvider();
				ev.topic.assign(a_topic);
				ev.payload = std::move(a_payload);
				m_recent.push_back(ev);
				while (m_recent.size() > kMaxRecent)
					m_recent.erase(m_recent.begin());
				m_deliverQueue.push_back(ev);  // hand off; worker delivers
			}
			m_cv.notify_one();
		}

		/// Events with seq strictly greater than `a_since` (for poll-style readers).
		std::vector<Event> Since(uint64_t a_since) const
		{
			std::lock_guard    lock(m_mutex);
			std::vector<Event> out;
			for (const auto& e : m_recent)
				if (e.seq > a_since)
					out.push_back(e);
			return out;
		}

		uint64_t HeadSeq() const
		{
			std::lock_guard lock(m_mutex);
			return m_seq;
		}

	private:
		static constexpr size_t kMaxRecent = 256;  // bounded; oldest dropped first

		// Drains the delivery queue and notifies subscribers off the publisher's thread.
		void DeliverLoop()
		{
			for (;;) {
				std::deque<Event>       batch;
				std::vector<Subscriber> subs;
				{
					std::unique_lock lock(m_mutex);
					m_cv.wait(lock, [this] { return m_stop || !m_deliverQueue.empty(); });
					if (m_stop && m_deliverQueue.empty())
						return;
					batch.swap(m_deliverQueue);
					subs.reserve(m_subs.size());
					for (auto& s : m_subs)
						subs.push_back(s.second);
				}
				// Outside m_mutex (so Publish never blocks) but under m_deliverMutex, so
				// Unsubscribe can barrier on a batch before its caller destroys the subscriber.
				// A subscriber may block here (e.g., a socket write) — that's the whole point of
				// delivering off the publisher's thread.
				std::lock_guard deliver(m_deliverMutex);
				for (const auto& ev : batch)
					for (const auto& fn : subs)
						fn(ev);
			}
		}

		mutable std::mutex                        m_mutex;
		uint64_t                                  m_seq = 0;
		SubId                                     m_nextSub = 0;
		std::vector<Event>                        m_recent;
		std::vector<std::pair<SubId, Subscriber>> m_subs;
		std::function<int()>                      m_frameProvider;

		std::deque<Event>       m_deliverQueue;
		std::condition_variable m_cv;
		std::thread             m_worker;
		bool                    m_stop = false;
		std::mutex              m_deliverMutex;  // held across a notify batch; Unsubscribe barriers on it
	};
}
