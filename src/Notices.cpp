#include "Notices.h"

#include <algorithm>
#include <chrono>

namespace dvb::Notices
{
	namespace
	{
		std::string Truncate(const std::string& a_text, std::size_t a_max)
		{
			if (a_text.size() <= a_max)
				return a_text;
			std::size_t end = a_max;
			while (end > 0 && (static_cast<unsigned char>(a_text[end]) & 0xC0) == 0x80)
				--end;
			return a_text.substr(0, end);
		}
	}

	const char* SeverityName(Severity a_severity)
	{
		switch (a_severity) {
		case Severity::kCritical:
			return "critical";
		case Severity::kWarning:
			return "warning";
		default:
			return "info";
		}
	}

	bool ParseSeverity(const std::string& a_name, Severity& a_out)
	{
		if (a_name == "info")
			a_out = Severity::kInfo;
		else if (a_name == "warning")
			a_out = Severity::kWarning;
		else if (a_name == "critical")
			a_out = Severity::kCritical;
		else
			return false;
		return true;
	}

	void Board::Post(std::string a_key, std::string a_source, Severity a_severity, std::string a_text, std::uint32_t a_ttlMs, long long a_nowMs)
	{
		Notice n;
		n.key = Truncate(a_key, kMaxKeyBytes);
		n.source = Truncate(a_source, kMaxSourceBytes);
		n.text = Truncate(a_text, kMaxTextBytes);
		n.severity = a_severity;
		n.expiresMs = a_ttlMs ? a_nowMs + static_cast<long long>(a_ttlMs) : 0;

		std::lock_guard lock(m_mutex);
		std::erase_if(m_notices, [&](const Notice& x) { return x.key == n.key || (x.expiresMs && x.expiresMs <= a_nowMs); });
		if (m_notices.size() >= kMaxNotices)
			m_notices.erase(m_notices.begin());
		m_notices.push_back(std::move(n));
	}

	bool Board::Clear(const std::string& a_key)
	{
		const std::string key = Truncate(a_key, kMaxKeyBytes);
		std::lock_guard   lock(m_mutex);
		return std::erase_if(m_notices, [&](const Notice& x) { return x.key == key; }) > 0;
	}

	std::vector<Notice> Board::Active(long long a_nowMs)
	{
		std::lock_guard lock(m_mutex);
		std::erase_if(m_notices, [&](const Notice& x) { return x.expiresMs && x.expiresMs <= a_nowMs; });
		std::vector<Notice> out = m_notices;
		std::stable_sort(out.begin(), out.end(), [](const Notice& a, const Notice& b) { return a.severity > b.severity; });
		return out;
	}

	Board& Global()
	{
		static Board board;
		return board;
	}

	long long NowMs()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	json ToJson(const std::vector<Notice>& a_notices)
	{
		json out = json::array();
		for (const auto& n : a_notices)
			out.push_back(json{ { "key", n.key }, { "source", n.source }, { "severity", SeverityName(n.severity) }, { "text", n.text } });
		return out;
	}
}
