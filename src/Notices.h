#pragma once

#include "Json.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace dvb::Notices
{
	enum class Severity
	{
		kInfo,
		kWarning,
		kCritical
	};

	struct Notice
	{
		std::string key;     ///< stable id; posting the same key again replaces the notice
		std::string source;  ///< who posted it (a plugin or tool name)
		std::string text;
		Severity    severity = Severity::kInfo;
		long long   expiresMs = 0;  ///< 0 = until cleared
	};

	constexpr std::size_t   kMaxNotices = 32;
	constexpr std::size_t   kMaxKeyBytes = 64;
	constexpr std::size_t   kMaxSourceBytes = 64;
	constexpr std::size_t   kMaxTextBytes = 500;
	constexpr std::uint32_t kMaxTtlMs = 24 * 60 * 60 * 1000;

	/// "info" | "warning" | "critical"; ParseSeverity returns false (a_out untouched) for any other name.
	[[nodiscard]] const char* SeverityName(Severity a_severity);
	[[nodiscard]] bool        ParseSeverity(const std::string& a_name, Severity& a_out);

	/// Notices posted by registered tools or plugins that every tool reply carries until they are
	/// cleared or expire, so an agent learns of a critical condition on its next call. Thread-safe.
	class Board
	{
	public:
		/// Adds or replaces the notice with a_key. a_ttlMs 0 keeps it until Clear. At the cap the
		/// oldest notice is evicted, so a flood of keys can never grow the board or hide a newer one.
		void Post(std::string a_key, std::string a_source, Severity a_severity, std::string a_text, std::uint32_t a_ttlMs, long long a_nowMs);

		/// True if a notice with a_key existed.
		bool Clear(const std::string& a_key);

		/// Unexpired notices, most severe first, then oldest first.
		[[nodiscard]] std::vector<Notice> Active(long long a_nowMs);

	private:
		std::mutex          m_mutex;
		std::vector<Notice> m_notices;
	};

	/// The process-wide board, and the monotonic clock it is read against.
	Board&                  Global();
	[[nodiscard]] long long NowMs();

	[[nodiscard]] json ToJson(const std::vector<Notice>& a_notices);
}
