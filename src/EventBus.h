#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "Json.h"

namespace dvb
{
	/// Fan-out for asynchronous notifications (e.g. "shader recompiled"). The MCP
	/// adapter subscribes and forwards as MCP notifications; the REST adapter serves
	/// the recent ring via `GET /api/events?since=N` (poll) or an SSE stream.
	///
	/// Thread-safe. Publishers run on whatever thread produced the event; subscriber
	/// callbacks are invoked synchronously under no lock other than their own.
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

		/// Stamp each published event with the current game frame (for syncing events to a
		/// Tracy/CS capture). Optional — without a provider, Event::frame stays -1.
		void SetFrameProvider(std::function<int()> a_fn)
		{
			std::lock_guard lock(m_mutex);
			m_frameProvider = std::move(a_fn);
		}

		using Subscriber = std::function<void(const Event&)>;
		using SubId = uint64_t;

		/// Subscribe to all topics. Returns an id for Unsubscribe.
		SubId Subscribe(Subscriber a_fn)
		{
			std::lock_guard lock(m_mutex);
			const SubId     id = ++m_nextSub;
			m_subs.emplace_back(id, std::move(a_fn));
			return id;
		}

		void Unsubscribe(SubId a_id)
		{
			std::lock_guard lock(m_mutex);
			std::erase_if(m_subs, [&](const auto& s) { return s.first == a_id; });
		}

		/// Append to the recent ring and notify subscribers.
		void Publish(std::string_view a_topic, json a_payload)
		{
			Event                   ev;
			std::vector<Subscriber> targets;
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
				targets.reserve(m_subs.size());
				for (auto& s : m_subs)
					targets.push_back(s.second);
			}
			// Notify outside the lock so subscribers can't deadlock against Publish.
			for (auto& fn : targets)
				fn(ev);
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

		mutable std::mutex                        m_mutex;
		uint64_t                                  m_seq = 0;
		SubId                                     m_nextSub = 0;
		std::vector<Event>                        m_recent;
		std::vector<std::pair<SubId, Subscriber>> m_subs;
		std::function<int()>                      m_frameProvider;
	};
}
