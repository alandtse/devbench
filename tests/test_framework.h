#pragma once

// Minimal self-contained test harness. Deliberately dependency-free: the rest of
// devbench's deps are pinned in xmake-requires.lock, and adding a unit-test
// framework (doctest/Catch/gtest) would mean lock churn + a network fetch for a
// handful of host-independent cases. If the suite outgrows this, swapping in
// doctest is a drop-in replacement (same TEST_CASE / CHECK surface).

#include <cstdio>
#include <exception>
#include <functional>
#include <string>
#include <vector>

namespace tf
{
	struct Case
	{
		const char*           name;
		std::function<void()> fn;
	};

	inline std::vector<Case>& cases()
	{
		static std::vector<Case> c;
		return c;
	}

	inline int& failures()
	{
		static int f = 0;
		return f;
	}

	inline void fail(const char* a_file, int a_line, const std::string& a_msg)
	{
		++failures();
		std::printf("    FAIL %s:%d: %s\n", a_file, a_line, a_msg.c_str());
	}

	struct Registrar
	{
		Registrar(const char* a_name, std::function<void()> a_fn) { cases().push_back({ a_name, std::move(a_fn) }); }
	};

	/// Run every registered case; print a per-case PASS/FAIL line and a summary.
	/// Returns a process exit code: 0 = all passed, 1 = at least one failure.
	inline int run()
	{
		int failed = 0;
		for (auto& c : cases()) {
			const int before = failures();
			try {
				c.fn();
			} catch (const std::exception& e) {
				fail(__FILE__, __LINE__, std::string("unexpected exception: ") + e.what());
			} catch (...) {
				fail(__FILE__, __LINE__, "unexpected non-std exception");
			}
			const bool ok = failures() == before;
			std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", c.name);
			if (!ok)
				++failed;
		}
		std::printf("\n%zu case(s), %d failed\n", cases().size(), failed);
		return failed == 0 ? 0 : 1;
	}
}

#define TF_CONCAT_INNER(a, b) a##b
#define TF_CONCAT(a, b) TF_CONCAT_INNER(a, b)

/// Define and auto-register a test case: `TEST_CASE("name") { ... }`.
#define TEST_CASE(name)                                     \
	static void            TF_CONCAT(tf_case_, __LINE__)(); \
	static ::tf::Registrar TF_CONCAT(tf_reg_, __LINE__)(    \
		name, &TF_CONCAT(tf_case_, __LINE__));              \
	static void TF_CONCAT(tf_case_, __LINE__)()

#define CHECK(cond)                                                 \
	do {                                                            \
		if (!(cond))                                                \
			::tf::fail(__FILE__, __LINE__, "CHECK failed: " #cond); \
	} while (0)

#define CHECK_MESSAGE(cond, msg)                   \
	do {                                           \
		if (!(cond))                               \
			::tf::fail(__FILE__, __LINE__, (msg)); \
	} while (0)

#define CHECK_THROWS(expr)                                                     \
	do {                                                                       \
		bool tf_threw = false;                                                 \
		try {                                                                  \
			(void)(expr);                                                      \
		} catch (...) {                                                        \
			tf_threw = true;                                                   \
		}                                                                      \
		if (!tf_threw)                                                         \
			::tf::fail(__FILE__, __LINE__, "expected exception from: " #expr); \
	} while (0)

#define CHECK_NOTHROW(expr)                                                      \
	do {                                                                         \
		try {                                                                    \
			(void)(expr);                                                        \
		} catch (...) {                                                          \
			::tf::fail(__FILE__, __LINE__, "unexpected exception from: " #expr); \
		}                                                                        \
	} while (0)
