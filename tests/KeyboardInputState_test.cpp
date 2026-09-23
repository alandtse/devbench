#include "test_framework.h"

#include "KeyboardInputState.h"

using dvb::KeyboardAcquireStatus;
using dvb::KeyboardKey;
using dvb::KeyboardLeaseTable;
using dvb::ResolveKeyboardKey;

TEST_CASE("keyboard aliases normalize case and separators to DirectInput scancodes")
{
	const auto pageUp = ResolveKeyboardKey("Page_Up");
	const auto arrow = ResolveKeyboardKey("arrow-up");
	const auto enter = ResolveKeyboardKey("RETURN");
	const auto minus = ResolveKeyboardKey("-");
	CHECK(pageUp.has_value());
	CHECK(pageUp->scancode == 0xC9);
	CHECK(pageUp->name == "pageUp");
	CHECK(arrow.has_value());
	CHECK(arrow->scancode == 0xC8);
	CHECK(enter.has_value());
	CHECK(enter->scancode == 0x1C);
	CHECK(minus.has_value());
	CHECK(minus->scancode == 0x0C);
	CHECK(minus->name == "minus");
}

TEST_CASE("raw DirectInput scancodes preserve unknown but valid keys")
{
	const auto known = ResolveKeyboardKey(0x11);
	const auto unknown = ResolveKeyboardKey(0xAA);
	CHECK(known.has_value());
	CHECK(known->name == "w");
	CHECK(unknown.has_value());
	CHECK(unknown->name == "scancode-170");
	CHECK(!ResolveKeyboardKey(0).has_value());
	CHECK(!ResolveKeyboardKey(256).has_value());
}

TEST_CASE("keyboard leases are idempotent for their owner and reject another owner")
{
	KeyboardLeaseTable leases;
	const KeyboardKey  w = *ResolveKeyboardKey("w");
	const auto         first = leases.Acquire(w, "task-a", 1000, 5000, 1000);
	const auto         retry = leases.Acquire(w, "task-a", 2000, 5000, 2000);
	const auto         conflict = leases.Acquire(w, "task-b", 2000, 5000, 2000);
	CHECK(first.status == KeyboardAcquireStatus::kAcquired);
	CHECK(retry.status == KeyboardAcquireStatus::kAlreadyOwned);
	CHECK(retry.lease.generation == first.lease.generation);
	CHECK(retry.lease.expiresAtMs == first.lease.expiresAtMs);
	CHECK(conflict.status == KeyboardAcquireStatus::kConflict);
	CHECK(conflict.lease.owner == "task-a");
	CHECK(leases.Size() == 1);
}

TEST_CASE("keyboard lease release enforces owner unless explicitly forced")
{
	KeyboardLeaseTable leases;
	const KeyboardKey  key = *ResolveKeyboardKey("enter");
	leases.Acquire(key, "task-a", 100, 5000, 100);
	CHECK(!leases.Remove(key.scancode, "task-b").has_value());
	CHECK(leases.Size() == 1);
	const auto forced = leases.Remove(key.scancode, "task-b", true);
	CHECK(forced.has_value());
	CHECK(forced->owner == "task-a");
	CHECK(leases.Size() == 0);
}

TEST_CASE("generation-qualified cleanup cannot release a newer hold")
{
	KeyboardLeaseTable leases;
	const KeyboardKey  key = *ResolveKeyboardKey("space");
	const auto         oldLease = leases.Acquire(key, "task-a", 100, 5000, 100).lease;
	CHECK(leases.RemoveExact(key.scancode, oldLease.generation).has_value());
	const auto newLease = leases.Acquire(key, "task-a", 200, 5000, 200).lease;
	CHECK(newLease.generation != oldLease.generation);
	CHECK(!leases.RemoveExact(key.scancode, oldLease.generation).has_value());
	CHECK(leases.Find(key.scancode)->generation == newLease.generation);
}

TEST_CASE("release all can target one owner without touching another")
{
	KeyboardLeaseTable leases;
	leases.Acquire(*ResolveKeyboardKey("w"), "task-a", 0, 1000, 0);
	leases.Acquire(*ResolveKeyboardKey("leftShift"), "task-a", 0, 1000, 0);
	leases.Acquire(*ResolveKeyboardKey("enter"), "task-b", 0, 1000, 0);
	const auto released = leases.RemoveAll("task-a");
	CHECK(released.size() == 2);
	CHECK(leases.Size() == 1);
	CHECK(leases.Find(0x1C)->owner == "task-b");
	CHECK(leases.RemoveAll().size() == 1);
	CHECK(leases.Size() == 0);
}

TEST_CASE("a lease carries the game-time press alongside the wall-clock one")
{
	KeyboardLeaseTable leases;
	const KeyboardKey  w = *ResolveKeyboardKey("w");
	const auto         lease = leases.Acquire(w, "task-a", 1000, 5000, 42).lease;
	CHECK(lease.pressedAtMs == 1000);
	CHECK(lease.pressedAtGameMs == 42);
	CHECK(lease.expiresAtMs == 6000);  // the safety bound stays on the wall clock
}

TEST_CASE("held seconds are game seconds, so a hold keeps its meaning at any time scale")
{
	using dvb::HeldDownSeconds;
	constexpr std::int64_t pressedAtGameMs = 5000;

	CHECK(HeldDownSeconds(pressedAtGameMs, pressedAtGameMs) == 0.001F);
	CHECK(HeldDownSeconds(pressedAtGameMs, pressedAtGameMs + 2000) == 2.0F);
	// The same 2000 ms of wall-clock holding at scale 3 is 6000 ms of play.
	CHECK(HeldDownSeconds(pressedAtGameMs, pressedAtGameMs + 6000) == 6.0F);
}
